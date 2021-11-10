/*
 * Copyright (c) 2020, NVIDIA CORPORATION. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

/**
 * Execution command:
 * ffmpeg -c:v h264_nvv4l2 -i ../h264_1080p_high_30mbps_30fps.mp4 -f null -
 * ffmpeg -c:v h264_nvv4l2 -i ../h264_1080p_high_30mbps_30fps.h264 -c:v rawvideo -pix_fmt yuv420p out.yuv
 **/

#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include "decode.h"
#include "internal.h"
#include "libavutil/buffer.h"
#include "libavutil/common.h"
#include "libavutil/frame.h"
#include "libavutil/hwcontext.h"
#include "libavutil/hwcontext_drm.h"
#include "libavutil/imgutils.h"
#include "libavutil/log.h"
#include "avcodec.h"
#include <stdint.h>
#include <unistd.h>
#include <libv4l2.h>
#include <linux/videodev2.h>
#include <malloc.h>
#include <pthread.h>
#include <string.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <errno.h>
#include <assert.h>

#include "nvbuf_utils.h"
#include "v4l2_nv_extensions.h"

#include "nvv4l2_dec.h"

static void push(AVCodecContext * avctx, queues * q, uint32_t val)
{
    if (q->capacity < MAX_BUFFERS) {
        q->back = (q->back + 1) % MAX_BUFFERS;
        q->data[q->back] = val;
        q->capacity++;
    } else {
        av_log(avctx, AV_LOG_ERROR, "Queue already full!!!\n");
        return;
    }
    return;
}

static void pop(AVCodecContext * avctx, queues * q)
{
    if (q->capacity != 0) {
        q->data[q->front] = -1;
        q->front = (q->front + 1) % MAX_BUFFERS;
        q->capacity--;
    } else {
        av_log(avctx, AV_LOG_ERROR, "Queue already empty");
        return;
    }
    return;
}

int create_bufferfmt(Buffer * buffer, enum v4l2_buf_type buf_type,
                     enum v4l2_memory memory_type, uint32_t n_planes,
                     BufferPlaneFormat * fmt, uint32_t index)
{
    uint32_t i;
    buffer->mapped = false;
    buffer->buf_type = buf_type;
    buffer->memory_type = memory_type;
    buffer->index = index;
    buffer->n_planes = n_planes;

    memset(buffer->planes, 0, sizeof(BufferPlane));
    for (i = 0; i < buffer->n_planes; i++) {
        buffer->planes[i].fd = -1;
        buffer->planes[i].fmt = fmt[i];
    }
    return 0;
}

void destroyBuffer(AVCodecContext * avctx, Buffer * buffer)
{
    if (buffer->mapped) {
        unmap(avctx, buffer);
    }
}

int allocateMemory(AVCodecContext * avctx, Buffer * buffer)
{
    uint32_t j;

    for (j = 0; j < buffer->n_planes; j++) {
        buffer->planes[j].length =
            (buffer->planes[j].fmt.sizeimage >
             buffer->planes[j].fmt.width *
             buffer->planes[j].fmt.bytesperpixel *
             buffer->planes[j].fmt.height) ? buffer->planes[j].
            fmt.sizeimage : buffer->planes[j].fmt.width *
            buffer->planes[j].fmt.bytesperpixel *
            buffer->planes[j].fmt.height;

        buffer->planes[j].data =
            (unsigned char *) malloc(sizeof(unsigned char) *
                                     buffer->planes[j].length);
        if (buffer->planes[j].data == MAP_FAILED) {
            av_log(avctx, AV_LOG_ERROR,
                   "Could not map buffer %d plane %d \n", buffer->index,
                   j);
            return -1;
        }
    }
    return 0;
}

int map(AVCodecContext * avctx, Buffer * buffer)
{
    uint32_t j;
    if (buffer->memory_type != V4L2_MEMORY_MMAP) {
        av_log(avctx, AV_LOG_ERROR, "Buffer %d already mapped \n",
               buffer->index);
        return -1;
    }

    if (buffer->mapped) {
        av_log(avctx, AV_LOG_VERBOSE, "Buffer %d already mapped \n",
               buffer->index);
        return 0;
    }

    for (j = 0; j < buffer->n_planes; j++) {
        if (buffer->planes[j].fd == -1) {
            return -1;
        }

        buffer->planes[j].data = (unsigned char *) mmap(NULL,
                                                        buffer->
                                                        planes[j].length,
                                                        PROT_READ |
                                                        PROT_WRITE,
                                                        MAP_SHARED,
                                                        buffer->
                                                        planes[j].fd,
                                                        buffer->
                                                        planes
                                                        [j].mem_offset);
        if (buffer->planes[j].data == MAP_FAILED) {
            av_log(avctx, AV_LOG_ERROR,
                   "Could not map buffer %d plane %d \n", buffer->index,
                   j);
            return -1;
        }
    }
    buffer->mapped = true;
    return 0;
}

void unmap(AVCodecContext * avctx, Buffer * buffer)
{
    if (buffer->memory_type != V4L2_MEMORY_MMAP || !buffer->mapped) {
        av_log(avctx, AV_LOG_VERBOSE,
               "Cannot Unmap Buffer %d Only mapped MMAP buffer can be unmapped\n",
               buffer->index);
        return;
    }

    for (uint32_t j = 0; j < buffer->n_planes; j++) {
        if (buffer->planes[j].data) {
            munmap(buffer->planes[j].data, buffer->planes[j].length);
        }
        buffer->planes[j].data = NULL;
    }
    buffer->mapped = false;
}

int fill_buffer_plane_format(AVCodecContext * avctx, uint32_t * num_planes,
                             BufferPlaneFormat * planefmts,
                             uint32_t width, uint32_t height,
                             uint32_t raw_pixfmt)
{
    switch (raw_pixfmt) {
    case V4L2_PIX_FMT_YUV420M:
        *num_planes = 3;

        planefmts[0].width = width;
        planefmts[1].width = width / 2;
        planefmts[2].width = width / 2;

        planefmts[0].height = height;
        planefmts[1].height = height / 2;
        planefmts[2].height = height / 2;

        planefmts[0].bytesperpixel = 1;
        planefmts[1].bytesperpixel = 1;
        planefmts[2].bytesperpixel = 1;
        break;
    case V4L2_PIX_FMT_NV12M:
        *num_planes = 2;

        planefmts[0].width = width;
        planefmts[1].width = width / 2;

        planefmts[0].height = height;
        planefmts[1].height = height / 2;

        planefmts[0].bytesperpixel = 1;
        planefmts[1].bytesperpixel = 2;
        break;
    default:
        av_log(avctx, AV_LOG_ERROR, "Unsupported pixel format ");
        return -1;
    }
    return 0;
}

static int
dq_event(context_t * ctx, struct v4l2_event *event, uint32_t max_wait_ms)
{
    int ret_val;
    do {
        ret_val = v4l2_ioctl(ctx->fd, VIDIOC_DQEVENT, event);

        if (errno != EAGAIN) {
            break;
        } else if (max_wait_ms-- == 0) {
            break;
        } else {
            usleep(1000);
        }
    }
    while (ret_val && (ctx->op_streamon || ctx->cp_streamon));

    return ret_val;
}

static int
dq_buffer(AVCodecContext * avctx, context_t * ctx,
          struct v4l2_buffer *v4l2_buf, Buffer ** buffer,
          enum v4l2_buf_type buf_type, enum v4l2_memory memory_type,
          uint32_t num_retries)
{
    int ret_val;
    bool is_in_error = false;
    v4l2_buf->type = buf_type;
    v4l2_buf->memory = memory_type;
    do {
        ret_val = v4l2_ioctl(ctx->fd, VIDIOC_DQBUF, v4l2_buf);
        if (ret_val == 0) {
            pthread_mutex_lock(&ctx->queue_lock);
            switch (v4l2_buf->type) {
            case V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE:
                if (buffer)
                    *buffer = ctx->op_buffers[v4l2_buf->index];
                for (uint32_t j = 0;
                     j < ctx->op_buffers[v4l2_buf->index]->n_planes; j++) {
                    ctx->op_buffers[v4l2_buf->index]->planes[j].bytesused =
                        v4l2_buf->m.planes[j].bytesused;
                }
                ctx->num_queued_op_buffers--;
                break;

            case V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE:
                if (buffer)
                    *buffer = ctx->cp_buffers[v4l2_buf->index];
                for (uint32_t j = 0;
                     j < ctx->cp_buffers[v4l2_buf->index]->n_planes; j++) {
                    ctx->cp_buffers[v4l2_buf->index]->planes[j].bytesused =
                        v4l2_buf->m.planes[j].bytesused;
                }
                break;

            default:
                av_log(avctx, AV_LOG_ERROR, "Invaild buffer type\n");
            }
            pthread_cond_broadcast(&ctx->queue_cond);
            pthread_mutex_unlock(&ctx->queue_lock);
        } else if (errno == EAGAIN) {
            pthread_mutex_lock(&ctx->queue_lock);
            if (v4l2_buf->flags & V4L2_BUF_FLAG_LAST) {
                pthread_mutex_unlock(&ctx->queue_lock);
                break;
            }
            pthread_mutex_unlock(&ctx->queue_lock);

            if (num_retries-- == 0) {
                av_log(avctx, AV_LOG_VERBOSE, "Resource unavailable\n");
                break;
            }
        } else {
            is_in_error = 1;
            break;
        }
    }
    while (ret_val && !is_in_error);

    return ret_val;
}

static int
q_buffer(context_t * ctx, struct v4l2_buffer *v4l2_buf, Buffer * buffer,
         enum v4l2_buf_type buf_type, enum v4l2_memory memory_type,
         int num_planes)
{
    int ret_val;
    uint32_t j;
    uint32_t i;

    pthread_mutex_lock(&ctx->queue_lock);
    buffer = ctx->op_buffers[v4l2_buf->index];
    v4l2_buf->type = buf_type;
    v4l2_buf->memory = memory_type;
    v4l2_buf->length = num_planes;

    switch (memory_type) {
    case V4L2_MEMORY_USERPTR:
        for (i = 0; i < buffer->n_planes; i++) {
            v4l2_buf->m.planes[i].m.userptr =
                (unsigned long) buffer->planes[i].data;
            v4l2_buf->m.planes[i].bytesused = buffer->planes[i].bytesused;
        }
        break;
    case V4L2_MEMORY_MMAP:
        for (j = 0; j < buffer->n_planes; ++j) {
            v4l2_buf->m.planes[j].bytesused = buffer->planes[j].bytesused;
        }
        break;

    case V4L2_MEMORY_DMABUF:
        break;

    default:
        pthread_cond_broadcast(&ctx->queue_cond);
        pthread_mutex_unlock(&ctx->queue_lock);
        return -1;
    }
    ret_val = v4l2_ioctl(ctx->fd, VIDIOC_QBUF, v4l2_buf);

    if (ret_val == 0) {
        if (v4l2_buf->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
            ctx->num_queued_op_buffers++;
        }
        pthread_cond_broadcast(&ctx->queue_cond);
    }
    pthread_mutex_unlock(&ctx->queue_lock);

    return ret_val;
}

static int
req_buffers_on_capture_plane(context_t * ctx, enum v4l2_buf_type buf_type,
                             enum v4l2_memory mem_type, int num_buffers)
{
    struct v4l2_requestbuffers reqbufs;
    int ret_val;
    memset(&reqbufs, 0, sizeof(struct v4l2_requestbuffers));

    reqbufs.count = num_buffers;
    reqbufs.memory = mem_type;
    reqbufs.type = buf_type;

    ret_val = v4l2_ioctl(ctx->fd, VIDIOC_REQBUFS, &reqbufs);
    if (ret_val)
        return ret_val;

    if (reqbufs.count) {
        ctx->cp_buffers =
            (Buffer **) malloc(reqbufs.count * sizeof(Buffer *));
        for (uint32_t i = 0; i < reqbufs.count; ++i) {
            ctx->cp_buffers[i] = (Buffer *) malloc(sizeof(Buffer));
            create_bufferfmt(ctx->cp_buffers[i], buf_type, mem_type,
                             ctx->cp_num_planes, ctx->cp_planefmts, i);
        }
    } else {
        for (uint32_t i = 0; i < ctx->cp_num_buffers; ++i) {
            free(ctx->cp_buffers[i]);
        }
        free(ctx->cp_buffers);
        ctx->cp_buffers = NULL;
    }
    ctx->cp_num_buffers = reqbufs.count;

    return ret_val;
}

static int
req_buffers_on_output_plane(context_t * ctx, enum v4l2_buf_type buf_type,
                            enum v4l2_memory mem_type, int num_buffers)
{
    struct v4l2_requestbuffers reqbufs;
    int ret_val;
    memset(&reqbufs, 0, sizeof(struct v4l2_requestbuffers));

    reqbufs.count = num_buffers;
    reqbufs.memory = mem_type;
    reqbufs.type = buf_type;

    ret_val = v4l2_ioctl(ctx->fd, VIDIOC_REQBUFS, &reqbufs);
    if (ret_val)
        return ret_val;

    if (reqbufs.count) {
        ctx->op_buffers =
            (Buffer **) malloc(reqbufs.count * sizeof(Buffer *));
        for (uint32_t i = 0; i < reqbufs.count; ++i) {
            ctx->op_buffers[i] = (Buffer *) malloc(sizeof(Buffer));
            create_bufferfmt(ctx->op_buffers[i], buf_type, mem_type,
                             ctx->op_num_planes, ctx->op_planefmts, i);
        }
    } else {
        for (uint32_t i = 0; i < ctx->op_num_buffers; ++i) {
            for (uint32_t j = 0; j < ctx->op_buffers[i]->n_planes; j++) {
                free(ctx->op_buffers[i]->planes[j].data);
            }
            free(ctx->op_buffers[i]);
        }
        free(ctx->op_buffers);
        ctx->op_buffers = NULL;
    }
    ctx->op_num_buffers = reqbufs.count;

    return ret_val;
}

static int
set_output_plane_format(context_t * ctx, uint32_t pixfmt,
                        uint32_t sizeimage)
{
    int ret_val;
    struct v4l2_format format;

    memset(&format, 0, sizeof(struct v4l2_format));
    format.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    format.fmt.pix_mp.pixelformat = pixfmt;
    format.fmt.pix_mp.num_planes = 1;
    format.fmt.pix_mp.plane_fmt[0].sizeimage = sizeimage;

    ret_val = v4l2_ioctl(ctx->fd, VIDIOC_S_FMT, &format);

    if (ret_val == 0) {
        ctx->op_num_planes = format.fmt.pix_mp.num_planes;
        for (uint32_t i = 0; i < ctx->op_num_planes; ++i) {
            ctx->op_planefmts[i].stride =
                format.fmt.pix_mp.plane_fmt[i].bytesperline;
            ctx->op_planefmts[i].sizeimage =
                format.fmt.pix_mp.plane_fmt[i].sizeimage;
        }
    }

    return ret_val;
}

static int set_ext_controls(int fd, uint32_t id, uint32_t value)
{
    int ret_val;
    struct v4l2_ext_control ctl;
    struct v4l2_ext_controls ctrls;

    memset(&ctl, 0, sizeof(struct v4l2_ext_control));
    memset(&ctrls, 0, sizeof(struct v4l2_ext_controls));
    ctl.id = id;
    ctl.value = value;
    ctrls.controls = &ctl;
    ctrls.count = 1;

    ret_val = v4l2_ioctl(fd, VIDIOC_S_EXT_CTRLS, &ctrls);

    return ret_val;
}

static int
subscribe_event(int fd, uint32_t type, uint32_t id, uint32_t flags)
{
    struct v4l2_event_subscription sub;
    int ret_val;

    memset(&sub, 0, sizeof(struct v4l2_event_subscription));

    sub.type = type;
    sub.id = id;
    sub.flags = flags;

    ret_val = v4l2_ioctl(fd, VIDIOC_SUBSCRIBE_EVENT, &sub);

    return ret_val;
}

static int
set_capture_plane_format(AVCodecContext * avctx, context_t * ctx,
                         uint32_t pixfmt, uint32_t width, uint32_t height)
{
    int ret_val;
    struct v4l2_format format;
    uint32_t num_bufferplanes;
    BufferPlaneFormat planefmts[MAX_PLANES];

    fill_buffer_plane_format(avctx, &num_bufferplanes, planefmts, width,
                             height, pixfmt);
    ctx->cp_num_planes = num_bufferplanes;
    for (uint32_t j = 0; j < num_bufferplanes; ++j) {
        ctx->cp_planefmts[j] = planefmts[j];
    }
    memset(&format, 0, sizeof(struct v4l2_format));
    format.type = ctx->cp_buf_type;
    format.fmt.pix_mp.width = width;
    format.fmt.pix_mp.height = height;
    format.fmt.pix_mp.pixelformat = pixfmt;
    format.fmt.pix_mp.num_planes = num_bufferplanes;

    ret_val = v4l2_ioctl(ctx->fd, VIDIOC_S_FMT, &format);
    if (ret_val) {
        av_log(avctx, AV_LOG_ERROR, "Error in VIDIOC_S_FMT\n");
        ctx->in_error = 1;
    } else {
        ctx->cp_num_planes = format.fmt.pix_mp.num_planes;
        for (uint32_t j = 0; j < ctx->cp_num_planes; j++) {
            ctx->cp_planefmts[j].stride =
                format.fmt.pix_mp.plane_fmt[j].bytesperline;
            ctx->cp_planefmts[j].sizeimage =
                format.fmt.pix_mp.plane_fmt[j].sizeimage;
        }
    }

    return ret_val;
}

static void query_set_capture(AVCodecContext * avctx, context_t * ctx)
{
    struct v4l2_format format;
    struct v4l2_crop crop;
    struct v4l2_control ctl;
    int ret_val;
    int32_t min_cap_buffers;
    NvBufferCreateParams input_params = { 0 };
    NvBufferCreateParams cap_params = { 0 };

    /* Get format on capture plane set by device.
     ** This may change after an resolution change event.
     */
    format.type = ctx->cp_buf_type;
    ret_val = v4l2_ioctl(ctx->fd, VIDIOC_G_FMT, &format);
    if (ret_val) {
        av_log(avctx, AV_LOG_ERROR,
               "Could not get format from decoder capture plane\n");
        ctx->in_error = 1;
        return;
    }

    /* Query cropping size and position. */
    crop.type = ctx->cp_buf_type;
    ret_val = v4l2_ioctl(ctx->fd, VIDIOC_G_CROP, &crop);
    if (ret_val) {
        av_log(avctx, AV_LOG_ERROR,
               "Could not get crop from decoder capture plane\n");
        ctx->in_error = 1;
        return;
    }

    ctx->codec_height = crop.c.height;
    ctx->codec_width = crop.c.width;

    if (ctx->dst_dma_fd != -1) {
        NvBufferDestroy(ctx->dst_dma_fd);
        ctx->dst_dma_fd = -1;
    }

    /* Create a DMA buffer. */
    input_params.payloadType = NvBufferPayload_SurfArray;
    input_params.width = crop.c.width;
    input_params.height = crop.c.height;
    input_params.layout = NvBufferLayout_Pitch;
    input_params.colorFormat =
        ctx->out_pixfmt ==
        V4L2_PIX_FMT_NV12M ? NvBufferColorFormat_NV12 :
        NvBufferColorFormat_YUV420;
    input_params.nvbuf_tag = NvBufferTag_VIDEO_DEC;

    ret_val = NvBufferCreateEx(&ctx->dst_dma_fd, &input_params);
    if (ret_val) {
        av_log(avctx, AV_LOG_ERROR, "Creation of dmabuf failed\n");
        ctx->in_error = 1;
    }

    /* Stop streaming. */
    pthread_mutex_lock(&ctx->queue_lock);
    ret_val = v4l2_ioctl(ctx->fd, VIDIOC_STREAMOFF, &ctx->cp_buf_type);
    if (ret_val) {
        ctx->in_error = 1;
    } else {
        pthread_cond_broadcast(&ctx->queue_cond);
    }
    pthread_mutex_unlock(&ctx->queue_lock);

    for (uint32_t j = 0; j < ctx->cp_num_buffers; ++j) {
        switch (ctx->cp_mem_type) {
        case V4L2_MEMORY_MMAP:
            unmap(avctx, ctx->cp_buffers[j]);
            break;
        case V4L2_MEMORY_DMABUF:
            break;
        default:
            return;
        }
    }

    /* Request buffers with count 0 and destroy all
     ** previously allocated buffers.
     */
    ret_val =
        req_buffers_on_capture_plane(ctx,
                                     V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
                                     ctx->cp_mem_type, 0);
    if (ret_val) {
        av_log(avctx, AV_LOG_ERROR,
               "Error in requesting 0 capture plane buffers\n");
        ctx->in_error = 1;
        return;
    }

    /* Destroy previous DMA buffers. */
    if (ctx->cp_mem_type == V4L2_MEMORY_DMABUF) {
        for (uint32_t index = 0; index < ctx->cp_num_buffers; ++index) {
            if (ctx->dmabuff_fd[index] != 0) {
                ret_val = NvBufferDestroy(ctx->dmabuff_fd[index]);
                if (ret_val) {
                    av_log(avctx, AV_LOG_ERROR,
                           "Failed to Destroy NvBuffer\n");
                    ctx->in_error = 1;
                }
            }
        }
    }

    /* Set capture plane format to update vars. */
    ret_val =
        set_capture_plane_format(avctx, ctx, format.fmt.pix_mp.pixelformat,
                                 format.fmt.pix_mp.width,
                                 format.fmt.pix_mp.height);
    if (ret_val) {
        av_log(avctx, AV_LOG_ERROR,
               "Error in setting capture plane format\n");
        ctx->in_error = 1;
        return;
    }

    /* Get control value for min buffers which have to
     ** be requested on capture plane.
     */
    ctl.id = V4L2_CID_MIN_BUFFERS_FOR_CAPTURE;

    ret_val = v4l2_ioctl(ctx->fd, VIDIOC_G_CTRL, &ctl);
    if (ret_val) {
        av_log(avctx, AV_LOG_ERROR, "Error getting value of control\n");
        ctx->in_error = 1;
        return;
    } else {
        min_cap_buffers = ctl.value;
    }

    if (ctx->cp_mem_type == V4L2_MEMORY_DMABUF) {
        if (format.fmt.pix_mp.quantization == V4L2_QUANTIZATION_DEFAULT) {
            av_log(avctx, AV_LOG_VERBOSE,
                   "Decoder colorspace ITU-R BT.601 with standard range luma (16-235)\n");
            cap_params.colorFormat = NvBufferColorFormat_NV12;
        } else {
            av_log(avctx, AV_LOG_VERBOSE,
                   "Decoder colorspace ITU-R BT.601 with extended range luma (0-255)\n");
            cap_params.colorFormat = NvBufferColorFormat_NV12_ER;
        }

        /* Request number of buffers more than minimum returned by ctrl. */
        ctx->cp_num_buffers = min_cap_buffers + 5;

        /* Create DMA Buffers by defining the parameters for the HW Buffer.
         ** @payloadType defines the memory handle for the NvBuffer, here
         ** defined for the set of planes.
         ** @nvbuf_tag identifies the type of device or component
         ** requesting the operation.
         ** @layout defines memory layout for the surfaces, either Pitch/BLockLinear.
         */
        for (uint32_t index = 0; index < ctx->cp_num_buffers; index++) {
            cap_params.width = crop.c.width;
            cap_params.height = crop.c.height;
            cap_params.layout = NvBufferLayout_BlockLinear;
            cap_params.payloadType = NvBufferPayload_SurfArray;
            cap_params.nvbuf_tag = NvBufferTag_VIDEO_DEC;
            ret_val =
                NvBufferCreateEx(&ctx->dmabuff_fd[index], &cap_params);
            if (ret_val) {
                av_log(avctx, AV_LOG_ERROR, "Failed to create buffers\n");
                ctx->in_error = 1;
                break;
            }
        }

        /* Request buffers on capture plane. */
        ret_val =
            req_buffers_on_capture_plane(ctx,
                                         V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
                                         ctx->cp_mem_type,
                                         ctx->cp_num_buffers);
        if (ret_val) {
            av_log(avctx, AV_LOG_ERROR,
                   "Error in requesting capture plane buffers\n");
            ctx->in_error = 1;
            return;
        }

    }

    /* Enqueue all empty buffers on capture plane. */
    for (uint32_t i = 0; i < ctx->cp_num_buffers; ++i) {
        struct v4l2_buffer v4l2_buf;
        struct v4l2_plane planes[MAX_PLANES];

        memset(&v4l2_buf, 0, sizeof(v4l2_buf));
        memset(planes, 0, sizeof(planes));

        v4l2_buf.index = i;
        v4l2_buf.m.planes = planes;
        v4l2_buf.type = ctx->cp_buf_type;
        v4l2_buf.memory = ctx->cp_mem_type;
        v4l2_buf.length = ctx->cp_num_planes;
        if (ctx->cp_mem_type == V4L2_MEMORY_DMABUF) {
            v4l2_buf.m.planes[0].m.fd = ctx->dmabuff_fd[i];
            v4l2_buf.m.planes[1].m.fd = ctx->dmabuff_fd[i];
        }

        ret_val =
            q_buffer(ctx, &v4l2_buf, NULL, ctx->cp_buf_type,
                     ctx->cp_mem_type, ctx->cp_num_planes);

        if (ret_val) {
            av_log(avctx, AV_LOG_ERROR, "Qing failed on capture plane\n");
            ctx->in_error = 1;
            return;
        }
    }

    /* Set streaming status ON on capture plane. */
    ret_val = v4l2_ioctl(ctx->fd, VIDIOC_STREAMON, &ctx->cp_buf_type);
    if (ret_val != 0) {
        av_log(avctx, AV_LOG_ERROR, "Streaming error on capture plane\n");
        ctx->in_error = 1;
    }
    ctx->cp_streamon = 1;

    av_log(avctx, AV_LOG_VERBOSE, "Query and set capture successful\n");

    return;
}

static void *capture_thread(void *arg)
{
    context_t *ctx = (context_t *) arg;
    struct v4l2_event event;
    int ret_val;

    av_log(ctx->avctx, AV_LOG_VERBOSE, "Starting capture thread\n");

    /* Need to wait for the first Resolution change event, so that
     ** the decoder knows the stream resolution and can allocate
     ** appropriate buffers when REQBUFS is called.
     */
    do {
        /* Dequeue the subscribed event. */
        ret_val = dq_event(ctx, &event, 50000);
        if (ret_val) {
            if (errno == EAGAIN) {
                av_log(ctx->avctx, AV_LOG_VERBOSE,
                       "Timeout waiting for first V4L2_EVENT_RESOLUTION_CHANGE\n");
            } else {
                av_log(ctx->avctx, AV_LOG_ERROR,
                       "Error in dequeueing decoder event\n");
            }
            ctx->in_error = 1;
            break;
        }
    }
    while ((event.type != V4L2_EVENT_RESOLUTION_CHANGE) && !ctx->in_error);

    /* Recieved first resolution change event
     ** Format and buffers are now set on capture.
     */
    if (!ctx->in_error) {
        query_set_capture(ctx->avctx, ctx);
    }

    /* Check for resolution event to again
     ** set format and buffers on capture plane.
     */
    while (!(ctx->in_error || ctx->eos)) {
        Buffer *decoded_buffer = (Buffer *) malloc(sizeof(Buffer));
        create_bufferfmt(decoded_buffer, ctx->cp_buf_type,
                         ctx->cp_mem_type, 0, 0, 0);

        ret_val = dq_event(ctx, &event, 0);
        if (ret_val == 0) {
            switch (event.type) {
            case V4L2_EVENT_RESOLUTION_CHANGE:
                query_set_capture(ctx->avctx, ctx);
                continue;
            }
        }
        /* Main Capture loop for DQ and Q. */

        while (!ctx->eos) {
            int buf_index;
            struct v4l2_buffer v4l2_buf;
            struct v4l2_plane planes[MAX_PLANES];
            NvBufferRect src_rect, dest_rect;
            NvBufferParams parm;
            NvBufferTransformParams transform_params;
            memset(&v4l2_buf, 0, sizeof(v4l2_buf));
            memset(planes, 0, sizeof(planes));
            v4l2_buf.m.planes = planes;

            /* Dequeue the filled buffer. */
            if (dq_buffer
                (ctx->avctx, ctx, &v4l2_buf, &decoded_buffer,
                 ctx->cp_buf_type, ctx->cp_mem_type, 0)) {
                if (errno == EAGAIN) {
                    usleep(1000);
                }
                break;
            }
            /* Transformation parameters are defined
             ** which are passed to the NvBufferTransform
             ** for required conversion.
             */
            src_rect.top = 0;
            src_rect.left = 0;
            src_rect.width = ctx->codec_width;
            src_rect.height = ctx->codec_height;
            dest_rect.top = 0;
            dest_rect.left = 0;
            dest_rect.width = ctx->codec_width;
            dest_rect.height = ctx->codec_height;

            memset(&transform_params, 0, sizeof(transform_params));

            /* @transform_flag defines the flags for enabling the
             ** valid transforms. All the valid parameters are
             **  present in the nvbuf_utils header.
             */
            transform_params.transform_flag = NVBUFFER_TRANSFORM_FILTER;
            transform_params.transform_flip = NvBufferTransform_None;
            transform_params.transform_filter =
                NvBufferTransform_Filter_Smart;
            transform_params.src_rect = src_rect;
            transform_params.dst_rect = dest_rect;

            if (ctx->cp_mem_type == V4L2_MEMORY_DMABUF) {
                decoded_buffer->planes[0].fd =
                    ctx->dmabuff_fd[v4l2_buf.index];
            }

            pthread_mutex_lock(&ctx->queue_lock);

            /* Blocklinear to Pitch transformation is required
             ** to dump the raw decoded buffer data.
             */
            ret_val =
                NvBufferTransform(decoded_buffer->planes[0].fd,
                                  ctx->dst_dma_fd, &transform_params);
            if (ret_val == -1) {
                ctx->in_error = 1;
                av_log(ctx->avctx, AV_LOG_ERROR, "Transform failed\n");
                break;
            }

            ret_val = NvBufferGetParams(ctx->dst_dma_fd, &parm);

            if (!ctx->frame_size[0]) {
                for (int index = 0; index < MAX_BUFFERS; index++) {
                    ctx->bufptr_0[index] =
                        (unsigned char *) malloc(sizeof(unsigned char) *
                                                 (parm.psize[0]));
                    ctx->bufptr_1[index] =
                        (unsigned char *) malloc(sizeof(unsigned char) *
                                                 (parm.psize[1]));
                    if (ctx->out_pixfmt == V4L2_PIX_FMT_YUV420M) {
                        ctx->bufptr_2[index] =
                            (unsigned char *) malloc(sizeof(unsigned char)
                                                     * (parm.psize[2]));
                    }
                }
            }

            ctx->frame_linesize[0] = parm.width[0];
            ctx->frame_size[0] = parm.psize[0];
            ctx->frame_linesize[1] = parm.width[1];
            ctx->frame_size[1] = parm.psize[1];
            if (ctx->out_pixfmt == V4L2_PIX_FMT_YUV420M) {
                ctx->frame_linesize[2] = parm.width[2];
                ctx->frame_size[2] = parm.psize[2];
            }

            if (ret_val != 0) {
                av_log(ctx->avctx, AV_LOG_ERROR, "GetParams failed\n");
                return NULL;
            }

            NvBuffer2Raw(ctx->dst_dma_fd, 0, parm.width[0], parm.height[0],
                         ctx->bufptr_0[buf_index]);
            NvBuffer2Raw(ctx->dst_dma_fd, 1, parm.width[1], parm.height[1],
                         ctx->bufptr_1[buf_index]);
            if (ctx->out_pixfmt == V4L2_PIX_FMT_YUV420M) {
                NvBuffer2Raw(ctx->dst_dma_fd, 2, parm.width[2],
                             parm.height[2], ctx->bufptr_2[buf_index]);
            }
            push(ctx->avctx, ctx->frame_pools, buf_index);
            ctx->timestamp[buf_index] = v4l2_buf.timestamp.tv_usec;

            buf_index = (buf_index + 1) % MAX_BUFFERS;
            pthread_mutex_unlock(&ctx->queue_lock);
            if (ctx->cp_mem_type == V4L2_MEMORY_DMABUF) {
                v4l2_buf.m.planes[0].m.fd =
                    ctx->dmabuff_fd[v4l2_buf.index];
            }
            /* Queue the buffer. */
            ret_val =
                q_buffer(ctx, &v4l2_buf, NULL, ctx->cp_buf_type,
                         ctx->cp_mem_type, ctx->cp_num_planes);
            if (ret_val) {
                av_log(ctx->avctx, AV_LOG_ERROR,
                       "Qing failed on capture plane\n");
                ctx->in_error = 1;
                break;
            }
        }
    }

    av_log(ctx->avctx, AV_LOG_VERBOSE,
           "Exiting decoder capture loop thread\n");
    return NULL;
}


int nvv4l2dec_decoder_get_frame(AVCodecContext * avctx, context_t * ctx,
                                nvFrame * frame)
{

    int picture_index;

    if (ctx->frame_pools->capacity == 0)
        return -1;

    picture_index = ctx->frame_pools->front;
    pop(avctx, ctx->frame_pools);
    frame->width = ctx->codec_width;
    frame->height = ctx->codec_height;

    frame->linesize[0] = ctx->frame_linesize[0];
    frame->linesize[1] = ctx->frame_linesize[1];
    frame->linesize[2] = ctx->frame_linesize[2];

    frame->payload[0] = ctx->bufptr_0[picture_index];
    frame->payload[1] = ctx->bufptr_1[picture_index];
    frame->payload[2] = ctx->bufptr_2[picture_index];

    frame->payload_size[0] = ctx->frame_size[0];
    frame->payload_size[1] = ctx->frame_size[1];
    frame->payload_size[2] = ctx->frame_size[2];
    frame->timestamp = ctx->timestamp[picture_index];

    return 0;

}

int nvv4l2_decode_process(AVCodecContext * avctx, context_t * ctx,
                          nvPacket * packet)
{
    int ret;
    /* Read the encoded data and Enqueue the output
     ** plane buffers. Exit loop in case file read is complete.
     */
    struct v4l2_buffer queue_v4l2_buf_op;
    struct v4l2_plane queue_op_planes[MAX_PLANES];
    Buffer *buffer;

    memset(&queue_v4l2_buf_op, 0, sizeof(queue_v4l2_buf_op));
    memset(queue_op_planes, 0, sizeof(queue_op_planes));
    queue_v4l2_buf_op.m.planes = queue_op_planes;
    if (ctx->index < ctx->op_num_buffers) {
        buffer = ctx->op_buffers[ctx->index];
    } else {
        ret =
            dq_buffer(avctx, ctx, &queue_v4l2_buf_op, &buffer,
                      ctx->op_buf_type, ctx->op_mem_type, -1);
        if (ret) {
            av_log(avctx, AV_LOG_ERROR,
                   "Error DQing buffer at output plane\n");
            ctx->in_error = 1;
            return 0;
        }
    }
    memcpy(buffer->planes[0].data, packet->payload, packet->payload_size);
    buffer->planes[0].bytesused = packet->payload_size;

    if (ctx->index < ctx->op_num_buffers) {
        queue_v4l2_buf_op.index = ctx->index;
        queue_v4l2_buf_op.m.planes = queue_op_planes;
    }
    queue_v4l2_buf_op.m.planes[0].bytesused = buffer->planes[0].bytesused;
    queue_v4l2_buf_op.flags |= V4L2_BUF_FLAG_TIMESTAMP_COPY;
    queue_v4l2_buf_op.timestamp.tv_usec = packet->pts;

    ret = q_buffer(ctx, &queue_v4l2_buf_op, buffer,
                   ctx->op_buf_type, ctx->op_mem_type, ctx->op_num_planes);
    if (ret) {
        av_log(avctx, AV_LOG_ERROR, "Error Qing buffer at output plane\n");
        ctx->in_error = 1;
        return 0;
    }
    if (ctx->index < ctx->op_num_buffers) {
        ctx->index++;
    }

    if (queue_v4l2_buf_op.m.planes[0].bytesused == 0) {
        ctx->eos = true;
        av_log(avctx, AV_LOG_VERBOSE, "Input file read complete\n");
    }

    return 0;
}


context_t *nvv4l2dec_create_decoder(AVCodecContext * avctx,
                                    nvCodingType nv_codec_type,
                                    int pixFormat)
{
    context_t *ctx = (context_t *) calloc(1, sizeof(context_t));
    int ret = 0;
    int flags = 0;
    ctx->fd = -1;
    /* The call creates a new V4L2 Video Decoder object
     ** on the device node "/dev/nvhost-nvdec"
     ** Additional flags can also be given with which the device
     ** should be opened.
     ** This opens the device in Blocking mode.
     */
    ctx->fd = v4l2_open(DECODER_DEV, flags | O_RDWR);
    if (ctx->fd == -1) {
        av_log(avctx, AV_LOG_ERROR, "Could not open device\n");
        ctx->in_error = 1;
    }

    /* Initialisation. */
    ctx->out_pixfmt = pixFormat;
    switch (nv_codec_type) {
    case NvVideoCodec_H264:
        ctx->decode_pixfmt = V4L2_PIX_FMT_H264;
        break;
    case NvVideoCodec_HEVC:
        ctx->decode_pixfmt = V4L2_PIX_FMT_H265;
        break;
    case NvVideoCodec_MPEG2:
        ctx->decode_pixfmt = V4L2_PIX_FMT_MPEG2;
        break;
    case NvVideoCodec_MPEG4:
        ctx->decode_pixfmt = V4L2_PIX_FMT_MPEG4;
        break;
    case NvVideoCodec_VP8:
        ctx->decode_pixfmt = V4L2_PIX_FMT_VP8;
        break;
    case NvVideoCodec_VP9:
        ctx->decode_pixfmt = V4L2_PIX_FMT_VP9;
        break;
    default:
        ctx->decode_pixfmt = V4L2_PIX_FMT_H264;
        break;
    }
    ctx->op_mem_type = V4L2_MEMORY_USERPTR;
    ctx->cp_mem_type = V4L2_MEMORY_DMABUF;
    ctx->op_buf_type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    ctx->cp_buf_type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    ctx->index = 0;
    ctx->dst_dma_fd = -1;
    ctx->num_queued_op_buffers = 0;
    ctx->op_buffers = NULL;
    ctx->cp_buffers = NULL;
    ctx->frame_pools = (queues *) malloc(sizeof(queues));
    ctx->frame_pools->data =
        (uint32_t *) malloc(sizeof(uint32_t) * MAX_BUFFERS);
    ctx->frame_pools->front = 0;
    ctx->frame_pools->back = 0;
    ctx->frame_pools->capacity = 0;
    ctx->frame_size[0] = 0;
    pthread_mutex_init(&ctx->queue_lock, NULL);
    pthread_cond_init(&ctx->queue_cond, NULL);

    /* Subscribe to Resolution change event.
     ** This is required to catch whenever resolution change event
     ** is triggered to set the format on capture plane.
     */
    ret = subscribe_event(ctx->fd, V4L2_EVENT_RESOLUTION_CHANGE, 0, 0);
    if (ret) {
        av_log(avctx, AV_LOG_ERROR,
               "Failed to subscribe for resolution change\n");
        ctx->in_error = 1;
    }

    /* Set format on output plane.
     ** The format of the encoded bitstream is set.
     */
    ret = set_output_plane_format(ctx, ctx->decode_pixfmt, CHUNK_SIZE);
    if (ret) {
        av_log(avctx, AV_LOG_ERROR,
               "Error in setting output plane format\n");
        ctx->in_error = 1;
    }

    /* Set appropriate controls.
     ** V4L2_CID_MPEG_VIDEO_DISABLE_COMPLETE_FRAME_INPUT control is
     ** set to false so that application can send chunks of encoded
     ** data instead of forming complete frames.
     */
    ret =
        set_ext_controls(ctx->fd,
                         V4L2_CID_MPEG_VIDEO_DISABLE_COMPLETE_FRAME_INPUT,
                         1);
    if (ret) {
        av_log(avctx, AV_LOG_ERROR,
               "Failed to set control disable complete frame\n");
        ctx->in_error = 1;
    }

    /* Request buffers on output plane to fill
     ** the input bitstream.
     */
    ret =
        req_buffers_on_output_plane(ctx, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
                                    ctx->op_mem_type, 10);
    if (ret) {
        av_log(avctx, AV_LOG_ERROR,
               "Error in requesting buffers on output plane\n");
        ctx->in_error = 1;
    }

    for (uint32_t i = 0; i < ctx->op_num_buffers; ++i) {
        if (allocateMemory(avctx, ctx->op_buffers[i])) {
            av_log(avctx, AV_LOG_ERROR,
                   "Buffer mapping error on output plane\n");
            ctx->in_error = 1;
        }
    }

    /* Start stream processing on output plane
     ** by setting the streaming status ON.
     */
    ret = v4l2_ioctl(ctx->fd, VIDIOC_STREAMON, &ctx->op_buf_type);
    if (ret != 0) {
        av_log(avctx, AV_LOG_ERROR, "Streaming error on output plane\n");
        ctx->in_error = 1;
    }

    ctx->op_streamon = 1;

    /* Create Capture loop thread. */
    ctx->avctx = avctx;
    pthread_create(&ctx->dec_capture_thread, NULL, capture_thread, ctx);


    return ctx;
}

int nvv4l2dec_decoder_close(AVCodecContext * avctx, context_t * ctx)
{
    int ret;
    pthread_mutex_lock(&ctx->queue_lock);
    ctx->eos = true;
    pthread_mutex_unlock(&ctx->queue_lock);
    ret = v4l2_ioctl(ctx->fd, VIDIOC_STREAMOFF, &ctx->cp_buf_type);
    if (ctx->fd != -1) {
        if (ctx->dec_capture_thread) {
            pthread_join(ctx->dec_capture_thread, NULL);
        }
        /* All the allocated DMA buffers must be destroyed. */
        if (ctx->cp_mem_type == V4L2_MEMORY_DMABUF) {
            for (uint32_t idx = 0; idx < ctx->cp_num_buffers; ++idx) {
                ret = NvBufferDestroy(ctx->dmabuff_fd[idx]);
                if (ret) {
                    av_log(avctx, AV_LOG_ERROR,
                           "Failed to Destroy Buffers\n");
                }
            }
        }

        ret = v4l2_ioctl(ctx->fd, VIDIOC_STREAMOFF, &ctx->op_buf_type);

        /* Unmap MMAPed buffers. */
        if (ctx->op_mem_type == V4L2_MEMORY_MMAP) {
            for (uint32_t i = 0; i < ctx->op_num_buffers; ++i) {
                unmap(avctx, ctx->op_buffers[i]);
            }
        }
        /* Request 0 buffers on both planes. */
        ret =
            req_buffers_on_output_plane(ctx,
                                        V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
                                        ctx->op_mem_type, 0);
        ret =
            req_buffers_on_capture_plane(ctx,
                                         V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
                                         ctx->cp_mem_type, 0);

        /* Destroy DMA buffers. */
        if (ctx->cp_mem_type == V4L2_MEMORY_DMABUF) {
            for (uint32_t i = 0; i < ctx->cp_num_buffers; ++i) {
                if (ctx->dmabuff_fd[i] != 0) {
                    ret = NvBufferDestroy(ctx->dmabuff_fd[i]);
                    ctx->dmabuff_fd[i] = 0;
                    if (ret < 0) {
                        av_log(avctx, AV_LOG_ERROR,
                               "Failed to destroy buffer\n");
                    }
                }
            }
        }
        if (ctx->dst_dma_fd != -1) {
            NvBufferDestroy(ctx->dst_dma_fd);
            ctx->dst_dma_fd = -1;
        }
        for (int index = 0; index < MAX_BUFFERS; index++) {
            free(ctx->bufptr_0[index]);
            free(ctx->bufptr_1[index]);
            free(ctx->bufptr_2[index]);
        }

        free(ctx->frame_pools->data);
        free(ctx->frame_pools);

        /* Close the opened V4L2 device. */
        ret = v4l2_close(ctx->fd);
        if (ret) {
            av_log(avctx, AV_LOG_ERROR, "Unable to close the device\n");
        }
    }
    /* Report application run status on exit. */
    if (ctx->in_error) {
        av_log(avctx, AV_LOG_VERBOSE, "Decoder Run failed\n");
    } else {
        free(ctx);
        av_log(avctx, AV_LOG_VERBOSE, "Decoder Run is successful\n");
    }

    return ret;
}

static nvCodingType map_avcodec_id(enum AVCodecID id)
{
    switch (id) {
    case AV_CODEC_ID_H264:
        return NvVideoCodec_H264;
    case AV_CODEC_ID_HEVC:
        return NvVideoCodec_HEVC;
    case AV_CODEC_ID_MPEG2VIDEO:
        return NvVideoCodec_MPEG2;
    case AV_CODEC_ID_MPEG4:
        return NvVideoCodec_MPEG4;
    case AV_CODEC_ID_VP8:
        return NvVideoCodec_VP8;
    case AV_CODEC_ID_VP9:
        return NvVideoCodec_VP9;
    }
    return NvVideoCodec_UNDEFINED;
}

typedef struct {
    char eos_reached;
    context_t *ctx;
    AVClass *av_class;
} nvv4l2DecodeContext;

static int nvv4l2dec_init_decoder(AVCodecContext * avctx)
{
    int ret = 0;
    nvv4l2DecodeContext *nvv4l2_context = avctx->priv_data;
    nvCodingType nv_codec_type;
    nv_codec_type = map_avcodec_id(avctx->codec_id);
    if (nv_codec_type < 0) {
        av_log(avctx, AV_LOG_ERROR, "Unsupported codec ID\n");
        ret = AVERROR_BUG;
        return ret;
    }

    nvv4l2_context->ctx =
        nvv4l2dec_create_decoder(avctx, nv_codec_type,
                                 V4L2_PIX_FMT_YUV420M);

    if (!nvv4l2_context->ctx) {
        av_log(avctx, AV_LOG_ERROR,
               "Failed to nvv4l2dec_create_decoder (code = %d).\n", ret);
        ret = AVERROR_UNKNOWN;
        return ret;
    }
    return ret;
}

static int nvv4l2dec_close(AVCodecContext * avctx)
{

    nvv4l2DecodeContext *nvv4l2_context = avctx->priv_data;
    return nvv4l2dec_decoder_close(avctx, nvv4l2_context->ctx);
}

static int nvv4l2dec_decode(AVCodecContext * avctx, void *data,
                            int *got_frame, AVPacket * avpkt)
{
    nvv4l2DecodeContext *nvv4l2_context = avctx->priv_data;
    AVFrame *frame = (AVFrame *) data;
    nvFrame _nvframe = { 0 };
    nvPacket packet;
    uint8_t *ptrs[3];
    int res, linesize[3];

    if (avpkt->size) {
        packet.payload_size = avpkt->size;
        packet.payload = avpkt->data;
        packet.pts = avpkt->pts;
        res = nvv4l2_decode_process(avctx, nvv4l2_context->ctx, &packet);
    }
    res =
        nvv4l2dec_decoder_get_frame(avctx, nvv4l2_context->ctx, &_nvframe);

    if (res < 0)
        return avpkt->size;

    if (ff_get_buffer(avctx, frame, 0) < 0) {
        return AVERROR(ENOMEM);
    }

    linesize[0] = _nvframe.linesize[0];
    linesize[1] = _nvframe.linesize[1];
    linesize[2] = _nvframe.linesize[2];

    ptrs[0] = _nvframe.payload[0];
    ptrs[1] = _nvframe.payload[1];
    ptrs[2] = _nvframe.payload[2];

    av_image_copy(frame->data, frame->linesize, (const uint8_t **) ptrs,
                  linesize, avctx->pix_fmt, _nvframe.width,
                  _nvframe.height);

    frame->width = _nvframe.width;
    frame->height = _nvframe.height;

    frame->format = AV_PIX_FMT_YUV420P;
    frame->pts = _nvframe.timestamp;
    frame->pkt_dts = AV_NOPTS_VALUE;

    avctx->coded_width = _nvframe.width;
    avctx->coded_height = _nvframe.height;
    avctx->width = _nvframe.width;
    avctx->height = _nvframe.height;

    *got_frame = 1;

    return avpkt->size;
}

#define nvv4l2dec_DEC_CLASS(NAME) \
	static const AVClass nvv4l2dec_##NAME##_dec_class = { \
		.class_name = "nvv4l2dec_" #NAME "_dec", \
		.version    = LIBAVUTIL_VERSION_INT, \
	};

#define nvv4l2dec_DEC(NAME, ID, BSFS) \
	nvv4l2dec_DEC_CLASS(NAME) \
	AVCodec ff_##NAME##_nvv4l2dec_decoder = { \
		.name           = #NAME "_nvv4l2dec", \
		.long_name      = NULL_IF_CONFIG_SMALL(#NAME " (nvv4l2dec)"), \
		.type           = AVMEDIA_TYPE_VIDEO, \
		.id             = ID, \
		.priv_data_size = sizeof(nvv4l2DecodeContext), \
		.init           = nvv4l2dec_init_decoder, \
		.close          = nvv4l2dec_close, \
		.decode         = nvv4l2dec_decode, \
		.priv_class     = &nvv4l2dec_##NAME##_dec_class, \
		.capabilities   = AV_CODEC_CAP_DELAY | AV_CODEC_CAP_AVOID_PROBING | AV_CODEC_CAP_HARDWARE, \
		.pix_fmts	=(const enum AVPixelFormat[]){AV_PIX_FMT_YUV420P,AV_PIX_FMT_NV12,AV_PIX_FMT_NONE},\
		.bsfs           = BSFS, \
		.wrapper_name   = "nvv4l2dec", \
	};



nvv4l2dec_DEC(h264, AV_CODEC_ID_H264, "h264_mp4toannexb");
nvv4l2dec_DEC(hevc, AV_CODEC_ID_HEVC, "hevc_mp4toannexb");
nvv4l2dec_DEC(mpeg2, AV_CODEC_ID_MPEG2VIDEO, NULL);
nvv4l2dec_DEC(mpeg4, AV_CODEC_ID_MPEG4, NULL);
nvv4l2dec_DEC(vp9, AV_CODEC_ID_VP9, NULL);
nvv4l2dec_DEC(vp8, AV_CODEC_ID_VP8, NULL);

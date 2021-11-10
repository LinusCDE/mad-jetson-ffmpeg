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
 * Specifies the decoder device node.
 */
#ifndef __nvv4l2_H__
#define __nvv4l2_H__

#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <linux/videodev2.h>

#define DECODER_DEV "/dev/nvhost-nvdec"
#define MAX_BUFFERS 32
#define MAX_NUM_PLANES 4
#define CHUNK_SIZE 4000000

/**
 * Specifies the maximum number of planes a buffer can contain.
 */
#define MAX_PLANES 3
#define MIN(a,b) (((a) < (b)) ? (a) : (b))

typedef struct _queue {

    uint32_t *data;
    uint32_t capacity;
    uint32_t front;
    uint32_t back;
} queues;

typedef enum {
    NV_PIX_NV12,
    NV_PIX_YUV420
} nvPixFormat;

typedef struct _NVPACKET {
    unsigned long flags;
    unsigned long payload_size;
    unsigned char *payload;
    unsigned long pts;
} nvPacket;

typedef struct _NVFRAME {
    unsigned long flags;
    unsigned long payload_size[3];
    unsigned char *payload[3];
    unsigned int linesize[3];
    nvPixFormat type;
    unsigned int width;
    unsigned int height;
    time_t timestamp;
} nvFrame;


typedef enum {
    NvVideoCodec_H264,                 /**< H.264 */
    NvVideoCodec_MPEG4,                  /**< MPEG-4 */
    NvVideoCodec_MPEG2,                  /**< MPEG-2 */
    NvVideoCodec_VP8,                    /**< VP8 */
    NvVideoCodec_VP9,                    /**< VP9 */
    NvVideoCodec_HEVC,                   /**< H.265/HEVC */
    NvVideoCodec_UNDEFINED,
} nvCodingType;

typedef struct {
    uint32_t width;                 /**< Holds the width of the plane in pixels. */
    uint32_t height;                /**< Holds the height of the plane in pixels. */

    uint32_t bytesperpixel;         /**< Holds the bytes used to represent one
									pixel in the plane. */
    uint32_t stride;                /**< Holds the stride of the plane in bytes. */
    uint32_t sizeimage;             /**< Holds the size of the plane in bytes. */
} BufferPlaneFormat;

    /**
     * Holds the buffer plane parameters.
     */

typedef struct {
    BufferPlaneFormat fmt;          /**< Holds the format of the plane. */

    unsigned char *data;            /**< Holds a pointer to the plane memory. */
    uint32_t bytesused;             /**< Holds the number of valid bytes in the plane. */

    int fd;                         /**< Holds the file descriptor (FD) of the plane of the
									exported buffer, in the case of V4L2 MMAP buffers. */
    uint32_t mem_offset;            /**< Holds the offset of the first valid byte
									from the data pointer. */
    uint32_t length;                /**< Holds the size of the buffer in bytes. */
} BufferPlane;

typedef struct {

    enum v4l2_buf_type buf_type;    /**< Type of the buffer. */
    enum v4l2_memory memory_type;   /**< Type of memory associated
                                        with the buffer. */

    uint32_t index;                 /**< Holds the buffer index. */

    uint32_t n_planes;              /**< Holds the number of planes in the buffer. */
    BufferPlane planes[MAX_PLANES];
    bool mapped;
} Buffer;

Buffer *create_buffer(enum v4l2_buf_type buf_type,
                      enum v4l2_memory memory_type, uint32_t index);

int create_bufferfmt(Buffer * buffer, enum v4l2_buf_type buf_type,
                     enum v4l2_memory memory_type, uint32_t n_planes,
                     BufferPlaneFormat * fmt, uint32_t index);

void destroyBuffer(AVCodecContext * avctx, Buffer * buffer);

int allocateMemory(AVCodecContext * avctx, Buffer * buffer);

int map(AVCodecContext * avctx, Buffer * buffer);

void unmap(AVCodecContext * avctx, Buffer * buffer);

int fill_buffer_plane_format(AVCodecContext * avctx, uint32_t * num_planes,
                             BufferPlaneFormat * planefmts,
                             uint32_t width, uint32_t height,
                             uint32_t raw_pixfmt);

/**
 * @brief Struct defining the decoder context.
 * The video decoder device node is `/dev/nvhost-nvdec`. The category name
 * for the decoder is \c "NVDEC".
 *
 * The context stores the information for decoding.
 * Refer to [V4L2 Video Decoder](group__V4L2Dec.html) for more information on the decoder.
 */

typedef struct {
    int index;
    unsigned int decode_pixfmt;
    unsigned int out_pixfmt;
    unsigned int codec_width;
    unsigned int codec_height;
    enum v4l2_memory op_mem_type;
    enum v4l2_memory cp_mem_type;
    enum v4l2_buf_type op_buf_type;
    enum v4l2_buf_type cp_buf_type;
    BufferPlaneFormat op_planefmts[MAX_PLANES];
    BufferPlaneFormat cp_planefmts[MAX_PLANES];
    unsigned int cp_num_planes;
    unsigned int op_num_planes;
    unsigned int cp_num_buffers;
    unsigned int op_num_buffers;
    queues *frame_pools;
    unsigned int num_queued_op_buffers;
    unsigned int indx;
    Buffer **op_buffers;
    Buffer **cp_buffers;
    pthread_mutex_t queue_lock;
    pthread_cond_t queue_cond;
    pthread_t dec_capture_thread;
    bool in_error;
    bool eos;
    bool got_eos;
    bool op_streamon;
    bool cp_streamon;
    int fd;
    int dst_dma_fd;
    int dmabuff_fd[MAX_BUFFERS];
    unsigned char *bufptr_0[MAX_BUFFERS];
    unsigned char *bufptr_1[MAX_BUFFERS];
    unsigned char *bufptr_2[MAX_BUFFERS];
    unsigned int frame_size[MAX_NUM_PLANES];
    unsigned int frame_linesize[MAX_NUM_PLANES];
    unsigned long long timestamp[MAX_BUFFERS];
    AVCodecContext *avctx;
} context_t;


context_t *nvv4l2dec_create_decoder(AVCodecContext * avctx,
                                    nvCodingType codingType,
                                    int pixFormat);

int nvv4l2_decode_process(AVCodecContext * avctx, context_t * ctx,
                          nvPacket * packet);

int nvv4l2dec_decoder_get_frame(AVCodecContext * avctx, context_t * ctx,
                                nvFrame * frame);

int nvv4l2dec_decoder_close(AVCodecContext * avctx, context_t * ctx);

#endif

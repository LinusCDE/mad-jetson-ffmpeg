#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>

#include <nvmpi.h>
#include "avcodec.h"
#include "decode.h"
#include "internal.h"
#include "libavutil/buffer.h"
#include "libavutil/common.h"
#include "libavutil/frame.h"
#include "libavutil/hwcontext.h"
#include "libavutil/hwcontext_drm.h"
#include "libavutil/imgutils.h"
#include "libavutil/log.h"




typedef struct {
	char eos_reached;
	nvmpictx* ctx;
	AVClass *av_class;
} nvmpiDecodeContext;

static nvCodingType nvmpi_get_codingtype(AVCodecContext *avctx)
{
	switch (avctx->codec_id) {
		case AV_CODEC_ID_H264:          return NV_VIDEO_CodingH264;
		case AV_CODEC_ID_HEVC:          return NV_VIDEO_CodingHEVC;
		case AV_CODEC_ID_VP8:           return NV_VIDEO_CodingVP8;
		case AV_CODEC_ID_VP9:           return NV_VIDEO_CodingVP9;
		case AV_CODEC_ID_MPEG4:		return NV_VIDEO_CodingMPEG4;
		case AV_CODEC_ID_MPEG2VIDEO:    return NV_VIDEO_CodingMPEG2;
		default:                        return NV_VIDEO_CodingUnused;
	}
};


static int nvmpi_init_decoder(AVCodecContext *avctx){

	nvmpiDecodeContext *nvmpi_context = avctx->priv_data;
	nvCodingType codectype=NV_VIDEO_CodingUnused;

	codectype =nvmpi_get_codingtype(avctx);
	if (codectype == NV_VIDEO_CodingUnused) {
		av_log(avctx, AV_LOG_ERROR, "Unknown codec type (%d).\n", avctx->codec_id);
		return AVERROR_UNKNOWN;
	}

	//Workaround for default pix_fmt not being set, so check if it isnt set and set it,
   //or if it is set, but isnt set to something we can work with.

	if(avctx->pix_fmt ==AV_PIX_FMT_NONE){
		 avctx->pix_fmt=AV_PIX_FMT_YUV420P;
	}else if(avctx-> pix_fmt != AV_PIX_FMT_YUV420P){
		av_log(avctx, AV_LOG_ERROR, "Invalid Pix_FMT for NVMPI Only yuv420p is supported\n");
		return AVERROR_INVALIDDATA;
	}

	nvmpi_context->ctx=nvmpi_create_decoder(codectype,NV_PIX_YUV420);

	if(!nvmpi_context->ctx){
		av_log(avctx, AV_LOG_ERROR, "Failed to nvmpi_create_decoder (code = %d).\n", AVERROR_EXTERNAL);
		return AVERROR_EXTERNAL;
	}
   return 0;

}



static int nvmpi_close(AVCodecContext *avctx){

	nvmpiDecodeContext *nvmpi_context = avctx->priv_data;
	return nvmpi_decoder_close(nvmpi_context->ctx);

}



static int nvmpi_decode(AVCodecContext *avctx,void *data,int *got_frame, AVPacket *avpkt){

	nvmpiDecodeContext *nvmpi_context = avctx->priv_data;
	AVFrame *frame = data;
	nvFrame _nvframe={0};
	nvPacket packet;
	uint8_t* ptrs[3];
	int res,linesize[3];

	if(avpkt->size){
		packet.payload_size=avpkt->size;
		packet.payload=avpkt->data;
		packet.pts=avpkt->pts;

		res=nvmpi_decoder_put_packet(nvmpi_context->ctx,&packet);
	}

	res=nvmpi_decoder_get_frame(nvmpi_context->ctx,&_nvframe,avctx->flags & AV_CODEC_FLAG_LOW_DELAY);

	if(res<0)
		return avpkt->size;

	if (ff_get_buffer(avctx, frame, 0) < 0) {
		return AVERROR(ENOMEM);

	}

	linesize[0]=_nvframe.linesize[0];
	linesize[1]=_nvframe.linesize[1];
	linesize[2]=_nvframe.linesize[2];

	ptrs[0]=_nvframe.payload[0];
	ptrs[1]=_nvframe.payload[1];
	ptrs[2]=_nvframe.payload[2];

	av_image_copy(frame->data, frame->linesize, (const uint8_t **) ptrs, linesize, avctx->pix_fmt, _nvframe.width,_nvframe.height);

	frame->width=_nvframe.width;
	frame->height=_nvframe.height;

	frame->format=AV_PIX_FMT_YUV420P;
	frame->pts=_nvframe.timestamp;
	frame->pkt_dts = AV_NOPTS_VALUE;

	avctx->coded_width=_nvframe.width;
	avctx->coded_height=_nvframe.height;
	avctx->width=_nvframe.width;
	avctx->height=_nvframe.height;

	*got_frame = 1;

	return avpkt->size;
}




#define NVMPI_DEC_CLASS(NAME) \
	static const AVClass nvmpi_##NAME##_dec_class = { \
		.class_name = "nvmpi_" #NAME "_dec", \
		.version    = LIBAVUTIL_VERSION_INT, \
	};

#define NVMPI_DEC(NAME, ID, BSFS) \
	NVMPI_DEC_CLASS(NAME) \
	AVCodec ff_##NAME##_nvmpi_decoder = { \
		.name           = #NAME "_nvmpi", \
		.long_name      = NULL_IF_CONFIG_SMALL(#NAME " (nvmpi)"), \
		.type           = AVMEDIA_TYPE_VIDEO, \
		.id             = ID, \
		.priv_data_size = sizeof(nvmpiDecodeContext), \
		.init           = nvmpi_init_decoder, \
		.close          = nvmpi_close, \
		.decode         = nvmpi_decode, \
		.priv_class     = &nvmpi_##NAME##_dec_class, \
		.capabilities   = AV_CODEC_CAP_DELAY | AV_CODEC_CAP_AVOID_PROBING | AV_CODEC_CAP_HARDWARE, \
		.pix_fmts	=(const enum AVPixelFormat[]){AV_PIX_FMT_YUV420P,AV_PIX_FMT_NV12,AV_PIX_FMT_NONE},\
		.bsfs           = BSFS, \
		.wrapper_name   = "nvmpi", \
	};



NVMPI_DEC(h264,  AV_CODEC_ID_H264,"h264_mp4toannexb");
NVMPI_DEC(hevc,  AV_CODEC_ID_HEVC,"hevc_mp4toannexb");
NVMPI_DEC(mpeg2, AV_CODEC_ID_MPEG2VIDEO,NULL);
NVMPI_DEC(mpeg4, AV_CODEC_ID_MPEG4,NULL);
NVMPI_DEC(vp9,  AV_CODEC_ID_VP9,NULL);
NVMPI_DEC(vp8, AV_CODEC_ID_VP8,NULL);


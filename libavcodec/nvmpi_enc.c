#include <nvmpi.h>
#include "avcodec.h"
#include "internal.h"
#include <stdio.h>
#include "libavutil/avstring.h"
#include "libavutil/avutil.h"
#include "libavutil/common.h"
#include "libavutil/imgutils.h"
#include "libavutil/log.h"
#include "libavutil/opt.h"


typedef struct {
	const AVClass *class;
	nvmpictx* ctx;
	int num_capture_buffers;
	int profile;
	int level;
	int rc;
	int preset;
}nvmpiEncodeContext;

static av_cold int nvmpi_encode_init(AVCodecContext *avctx){

	nvmpiEncodeContext * nvmpi_context = avctx->priv_data;

	nvEncParam param={0};

	param.width=avctx->width;
	param.height=avctx->height;
	param.bitrate=avctx->bit_rate;
	param.mode_vbr=0;
	param.idr_interval=60;
	param.iframe_interval=30;
	param.peak_bitrate=0;
	param.fps_n=avctx->framerate.num;
	param.fps_d=avctx->framerate.den;
	param.profile=nvmpi_context->profile& ~FF_PROFILE_H264_INTRA;
	param.level=nvmpi_context->level;
	param.capture_num=nvmpi_context->num_capture_buffers;
	param.hw_preset_type=nvmpi_context->preset;
	param.insert_spspps_idr=(avctx->flags & AV_CODEC_FLAG_GLOBAL_HEADER)?0:1;

	if(nvmpi_context->rc==1){
		param.mode_vbr=1;
	}

	if(avctx->qmin >= 0 && avctx->qmax >= 0){
		param.qmin=avctx->qmin;
		param.qmax=avctx->qmax;
	}

	if (avctx->refs >= 0){
		param.refs=avctx->refs;

	}

	if(avctx->max_b_frames > 0 && avctx->max_b_frames < 3){
		param.max_b_frames=avctx->max_b_frames;
	}

	if(avctx->gop_size>0){
		param.idr_interval=param.iframe_interval=avctx->gop_size;

	}


	if ((avctx->flags & AV_CODEC_FLAG_GLOBAL_HEADER) && (avctx->codec->id == AV_CODEC_ID_H264)){

		uint8_t *dst[4];
		int linesize[4];
		nvFrame _nvframe={0};
		nvPacket packet={0};
		int i;
		int ret;
		nvmpictx* _ctx;
		av_image_alloc(dst, linesize,avctx->width,avctx->height,avctx->pix_fmt,1);

		_ctx=nvmpi_create_encoder(NV_VIDEO_CodingH264,&param);
		i=0;

		while(1){

			_nvframe.payload[0]=dst[0];
			_nvframe.payload[1]=dst[1];
			_nvframe.payload[2]=dst[2];
			_nvframe.payload_size[0]=linesize[0]*avctx->height;
			_nvframe.payload_size[1]=linesize[1]*avctx->height/2;
			_nvframe.payload_size[2]=linesize[2]*avctx->height/2;

			nvmpi_encoder_put_frame(_ctx,&_nvframe);

			ret=nvmpi_encoder_get_packet(_ctx,&packet);

			if(ret<0)
				continue;

			//find idr index 0x0000000165
			while((packet.payload[i]!=0||packet.payload[i+1]!=0||packet.payload[i+2]!=0||packet.payload[i+3]!=0x01||packet.payload[i+4]!=0x65)){
				i++;

			}

			avctx->extradata_size=i;
			avctx->extradata	= av_mallocz( avctx->extradata_size + AV_INPUT_BUFFER_PADDING_SIZE );
			memcpy( avctx->extradata, packet.payload,avctx->extradata_size);
			memset( avctx->extradata + avctx->extradata_size, 0, AV_INPUT_BUFFER_PADDING_SIZE );

			break;

		}

		nvmpi_encoder_close(_ctx);


	}

	if(avctx->codec->id == AV_CODEC_ID_H264)
		nvmpi_context->ctx=nvmpi_create_encoder(NV_VIDEO_CodingH264,&param);
	else if(avctx->codec->id == AV_CODEC_ID_HEVC){
		nvmpi_context->ctx=nvmpi_create_encoder(NV_VIDEO_CodingHEVC,&param);
	}


	return 0;
}


static int nvmpi_encode_frame(AVCodecContext *avctx, AVPacket *pkt,const AVFrame *frame, int *got_packet){

	nvmpiEncodeContext * nvmpi_context = avctx->priv_data;
	nvFrame _nvframe={0};
	nvPacket packet={0};
	int res;

	if(frame){

		_nvframe.payload[0]=frame->data[0];
		_nvframe.payload[1]=frame->data[1];
		_nvframe.payload[2]=frame->data[2];

		_nvframe.payload_size[0]=frame->linesize[0]*frame->height;
		_nvframe.payload_size[1]=frame->linesize[1]*frame->height/2;
		_nvframe.payload_size[2]=frame->linesize[2]*frame->height/2;

		_nvframe.linesize[0]=frame->linesize[0];
		_nvframe.linesize[1]=frame->linesize[1];
		_nvframe.linesize[2]=frame->linesize[2];

		_nvframe.timestamp=frame->pts;

		res=nvmpi_encoder_put_frame(nvmpi_context->ctx,&_nvframe);

		if(res<0)
			return res;
	}


	if(nvmpi_encoder_get_packet(nvmpi_context->ctx,&packet)<0)
		return 0;


	ff_alloc_packet2(avctx,pkt,packet.payload_size,packet.payload_size);

	memcpy(pkt->data,packet.payload,packet.payload_size);
	pkt->dts=pkt->pts=packet.pts;

	if(packet.flags& AV_PKT_FLAG_KEY)
		pkt->flags = AV_PKT_FLAG_KEY;


	*got_packet = 1;

	return 0;
}

static av_cold int nvmpi_encode_close(AVCodecContext *avctx){

	nvmpiEncodeContext *nvmpi_context = avctx->priv_data;
	nvmpi_encoder_close(nvmpi_context->ctx);

	return 0;
}

static const AVCodecDefault defaults[] = {
	{ "b", "2M" },
	{ "qmin", "-1" },
	{ "qmax", "-1" },
	{ "qdiff", "-1" },
	{ "qblur", "-1" },
	{ "qcomp", "-1" },
	{ "g", "50" },
	{ "bf", "0" },
	{ "refs", "0" },
	{ NULL },
};


#define OFFSET(x) offsetof(nvmpiEncodeContext, x)
#define VE AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_ENCODING_PARAM

static const AVOption options[] = {
	{ "num_capture_buffers", "Number of buffers in the capture context", OFFSET(num_capture_buffers), AV_OPT_TYPE_INT, {.i64 = 10 }, 1, 32, AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_ENCODING_PARAM },
	/// Profile,

	{ "profile",  "Set the encoding profile", OFFSET(profile), AV_OPT_TYPE_INT,   { .i64 = FF_PROFILE_UNKNOWN },       FF_PROFILE_UNKNOWN, FF_PROFILE_H264_HIGH, VE, "profile" },
	{ "baseline", "",                         0,               AV_OPT_TYPE_CONST, { .i64 = FF_PROFILE_H264_BASELINE }, 0, 0, VE, "profile" },
	{ "main",     "",                         0,               AV_OPT_TYPE_CONST, { .i64 = FF_PROFILE_H264_MAIN },     0, 0, VE, "profile" },
	{ "high",     "",                         0,               AV_OPT_TYPE_CONST, { .i64 = FF_PROFILE_H264_HIGH },     0, 0, VE, "profile" },

	/// Profile Level
	{ "level",          "Profile Level",        OFFSET(level),  AV_OPT_TYPE_INT,   { .i64 = 0  }, 0, 62, VE, "level" },
	{ "auto",           "",                     0,              AV_OPT_TYPE_CONST, { .i64 = 0  }, 0, 0,  VE, "level" },
	{ "1.0",            "",                     0,              AV_OPT_TYPE_CONST, { .i64 = 10 }, 0, 0,  VE, "level" },
	{ "1.1",            "",                     0,              AV_OPT_TYPE_CONST, { .i64 = 11 }, 0, 0,  VE, "level" },
	{ "1.2",            "",                     0,              AV_OPT_TYPE_CONST, { .i64 = 12 }, 0, 0,  VE, "level" },
	{ "1.3",            "",                     0,              AV_OPT_TYPE_CONST, { .i64 = 13 }, 0, 0,  VE, "level" },
	{ "2.0",            "",                     0,              AV_OPT_TYPE_CONST, { .i64 = 20 }, 0, 0,  VE, "level" },
	{ "2.1",            "",                     0,              AV_OPT_TYPE_CONST, { .i64 = 21 }, 0, 0,  VE, "level" },
	{ "2.2",            "",                     0,              AV_OPT_TYPE_CONST, { .i64 = 22 }, 0, 0,  VE, "level" },
	{ "3.0",            "",                     0,              AV_OPT_TYPE_CONST, { .i64 = 30 }, 0, 0,  VE, "level" },
	{ "3.1",            "",                     0,              AV_OPT_TYPE_CONST, { .i64 = 31 }, 0, 0,  VE, "level" },
	{ "3.2",            "",                     0,              AV_OPT_TYPE_CONST, { .i64 = 32 }, 0, 0,  VE, "level" },
	{ "4.0",            "",                     0,              AV_OPT_TYPE_CONST, { .i64 = 40 }, 0, 0,  VE, "level" },
	{ "4.1",            "",                     0,              AV_OPT_TYPE_CONST, { .i64 = 41 }, 0, 0,  VE, "level" },
	{ "4.2",            "",                     0,              AV_OPT_TYPE_CONST, { .i64 = 42 }, 0, 0,  VE, "level" },
	{ "5.0",            "",                     0,              AV_OPT_TYPE_CONST, { .i64 = 50 }, 0, 0,  VE, "level" },
	{ "5.1",            "",                     0,              AV_OPT_TYPE_CONST, { .i64 = 51 }, 0, 0,  VE, "level" },

	{ "rc",           "Override the preset rate-control",   OFFSET(rc),           AV_OPT_TYPE_INT,   { .i64 = -1 },                                  -1, INT_MAX, VE, "rc" },
	{ "cbr",          "Constant bitrate mode",              0,                    AV_OPT_TYPE_CONST, { .i64 = 0 },                       0, 0, VE, "rc" },
	{ "vbr",          "Variable bitrate mode",              0,                    AV_OPT_TYPE_CONST, { .i64 = 1 },                       0, 0, VE, "rc" },

	{ "preset",          "Set the encoding preset",            OFFSET(preset),       AV_OPT_TYPE_INT,   { .i64 = 3 }, 1, 4, VE, "preset" },
	{ "default",         "",                                   0,                    AV_OPT_TYPE_CONST, { .i64 = 3 }, 0, 0, VE, "preset" },
	{ "slow",            "",                        0,                    AV_OPT_TYPE_CONST, { .i64 = 4 },            0, 0, VE, "preset" },
	{ "medium",          "",                        0,                    AV_OPT_TYPE_CONST, { .i64 = 3 },            0, 0, VE, "preset" },
	{ "fast",            "",                        0,                    AV_OPT_TYPE_CONST, { .i64 = 2 },            0, 0, VE, "preset" },
	{ "ultrafast",       "",                        0,                    AV_OPT_TYPE_CONST, { .i64 = 1 },            0, 0, VE, "preset" },
	{ NULL }
};


#define NVMPI_ENC_CLASS(NAME) \
	static const AVClass nvmpi_ ## NAME ## _enc_class = { \
		.class_name = #NAME "_nvmpi_encoder", \
		.item_name  = av_default_item_name, \
		.option     = options, \
		.version    = LIBAVUTIL_VERSION_INT, \
	};


#define NVMPI_ENC(NAME, LONGNAME, CODEC) \
	NVMPI_ENC_CLASS(NAME) \
	AVCodec ff_ ## NAME ## _nvmpi_encoder = { \
		.name           = #NAME "_nvmpi" , \
		.long_name      = NULL_IF_CONFIG_SMALL("nvmpi " LONGNAME " encoder wrapper"), \
		.type           = AVMEDIA_TYPE_VIDEO, \
		.id             = CODEC , \
		.priv_data_size = sizeof(nvmpiEncodeContext), \
		.priv_class     = &nvmpi_ ## NAME ##_enc_class, \
		.init           = nvmpi_encode_init, \
		.encode2        = nvmpi_encode_frame, \
		.close          = nvmpi_encode_close, \
		.pix_fmts       = (const enum AVPixelFormat[]) { AV_PIX_FMT_YUV420P, AV_PIX_FMT_NONE },\
		.capabilities   = AV_CODEC_CAP_HARDWARE | AV_CODEC_CAP_DELAY, \
		.defaults       = defaults,\
		.wrapper_name   = "nvmpi", \
	};

NVMPI_ENC(h264, "H.264", AV_CODEC_ID_H264);
NVMPI_ENC(hevc, "HEVC", AV_CODEC_ID_HEVC);

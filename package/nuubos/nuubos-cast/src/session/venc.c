/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "venc.h"
#include "common.h"

#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>

#include <vpx/vp8cx.h>
#include <vpx/vpx_encoder.h>

#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>

#include "third_party/openh264/codec_api.h"

struct venc {
	enum venc_codec codec;
	int width, height, fps, kbps;
	int64_t first_us;
	int64_t last_pts;

	/* VP8 */
	vpx_codec_ctx_t vpx;
	vpx_codec_enc_cfg_t vpx_cfg;
	bool vpx_open;

	/* MPEG-2 */
	AVCodecContext *av;
	AVFrame *frame;
	AVPacket *pkt;

	/* H.264 (OpenH264) */
	void *h264_lib;
	ISVCEncoder *h264;
	void (*h264_destroy)(ISVCEncoder *);
	struct bytes h264_out;
};

/* ------------------------------------------------------------------ */
/* VP8                                                                 */
/* ------------------------------------------------------------------ */

static int vp8_open(struct venc *v)
{
	vpx_codec_iface_t *iface = vpx_codec_vp8_cx();

	if (vpx_codec_enc_config_default(iface, &v->vpx_cfg, 0) != VPX_CODEC_OK)
		return -1;
	/* Real-time settings of the Cast reference sender (openscreen
	 * standalone_sender/streaming_vpx_encoder.cc). */
	v->vpx_cfg.g_w = (unsigned)v->width;
	v->vpx_cfg.g_h = (unsigned)v->height;
	v->vpx_cfg.g_timebase.num = 1;
	v->vpx_cfg.g_timebase.den = 1000000;
	v->vpx_cfg.g_threads = 2;
	v->vpx_cfg.g_pass = VPX_RC_ONE_PASS;
	v->vpx_cfg.g_lag_in_frames = 0;
	v->vpx_cfg.g_error_resilient = 0;
	v->vpx_cfg.rc_dropframe_thresh = 0;
	v->vpx_cfg.rc_resize_allowed = 0;
	v->vpx_cfg.rc_end_usage = VPX_CBR;
	v->vpx_cfg.rc_target_bitrate = (unsigned)v->kbps;
	v->vpx_cfg.rc_min_quantizer = 4;
	v->vpx_cfg.rc_max_quantizer = 63;
	v->vpx_cfg.rc_undershoot_pct = 100;
	v->vpx_cfg.rc_overshoot_pct = 15;
	v->vpx_cfg.rc_buf_initial_sz = 500;
	v->vpx_cfg.rc_buf_optimal_sz = 600;
	v->vpx_cfg.rc_buf_sz = 1000;
	/* Key frames only on demand (receiver picture loss) and the first. */
	v->vpx_cfg.kf_mode = VPX_KF_DISABLED;
	if (vpx_codec_enc_init(&v->vpx, iface, &v->vpx_cfg, 0) != VPX_CODEC_OK) {
		log_msg("vp8 init failed: %s", vpx_codec_error(&v->vpx));
		return -1;
	}
	v->vpx_open = true;
	/* Fastest real-time speed: the encoder shares four Cortex-A53 cores
	 * with the running game. */
	vpx_codec_control(&v->vpx, VP8E_SET_CPUUSED, -12);
	vpx_codec_control(&v->vpx, VP8E_SET_STATIC_THRESHOLD, 1);
	vpx_codec_control(&v->vpx, VP8E_SET_TOKEN_PARTITIONS, VP8_ONE_TOKENPARTITION);
	vpx_codec_control(&v->vpx, VP8E_SET_NOISE_SENSITIVITY, 0);
	return 0;
}

static int vp8_encode(struct venc *v, const struct picture *pic, bool force_key, struct venc_packet *out)
{
	vpx_image_t img;
	const vpx_codec_cx_pkt_t *pkt;
	vpx_codec_iter_t iter = NULL;
	int64_t pts = pic->time_us - v->first_us;
	int rc = 1;

	if (pts <= v->last_pts)
		pts = v->last_pts + 1;
	v->last_pts = pts;
	vpx_img_wrap(&img, VPX_IMG_FMT_I420, (unsigned)pic->width, (unsigned)pic->height, 1, pic->plane[0]);
	img.planes[0] = pic->plane[0];
	img.planes[1] = pic->plane[1];
	img.planes[2] = pic->plane[2];
	img.stride[0] = pic->stride[0];
	img.stride[1] = pic->stride[1];
	img.stride[2] = pic->stride[2];
	if (vpx_codec_encode(&v->vpx, &img, pts, 1000000 / (unsigned long)v->fps,
			     force_key ? VPX_EFLAG_FORCE_KF : 0, VPX_DL_REALTIME) != VPX_CODEC_OK) {
		log_msg("vp8 encode failed: %s", vpx_codec_error(&v->vpx));
		return -1;
	}
	while ((pkt = vpx_codec_get_cx_data(&v->vpx, &iter)) != NULL) {
		if (pkt->kind != VPX_CODEC_CX_FRAME_PKT)
			continue;
		out->data = pkt->data.frame.buf;
		out->len = pkt->data.frame.sz;
		out->key = (pkt->data.frame.flags & VPX_FRAME_IS_KEY) != 0;
		out->time_us = pic->time_us;
		rc = 0;
		break; /* one frame per picture (no lag) */
	}
	return rc;
}

/* ------------------------------------------------------------------ */
/* MPEG-2 (FFmpeg)                                                     */
/* ------------------------------------------------------------------ */

static int mpeg2_open(struct venc *v)
{
	const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_MPEG2VIDEO);

	if (!codec) {
		log_msg("FFmpeg has no MPEG-2 encoder");
		return -1;
	}
	v->av = avcodec_alloc_context3(codec);
	v->frame = av_frame_alloc();
	v->pkt = av_packet_alloc();
	if (!v->av || !v->frame || !v->pkt)
		return -1;
	v->av->width = v->width;
	v->av->height = v->height;
	v->av->time_base = (AVRational){ 1, v->fps };
	v->av->framerate = (AVRational){ v->fps, 1 };
	v->av->pix_fmt = AV_PIX_FMT_YUV420P;
	v->av->bit_rate = (int64_t)v->kbps * 1000;
	v->av->rc_max_rate = v->av->bit_rate * 3 / 2;
	v->av->rc_buffer_size = (int)(v->av->rc_max_rate / 2);
	v->av->gop_size = v->fps / 2; /* a TV joining late waits <= 0.5 s */
	v->av->max_b_frames = 0;
	v->av->thread_count = 2;
	v->av->thread_type = FF_THREAD_SLICE;
	v->av->flags |= AV_CODEC_FLAG_LOW_DELAY;
	v->av->color_range = AVCOL_RANGE_MPEG;
	v->av->colorspace = AVCOL_SPC_SMPTE170M;
	if (avcodec_open2(v->av, codec, NULL) < 0) {
		log_msg("MPEG-2 encoder init failed");
		return -1;
	}
	v->frame->format = AV_PIX_FMT_YUV420P;
	v->frame->width = v->width;
	v->frame->height = v->height;
	return 0;
}

static int mpeg2_encode(struct venc *v, const struct picture *pic, bool force_key, struct venc_packet *out)
{
	int64_t pts = (pic->time_us - v->first_us) * v->fps / 1000000;
	int ret;

	if (pts <= v->last_pts)
		pts = v->last_pts + 1;
	v->last_pts = pts;
	for (int i = 0; i < 3; i++) {
		v->frame->data[i] = pic->plane[i];
		v->frame->linesize[i] = pic->stride[i];
	}
	v->frame->pts = pts;
	v->frame->pict_type = force_key ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
	ret = avcodec_send_frame(v->av, v->frame);
	if (ret < 0)
		return -1;
	av_packet_unref(v->pkt);
	ret = avcodec_receive_packet(v->av, v->pkt);
	if (ret == AVERROR(EAGAIN))
		return 1;
	if (ret < 0)
		return -1;
	out->data = v->pkt->data;
	out->len = (size_t)v->pkt->size;
	out->key = (v->pkt->flags & AV_PKT_FLAG_KEY) != 0;
	out->time_us = pic->time_us;
	return 0;
}

/* ------------------------------------------------------------------ */
/* H.264 (Cisco OpenH264, loaded at run time)                          */
/* ------------------------------------------------------------------ */

static int h264_open(struct venc *v, const char *path)
{
	int (*create)(ISVCEncoder **);
	SEncParamExt param;
	int option;

	if (!path || !path[0]) {
		log_msg("H.264 needs Cisco's OpenH264 library");
		return -1;
	}
	v->h264_lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
	if (!v->h264_lib) {
		log_msg("cannot load %s: %s", path, dlerror());
		return -1;
	}
	*(void **)&create = dlsym(v->h264_lib, "WelsCreateSVCEncoder");
	*(void **)&v->h264_destroy = dlsym(v->h264_lib, "WelsDestroySVCEncoder");
	if (!create || !v->h264_destroy || create(&v->h264) != 0 || !v->h264) {
		log_msg("OpenH264 encoder unavailable");
		v->h264 = NULL;
		return -1;
	}
	(*v->h264)->GetDefaultParams(v->h264, &param);
	param.iUsageType = CAMERA_VIDEO_REAL_TIME;
	param.iPicWidth = v->width;
	param.iPicHeight = v->height;
	param.iTargetBitrate = v->kbps * 1000;
	param.iMaxBitrate = v->kbps * 1500;
	param.iRCMode = RC_BITRATE_MODE;
	param.fMaxFrameRate = (float)v->fps;
	param.iTemporalLayerNum = 1;
	param.iSpatialLayerNum = 1;
	/* OpenH264's rate control works only with frame skipping. */
	param.bEnableFrameSkip = true;
	param.bEnableDenoise = false;
	param.bEnableBackgroundDetection = true;
	param.bEnableAdaptiveQuant = true;
	param.bEnableLongTermReference = false;
	param.iEntropyCodingModeFlag = 0; /* CAVLC: Constrained Baseline */
	param.uiIntraPeriod = (unsigned)v->fps * 2;
	param.eSpsPpsIdStrategy = CONSTANT_ID;
	param.bPrefixNalAddingCtrl = false;
	param.iMultipleThreadIdc = 2;
	param.sSpatialLayers[0].iVideoWidth = v->width;
	param.sSpatialLayers[0].iVideoHeight = v->height;
	param.sSpatialLayers[0].fFrameRate = (float)v->fps;
	param.sSpatialLayers[0].iSpatialBitrate = param.iTargetBitrate;
	param.sSpatialLayers[0].iMaxSpatialBitrate = param.iMaxBitrate;
	param.sSpatialLayers[0].uiProfileIdc = PRO_BASELINE;
	param.sSpatialLayers[0].uiLevelIdc = LEVEL_3_1;
	param.sSpatialLayers[0].sSliceArgument.uiSliceMode = SM_SINGLE_SLICE;
	if ((*v->h264)->InitializeExt(v->h264, &param) != 0) {
		log_msg("OpenH264 init failed");
		return -1;
	}
	option = videoFormatI420;
	(*v->h264)->SetOption(v->h264, ENCODER_OPTION_DATAFORMAT, &option);
	return 0;
}

static int h264_encode(struct venc *v, const struct picture *pic, bool force_key, struct venc_packet *out)
{
	SSourcePicture src;
	SFrameBSInfo info;

	memset(&src, 0, sizeof(src));
	memset(&info, 0, sizeof(info));
	src.iColorFormat = videoFormatI420;
	src.iPicWidth = pic->width;
	src.iPicHeight = pic->height;
	for (int i = 0; i < 3; i++) {
		src.pData[i] = pic->plane[i];
		src.iStride[i] = pic->stride[i];
	}
	src.uiTimeStamp = (long long)((pic->time_us - v->first_us) / 1000);
	if (force_key)
		(*v->h264)->ForceIntraFrame(v->h264, true);
	if ((*v->h264)->EncodeFrame(v->h264, &src, &info) != cmResultSuccess)
		return -1;
	if (info.eFrameType == videoFrameTypeSkip || info.eFrameType == videoFrameTypeInvalid)
		return 1;
	v->h264_out.len = 0;
	for (int l = 0; l < info.iLayerNum; l++) {
		const SLayerBSInfo *layer = &info.sLayerInfo[l];
		size_t size = 0;

		for (int n = 0; n < layer->iNalCount; n++)
			size += (size_t)layer->pNalLengthInByte[n];
		/* NAL units come with Annex B start codes. */
		if (bytes_append(&v->h264_out, layer->pBsBuf, size) != 0)
			return -1;
	}
	out->data = v->h264_out.data;
	out->len = v->h264_out.len;
	out->key = info.eFrameType == videoFrameTypeIDR;
	out->time_us = pic->time_us;
	return 0;
}

/* ------------------------------------------------------------------ */

struct venc *venc_open(enum venc_codec codec, int width, int height, int fps, int kbps,
		       const char *openh264)
{
	struct venc *v = calloc(1, sizeof(*v));
	int rc = -1;

	if (!v)
		return NULL;
	v->codec = codec;
	v->width = width;
	v->height = height;
	v->fps = fps;
	v->kbps = kbps;
	v->first_us = now_us();
	v->last_pts = -1;
	switch (codec) {
	case VENC_VP8:
		rc = vp8_open(v);
		break;
	case VENC_MPEG2:
		rc = mpeg2_open(v);
		break;
	case VENC_H264:
		rc = h264_open(v, openh264);
		break;
	}
	if (rc != 0) {
		venc_close(v);
		return NULL;
	}
	log_msg("video encoder %s %dx%d@%d %d kb/s",
		codec == VENC_VP8 ? "vp8" : codec == VENC_MPEG2 ? "mpeg2" : "h264",
		width, height, fps, kbps);
	return v;
}

void venc_close(struct venc *v)
{
	if (!v)
		return;
	if (v->vpx_open)
		vpx_codec_destroy(&v->vpx);
	avcodec_free_context(&v->av);
	av_frame_free(&v->frame);
	av_packet_free(&v->pkt);
	if (v->h264) {
		(*v->h264)->Uninitialize(v->h264);
		v->h264_destroy(v->h264);
	}
	if (v->h264_lib)
		dlclose(v->h264_lib);
	bytes_free(&v->h264_out);
	free(v);
}

int venc_encode(struct venc *v, const struct picture *pic, bool force_key, struct venc_packet *out)
{
	switch (v->codec) {
	case VENC_VP8:
		return vp8_encode(v, pic, force_key, out);
	case VENC_MPEG2:
		return mpeg2_encode(v, pic, force_key, out);
	case VENC_H264:
		return h264_encode(v, pic, force_key, out);
	}
	return -1;
}

void venc_set_bitrate(struct venc *v, int kbps)
{
	if (kbps == v->kbps)
		return;
	v->kbps = kbps;
	if (v->codec == VENC_VP8) {
		v->vpx_cfg.rc_target_bitrate = (unsigned)kbps;
		vpx_codec_enc_config_set(&v->vpx, &v->vpx_cfg);
	} else if (v->codec == VENC_H264) {
		SBitrateInfo info = { SPATIAL_LAYER_ALL, kbps * 1000 };

		(*v->h264)->SetOption(v->h264, ENCODER_OPTION_BITRATE, &info);
	}
	/* MPEG-2 keeps its rate: DLNA renderers buffer generously. */
}

int venc_bitrate(const struct venc *v)
{
	return v->kbps;
}

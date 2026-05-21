// SPDX-License-Identifier: GPL-2.0
/*
 * Allwinner sunxi H264 hardware encoder driver — V4L2 control handler.
 *
 * Exposes the standard V4L2 H264 encoder controls (bitrate, GOP, QP,
 * profile/level, entropy, force-keyframe). Values land in ctx->params and
 * are consumed per frame by the encoder core (sunxi_venc_h264.c).
 */

#include <media/v4l2-ctrls.h>

#include "sunxi_venc.h"

/* Map the V4L2 H264 level menu index to the bitstream level_idc value. */
static unsigned int sunxi_venc_level_idc(s32 menu)
{
	switch (menu) {
	case V4L2_MPEG_VIDEO_H264_LEVEL_1_0:	return 10;
	case V4L2_MPEG_VIDEO_H264_LEVEL_1B:	return 11;
	case V4L2_MPEG_VIDEO_H264_LEVEL_1_1:	return 11;
	case V4L2_MPEG_VIDEO_H264_LEVEL_1_2:	return 12;
	case V4L2_MPEG_VIDEO_H264_LEVEL_1_3:	return 13;
	case V4L2_MPEG_VIDEO_H264_LEVEL_2_0:	return 20;
	case V4L2_MPEG_VIDEO_H264_LEVEL_2_1:	return 21;
	case V4L2_MPEG_VIDEO_H264_LEVEL_2_2:	return 22;
	case V4L2_MPEG_VIDEO_H264_LEVEL_3_0:	return 30;
	case V4L2_MPEG_VIDEO_H264_LEVEL_3_1:	return 31;
	case V4L2_MPEG_VIDEO_H264_LEVEL_3_2:	return 32;
	case V4L2_MPEG_VIDEO_H264_LEVEL_4_0:	return 40;
	case V4L2_MPEG_VIDEO_H264_LEVEL_4_1:	return 41;
	case V4L2_MPEG_VIDEO_H264_LEVEL_4_2:	return 42;
	default:				return 30;
	}
}

static int sunxi_venc_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct sunxi_venc_ctx *ctx =
		container_of(ctrl->handler, struct sunxi_venc_ctx, hdl);
	struct sunxi_venc_params *p = &ctx->params;

	switch (ctrl->id) {
	case V4L2_CID_MPEG_VIDEO_BITRATE:
		p->bitrate = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_BITRATE_MODE:
		p->cbr = ctrl->val == V4L2_MPEG_VIDEO_BITRATE_MODE_CBR;
		break;
	case V4L2_CID_MPEG_VIDEO_GOP_SIZE:
		p->gop_size = ctrl->val ? ctrl->val : 1;
		break;
	case V4L2_CID_MPEG_VIDEO_H264_I_FRAME_QP:
		p->qp_i = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_H264_P_FRAME_QP:
		p->qp_p = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_H264_MIN_QP:
		p->qp_min = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_H264_MAX_QP:
		p->qp_max = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_H264_PROFILE:
		switch (ctrl->val) {
		case V4L2_MPEG_VIDEO_H264_PROFILE_MAIN:
			p->profile_idc = 77;
			p->constraints = 0x00;
			break;
		case V4L2_MPEG_VIDEO_H264_PROFILE_HIGH:
			p->profile_idc = 100;
			p->constraints = 0x00;
			break;
		case V4L2_MPEG_VIDEO_H264_PROFILE_CONSTRAINED_BASELINE:
			p->profile_idc = 66;
			p->constraints = 0x40;	/* constraint_set1_flag */
			break;
		default:
			p->profile_idc = 66;
			p->constraints = 0x00;
			break;
		}
		break;
	case V4L2_CID_MPEG_VIDEO_H264_LEVEL:
		p->level_idc = sunxi_venc_level_idc(ctrl->val);
		break;
	case V4L2_CID_MPEG_VIDEO_H264_ENTROPY_MODE:
		p->cabac = ctrl->val == V4L2_MPEG_VIDEO_H264_ENTROPY_MODE_CABAC;
		break;
	case V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME:
		ctx->force_keyframe = true;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static const struct v4l2_ctrl_ops sunxi_venc_ctrl_ops = {
	.s_ctrl = sunxi_venc_s_ctrl,
};

int sunxi_venc_ctrls_setup(struct sunxi_venc_ctx *ctx)
{
	struct v4l2_ctrl_handler *hdl = &ctx->hdl;
	struct v4l2_ctrl *ctrl;
	int ret;

	/* Parameter defaults — must match the control defaults registered below. */
	ctx->params = (struct sunxi_venc_params){
		.gop_size	= 30,
		.bitrate	= 2000000,
		.framerate	= 30,
		.cbr		= false,
		.qp_i		= 26,
		.qp_p		= 28,
		.qp_min		= 10,
		.qp_max		= 48,
		.profile_idc	= 66,
		.constraints	= 0x40,
		.level_idc	= 30,
		.cabac		= false,
	};

	v4l2_ctrl_handler_init(hdl, 12);

	v4l2_ctrl_new_std(hdl, &sunxi_venc_ctrl_ops,
			  V4L2_CID_MPEG_VIDEO_BITRATE,
			  16000, 60000000, 1000, 2000000);
	v4l2_ctrl_new_std_menu(hdl, &sunxi_venc_ctrl_ops,
			       V4L2_CID_MPEG_VIDEO_BITRATE_MODE,
			       V4L2_MPEG_VIDEO_BITRATE_MODE_VBR, 0,
			       V4L2_MPEG_VIDEO_BITRATE_MODE_VBR);
	v4l2_ctrl_new_std(hdl, &sunxi_venc_ctrl_ops,
			  V4L2_CID_MPEG_VIDEO_GOP_SIZE, 1, 300, 1, 30);

	v4l2_ctrl_new_std(hdl, &sunxi_venc_ctrl_ops,
			  V4L2_CID_MPEG_VIDEO_H264_I_FRAME_QP, 1, 51, 1, 26);
	v4l2_ctrl_new_std(hdl, &sunxi_venc_ctrl_ops,
			  V4L2_CID_MPEG_VIDEO_H264_P_FRAME_QP, 1, 51, 1, 28);
	v4l2_ctrl_new_std(hdl, &sunxi_venc_ctrl_ops,
			  V4L2_CID_MPEG_VIDEO_H264_MIN_QP, 1, 51, 1, 10);
	v4l2_ctrl_new_std(hdl, &sunxi_venc_ctrl_ops,
			  V4L2_CID_MPEG_VIDEO_H264_MAX_QP, 1, 51, 1, 48);

	/* Profile menu: baseline / constrained-baseline / main / high
	 * (skip EXTENDED — bit 3 — which the VE does not implement). */
	v4l2_ctrl_new_std_menu(hdl, &sunxi_venc_ctrl_ops,
			       V4L2_CID_MPEG_VIDEO_H264_PROFILE,
			       V4L2_MPEG_VIDEO_H264_PROFILE_HIGH,
			       BIT(V4L2_MPEG_VIDEO_H264_PROFILE_EXTENDED),
			       V4L2_MPEG_VIDEO_H264_PROFILE_CONSTRAINED_BASELINE);
	v4l2_ctrl_new_std_menu(hdl, &sunxi_venc_ctrl_ops,
			       V4L2_CID_MPEG_VIDEO_H264_LEVEL,
			       V4L2_MPEG_VIDEO_H264_LEVEL_4_2, 0,
			       V4L2_MPEG_VIDEO_H264_LEVEL_3_0);
	v4l2_ctrl_new_std_menu(hdl, &sunxi_venc_ctrl_ops,
			       V4L2_CID_MPEG_VIDEO_H264_ENTROPY_MODE,
			       V4L2_MPEG_VIDEO_H264_ENTROPY_MODE_CABAC, 0,
			       V4L2_MPEG_VIDEO_H264_ENTROPY_MODE_CAVLC);

	v4l2_ctrl_new_std(hdl, &sunxi_venc_ctrl_ops,
			  V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME, 0, 0, 0, 0);

	/* Stateful-encoder requirement: report the minimum number of
	 * OUTPUT buffers. The encoder has no frame delay, so one is enough. */
	ctrl = v4l2_ctrl_new_std(hdl, &sunxi_venc_ctrl_ops,
				 V4L2_CID_MIN_BUFFERS_FOR_OUTPUT, 1, 1, 1, 1);
	if (ctrl)
		ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	if (hdl->error) {
		ret = hdl->error;
		v4l2_ctrl_handler_free(hdl);
		return ret;
	}

	ctx->fh.ctrl_handler = hdl;
	return 0;
}

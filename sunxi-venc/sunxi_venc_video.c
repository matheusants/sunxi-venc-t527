// SPDX-License-Identifier: GPL-2.0
/*
 * Allwinner sunxi H264 hardware encoder driver — V4L2 video ops.
 *
 * Format negotiation and vb2 queue handling for the stateful M2M encoder.
 */

#include <linux/pm_runtime.h>

#include <media/v4l2-device.h>
#include <media/v4l2-event.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-mem2mem.h>
#include <media/videobuf2-dma-contig.h>

#include "sunxi_venc.h"

static const struct sunxi_venc_fmt sunxi_venc_formats[] = {
	{
		/* raw input */
		.pixelformat	= V4L2_PIX_FMT_NV12,
		.directions	= SUNXI_VENC_FMT_OUTPUT,
	},
	{
		/* coded output */
		.pixelformat	= V4L2_PIX_FMT_H264,
		.directions	= SUNXI_VENC_FMT_CAPTURE,
	},
};

#define SUNXI_VENC_FORMATS_COUNT	ARRAY_SIZE(sunxi_venc_formats)

static inline struct sunxi_venc_ctx *sunxi_venc_file2ctx(struct file *file)
{
	return container_of(file->private_data, struct sunxi_venc_ctx, fh);
}

static const struct sunxi_venc_fmt *sunxi_venc_find_format(u32 pixelformat,
							   u32 directions)
{
	unsigned int i;

	for (i = 0; i < SUNXI_VENC_FORMATS_COUNT; i++) {
		const struct sunxi_venc_fmt *fmt = &sunxi_venc_formats[i];

		if ((fmt->directions & directions) &&
		    fmt->pixelformat == pixelformat)
			return fmt;
	}

	return NULL;
}

void sunxi_venc_prepare_format(struct v4l2_pix_format *pix_fmt)
{
	unsigned int width = pix_fmt->width;
	unsigned int height = pix_fmt->height;
	unsigned int sizeimage;
	unsigned int bytesperline;

	pix_fmt->field = V4L2_FIELD_NONE;

	width = clamp(width, SUNXI_VENC_MIN_WIDTH, SUNXI_VENC_MAX_WIDTH);
	height = clamp(height, SUNXI_VENC_MIN_HEIGHT, SUNXI_VENC_MAX_HEIGHT);

	switch (pix_fmt->pixelformat) {
	case V4L2_PIX_FMT_H264:
		/* Coded output: no stride; size is a generous worst-case cap. */
		bytesperline = 0;
		sizeimage = max_t(u32, SZ_512K,
				  ALIGN(width, 16) * ALIGN(height, 16) * 3 / 2);
		break;

	case V4L2_PIX_FMT_NV12:
	default:
		/* 16-aligned NV12 raw frame. */
		bytesperline = ALIGN(width, 16);
		height = ALIGN(height, 16);
		sizeimage = bytesperline * height * 3 / 2;
		break;
	}

	pix_fmt->width = width;
	pix_fmt->height = height;
	pix_fmt->bytesperline = bytesperline;
	pix_fmt->sizeimage = sizeimage;
}

static int sunxi_venc_querycap(struct file *file, void *priv,
			       struct v4l2_capability *cap)
{
	strscpy(cap->driver, SUNXI_VENC_NAME, sizeof(cap->driver));
	strscpy(cap->card, SUNXI_VENC_NAME, sizeof(cap->card));
	snprintf(cap->bus_info, sizeof(cap->bus_info),
		 "platform:%s", SUNXI_VENC_NAME);

	return 0;
}

static int sunxi_venc_enum_fmt(struct v4l2_fmtdesc *f, u32 direction)
{
	unsigned int i, index = 0;

	for (i = 0; i < SUNXI_VENC_FORMATS_COUNT; i++) {
		const struct sunxi_venc_fmt *fmt = &sunxi_venc_formats[i];

		if (!(fmt->directions & direction))
			continue;

		if (index == f->index) {
			f->pixelformat = fmt->pixelformat;
			return 0;
		}

		index++;
	}

	return -EINVAL;
}

static int sunxi_venc_enum_fmt_vid_cap(struct file *file, void *priv,
				       struct v4l2_fmtdesc *f)
{
	return sunxi_venc_enum_fmt(f, SUNXI_VENC_FMT_CAPTURE);
}

static int sunxi_venc_enum_fmt_vid_out(struct file *file, void *priv,
				       struct v4l2_fmtdesc *f)
{
	return sunxi_venc_enum_fmt(f, SUNXI_VENC_FMT_OUTPUT);
}

static int sunxi_venc_g_fmt_vid_cap(struct file *file, void *priv,
				    struct v4l2_format *f)
{
	f->fmt.pix = sunxi_venc_file2ctx(file)->dst_fmt;
	return 0;
}

static int sunxi_venc_g_fmt_vid_out(struct file *file, void *priv,
				    struct v4l2_format *f)
{
	f->fmt.pix = sunxi_venc_file2ctx(file)->src_fmt;
	return 0;
}

static int sunxi_venc_try_fmt_vid_cap(struct file *file, void *priv,
				      struct v4l2_format *f)
{
	struct sunxi_venc_ctx *ctx = sunxi_venc_file2ctx(file);
	struct v4l2_pix_format *pix_fmt = &f->fmt.pix;

	if (!sunxi_venc_find_format(pix_fmt->pixelformat, SUNXI_VENC_FMT_CAPTURE))
		pix_fmt->pixelformat = V4L2_PIX_FMT_H264;

	/* Coded resolution follows the raw input. */
	pix_fmt->width = ctx->src_fmt.width;
	pix_fmt->height = ctx->src_fmt.height;
	sunxi_venc_prepare_format(pix_fmt);

	return 0;
}

static int sunxi_venc_try_fmt_vid_out(struct file *file, void *priv,
				      struct v4l2_format *f)
{
	struct v4l2_pix_format *pix_fmt = &f->fmt.pix;

	if (!sunxi_venc_find_format(pix_fmt->pixelformat, SUNXI_VENC_FMT_OUTPUT))
		pix_fmt->pixelformat = V4L2_PIX_FMT_NV12;

	sunxi_venc_prepare_format(pix_fmt);

	return 0;
}

static int sunxi_venc_s_fmt_vid_cap(struct file *file, void *priv,
				    struct v4l2_format *f)
{
	struct sunxi_venc_ctx *ctx = sunxi_venc_file2ctx(file);
	struct vb2_queue *vq;
	int ret;

	vq = v4l2_m2m_get_vq(ctx->fh.m2m_ctx, f->type);
	if (vb2_is_busy(vq))
		return -EBUSY;

	ret = sunxi_venc_try_fmt_vid_cap(file, priv, f);
	if (ret)
		return ret;

	ctx->dst_fmt = f->fmt.pix;
	return 0;
}

static int sunxi_venc_s_fmt_vid_out(struct file *file, void *priv,
				    struct v4l2_format *f)
{
	struct sunxi_venc_ctx *ctx = sunxi_venc_file2ctx(file);
	struct vb2_queue *vq, *peer_vq;
	int ret;

	vq = v4l2_m2m_get_vq(ctx->fh.m2m_ctx, f->type);
	if (vb2_is_busy(vq))
		return -EBUSY;

	/* Changing the raw format resets the coded queue. */
	peer_vq = v4l2_m2m_get_vq(ctx->fh.m2m_ctx, V4L2_BUF_TYPE_VIDEO_CAPTURE);
	if (vb2_is_busy(peer_vq))
		return -EBUSY;

	ret = sunxi_venc_try_fmt_vid_out(file, priv, f);
	if (ret)
		return ret;

	ctx->src_fmt = f->fmt.pix;

	/* Default visible rect = the whole coded frame (no SPS crop); a
	 * later VIDIOC_S_SELECTION narrows it to the real picture size. */
	ctx->crop.left = 0;
	ctx->crop.top = 0;
	ctx->crop.width = ctx->src_fmt.width;
	ctx->crop.height = ctx->src_fmt.height;

	/* Propagate dimensions + colour info to the coded queue. */
	ctx->dst_fmt.width = ctx->src_fmt.width;
	ctx->dst_fmt.height = ctx->src_fmt.height;
	ctx->dst_fmt.colorspace = f->fmt.pix.colorspace;
	ctx->dst_fmt.xfer_func = f->fmt.pix.xfer_func;
	ctx->dst_fmt.ycbcr_enc = f->fmt.pix.ycbcr_enc;
	ctx->dst_fmt.quantization = f->fmt.pix.quantization;
	sunxi_venc_prepare_format(&ctx->dst_fmt);

	return 0;
}

/* Frame rate via timeperframe on the OUTPUT queue — feeds rate control. */
static int sunxi_venc_g_parm(struct file *file, void *priv,
			     struct v4l2_streamparm *a)
{
	struct sunxi_venc_ctx *ctx = sunxi_venc_file2ctx(file);

	if (a->type != V4L2_BUF_TYPE_VIDEO_OUTPUT)
		return -EINVAL;

	a->parm.output.capability = V4L2_CAP_TIMEPERFRAME;
	a->parm.output.timeperframe.numerator = 1;
	a->parm.output.timeperframe.denominator = ctx->params.framerate;

	return 0;
}

static int sunxi_venc_s_parm(struct file *file, void *priv,
			     struct v4l2_streamparm *a)
{
	struct sunxi_venc_ctx *ctx = sunxi_venc_file2ctx(file);
	struct v4l2_fract *tpf;

	if (a->type != V4L2_BUF_TYPE_VIDEO_OUTPUT)
		return -EINVAL;

	tpf = &a->parm.output.timeperframe;
	if (tpf->numerator && tpf->denominator)
		ctx->params.framerate =
			clamp(tpf->denominator / tpf->numerator, 1U, 240U);
	else
		ctx->params.framerate = 30;

	tpf->numerator = 1;
	tpf->denominator = ctx->params.framerate;
	a->parm.output.capability = V4L2_CAP_TIMEPERFRAME;

	return 0;
}

/*
 * A stateful encoder must let userspace subscribe to V4L2_EVENT_EOS so it
 * can detect the end of a drain (VIDIOC_ENCODER_CMD STOP).
 */
/*
 * Frame-size enumeration. A stateful encoder must implement this
 * (v4l2-compliance testEnumFrameSizes). The VE works in 16-pixel
 * macroblocks; report a stepwise range for both the raw and coded
 * formats.
 */
static int sunxi_venc_enum_framesizes(struct file *file, void *priv,
				      struct v4l2_frmsizeenum *fsize)
{
	if (fsize->index != 0)
		return -EINVAL;
	if (fsize->pixel_format != V4L2_PIX_FMT_H264 &&
	    fsize->pixel_format != V4L2_PIX_FMT_NV12)
		return -EINVAL;

	fsize->type = V4L2_FRMSIZE_TYPE_STEPWISE;
	fsize->stepwise.min_width = 16;
	fsize->stepwise.max_width = 3840;
	fsize->stepwise.step_width = 16;
	fsize->stepwise.min_height = 16;
	fsize->stepwise.max_height = 2160;
	fsize->stepwise.step_height = 16;
	return 0;
}

static int sunxi_venc_subscribe_event(struct v4l2_fh *fh,
				      const struct v4l2_event_subscription *sub)
{
	switch (sub->type) {
	case V4L2_EVENT_EOS:
		return v4l2_event_subscribe(fh, sub, 0, NULL);
	case V4L2_EVENT_CTRL:
		return v4l2_ctrl_subscribe_event(fh, sub);
	default:
		return -EINVAL;
	}
}

static int sunxi_venc_g_selection(struct file *file, void *priv,
				  struct v4l2_selection *s)
{
	struct sunxi_venc_ctx *ctx = sunxi_venc_file2ctx(file);

	if (s->type != V4L2_BUF_TYPE_VIDEO_OUTPUT)
		return -EINVAL;

	switch (s->target) {
	case V4L2_SEL_TGT_CROP:
		s->r = ctx->crop;
		break;
	case V4L2_SEL_TGT_CROP_DEFAULT:
	case V4L2_SEL_TGT_CROP_BOUNDS:
		s->r.left = 0;
		s->r.top = 0;
		s->r.width = ctx->src_fmt.width;
		s->r.height = ctx->src_fmt.height;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int sunxi_venc_s_selection(struct file *file, void *priv,
				  struct v4l2_selection *s)
{
	struct sunxi_venc_ctx *ctx = sunxi_venc_file2ctx(file);

	if (s->type != V4L2_BUF_TYPE_VIDEO_OUTPUT)
		return -EINVAL;
	if (s->target != V4L2_SEL_TGT_CROP)
		return -EINVAL;

	/* The visible rect can only shrink the coded frame, never grow it. */
	s->r.left = 0;
	s->r.top = 0;
	if (s->r.width == 0 || s->r.width > ctx->src_fmt.width)
		s->r.width = ctx->src_fmt.width;
	if (s->r.height == 0 || s->r.height > ctx->src_fmt.height)
		s->r.height = ctx->src_fmt.height;

	ctx->crop = s->r;

	return 0;
}

const struct v4l2_ioctl_ops sunxi_venc_ioctl_ops = {
	.vidioc_querycap		= sunxi_venc_querycap,

	.vidioc_enum_fmt_vid_cap	= sunxi_venc_enum_fmt_vid_cap,
	.vidioc_g_fmt_vid_cap		= sunxi_venc_g_fmt_vid_cap,
	.vidioc_try_fmt_vid_cap		= sunxi_venc_try_fmt_vid_cap,
	.vidioc_s_fmt_vid_cap		= sunxi_venc_s_fmt_vid_cap,

	.vidioc_enum_fmt_vid_out	= sunxi_venc_enum_fmt_vid_out,
	.vidioc_g_fmt_vid_out		= sunxi_venc_g_fmt_vid_out,
	.vidioc_try_fmt_vid_out		= sunxi_venc_try_fmt_vid_out,
	.vidioc_s_fmt_vid_out		= sunxi_venc_s_fmt_vid_out,

	.vidioc_g_selection		= sunxi_venc_g_selection,
	.vidioc_s_selection		= sunxi_venc_s_selection,

	.vidioc_reqbufs			= v4l2_m2m_ioctl_reqbufs,
	.vidioc_querybuf		= v4l2_m2m_ioctl_querybuf,
	.vidioc_qbuf			= v4l2_m2m_ioctl_qbuf,
	.vidioc_dqbuf			= v4l2_m2m_ioctl_dqbuf,
	.vidioc_prepare_buf		= v4l2_m2m_ioctl_prepare_buf,
	.vidioc_create_bufs		= v4l2_m2m_ioctl_create_bufs,
	.vidioc_expbuf			= v4l2_m2m_ioctl_expbuf,

	.vidioc_g_parm			= sunxi_venc_g_parm,
	.vidioc_s_parm			= sunxi_venc_s_parm,

	.vidioc_streamon		= v4l2_m2m_ioctl_streamon,
	.vidioc_streamoff		= v4l2_m2m_ioctl_streamoff,

	.vidioc_try_encoder_cmd		= v4l2_m2m_ioctl_try_encoder_cmd,
	.vidioc_encoder_cmd		= v4l2_m2m_ioctl_encoder_cmd,

	.vidioc_enum_framesizes		= sunxi_venc_enum_framesizes,
	.vidioc_subscribe_event		= sunxi_venc_subscribe_event,
	.vidioc_unsubscribe_event	= v4l2_event_unsubscribe,
};

static int sunxi_venc_queue_setup(struct vb2_queue *vq, unsigned int *nbufs,
				  unsigned int *nplanes, unsigned int sizes[],
				  struct device *alloc_devs[])
{
	struct sunxi_venc_ctx *ctx = vb2_get_drv_priv(vq);
	struct v4l2_pix_format *pix_fmt;

	if (V4L2_TYPE_IS_OUTPUT(vq->type))
		pix_fmt = &ctx->src_fmt;
	else
		pix_fmt = &ctx->dst_fmt;

	if (*nplanes) {
		if (sizes[0] < pix_fmt->sizeimage)
			return -EINVAL;
	} else {
		sizes[0] = pix_fmt->sizeimage;
		*nplanes = 1;
	}

	return 0;
}

static int sunxi_venc_buf_prepare(struct vb2_buffer *vb)
{
	struct vb2_queue *vq = vb->vb2_queue;
	struct sunxi_venc_ctx *ctx = vb2_get_drv_priv(vq);
	struct v4l2_pix_format *pix_fmt;

	if (V4L2_TYPE_IS_OUTPUT(vq->type))
		pix_fmt = &ctx->src_fmt;
	else
		pix_fmt = &ctx->dst_fmt;

	if (vb2_plane_size(vb, 0) < pix_fmt->sizeimage)
		return -EINVAL;

	if (V4L2_TYPE_IS_OUTPUT(vq->type))
		vb2_set_plane_payload(vb, 0, pix_fmt->sizeimage);

	return 0;
}

static int sunxi_venc_buf_out_validate(struct vb2_buffer *vb)
{
	to_vb2_v4l2_buffer(vb)->field = V4L2_FIELD_NONE;
	return 0;
}

static void sunxi_venc_buf_queue(struct vb2_buffer *vb)
{
	struct sunxi_venc_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);

	v4l2_m2m_buf_queue(ctx->fh.m2m_ctx, to_vb2_v4l2_buffer(vb));
}

static void sunxi_venc_queue_cleanup(struct vb2_queue *vq, u32 state)
{
	struct sunxi_venc_ctx *ctx = vb2_get_drv_priv(vq);
	struct vb2_v4l2_buffer *vbuf;

	for (;;) {
		if (V4L2_TYPE_IS_OUTPUT(vq->type))
			vbuf = v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx);
		else
			vbuf = v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx);

		if (!vbuf)
			return;

		v4l2_m2m_buf_done(vbuf, state);
	}
}

static int sunxi_venc_start_streaming(struct vb2_queue *vq, unsigned int count)
{
	struct sunxi_venc_ctx *ctx = vb2_get_drv_priv(vq);
	struct sunxi_venc_dev *dev = ctx->dev;
	int ret;

	if (!V4L2_TYPE_IS_OUTPUT(vq->type))
		return 0;

	ctx->frame_num = 0;

	ret = pm_runtime_resume_and_get(dev->dev);
	if (ret < 0) {
		sunxi_venc_queue_cleanup(vq, VB2_BUF_STATE_QUEUED);
		return ret;
	}

	ret = sunxi_venc_h264_start(ctx);
	if (ret) {
		pm_runtime_put(dev->dev);
		sunxi_venc_queue_cleanup(vq, VB2_BUF_STATE_QUEUED);
		return ret;
	}

	return 0;
}

static void sunxi_venc_stop_streaming(struct vb2_queue *vq)
{
	struct sunxi_venc_ctx *ctx = vb2_get_drv_priv(vq);
	struct sunxi_venc_dev *dev = ctx->dev;

	if (V4L2_TYPE_IS_OUTPUT(vq->type)) {
		sunxi_venc_h264_stop(ctx);
		pm_runtime_put(dev->dev);
	}

	sunxi_venc_queue_cleanup(vq, VB2_BUF_STATE_ERROR);
}

static const struct vb2_ops sunxi_venc_qops = {
	.queue_setup		= sunxi_venc_queue_setup,
	.buf_prepare		= sunxi_venc_buf_prepare,
	.buf_queue		= sunxi_venc_buf_queue,
	.buf_out_validate	= sunxi_venc_buf_out_validate,
	.start_streaming	= sunxi_venc_start_streaming,
	.stop_streaming		= sunxi_venc_stop_streaming,
	.wait_prepare		= vb2_ops_wait_prepare,
	.wait_finish		= vb2_ops_wait_finish,
};

int sunxi_venc_queue_init(void *priv, struct vb2_queue *src_vq,
			  struct vb2_queue *dst_vq)
{
	struct sunxi_venc_ctx *ctx = priv;
	int ret;

	src_vq->type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	src_vq->io_modes = VB2_MMAP | VB2_DMABUF | VB2_USERPTR;
	src_vq->drv_priv = ctx;
	src_vq->buf_struct_size = sizeof(struct v4l2_m2m_buffer);
	src_vq->min_buffers_needed = 1;
	src_vq->ops = &sunxi_venc_qops;
	src_vq->mem_ops = &vb2_dma_contig_memops;
	src_vq->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	src_vq->lock = &ctx->dev->dev_mutex;
	src_vq->dev = ctx->dev->dev;

	ret = vb2_queue_init(src_vq);
	if (ret)
		return ret;

	dst_vq->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	dst_vq->io_modes = VB2_MMAP | VB2_DMABUF | VB2_USERPTR;
	dst_vq->drv_priv = ctx;
	dst_vq->buf_struct_size = sizeof(struct v4l2_m2m_buffer);
	dst_vq->min_buffers_needed = 1;
	dst_vq->ops = &sunxi_venc_qops;
	dst_vq->mem_ops = &vb2_dma_contig_memops;
	dst_vq->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	dst_vq->lock = &ctx->dev->dev_mutex;
	dst_vq->dev = ctx->dev->dev;

	return vb2_queue_init(dst_vq);
}

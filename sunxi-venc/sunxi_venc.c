// SPDX-License-Identifier: GPL-2.0
/*
 * Allwinner sunxi H264 hardware encoder driver.
 *
 * Stateful V4L2 M2M encoder for the AVC encode engine of the Allwinner T527
 * Video Engine. Path B of the T527 hardware-video project.
 *
 * The VE register block is shared with the cedrus decoder driver, so the
 * registers are mapped with devm_ioremap() (no exclusive region request).
 * Concurrent decode+encode is not supported in this version.
 */

#include <linux/clk.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/pm_runtime.h>
#include <linux/reset.h>

#include <media/v4l2-device.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-mem2mem.h>

#include "sunxi_venc.h"

#include <linux/semaphore.h>

extern struct semaphore sunxi_ve_sem;
extern void sunxi_ve_irq_disable(void);
extern void sunxi_ve_irq_enable(void);

/*
 * The VE reset line is held exclusively by the cedrus decoder driver, so this
 * driver cannot request it through the reset framework (dev->rstc stays NULL).
 * Deassert RST_BUS_VE directly via the CCU instead — same workaround the
 * cedar_ve BSP encoder uses (userpatches patch 0010).
 */
#define VENC_CCU_RST_BUS_VE		(0x02001000 + 0x069c)
#define VENC_CCU_RST_BUS_VE_BIT		BIT(16)

static void sunxi_venc_ccu_deassert_ve(struct sunxi_venc_dev *dev)
{
	void __iomem *ccu;

	if (dev->rstc)
		return;

	ccu = ioremap(VENC_CCU_RST_BUS_VE, 4);
	if (!ccu) {
		dev_warn(dev->dev, "Failed to map CCU reset register\n");
		return;
	}

	writel(readl(ccu) | VENC_CCU_RST_BUS_VE_BIT, ccu);
	iounmap(ccu);
}

/* ---- hardware bring-up ---- */

int sunxi_venc_hw_suspend(struct device *device)
{
	struct sunxi_venc_dev *dev = dev_get_drvdata(device);

	clk_disable_unprepare(dev->ram_clk);
	clk_disable_unprepare(dev->mod_clk);
	clk_disable_unprepare(dev->ahb_clk);

	reset_control_assert(dev->rstc);

	return 0;
}

int sunxi_venc_hw_resume(struct device *device)
{
	struct sunxi_venc_dev *dev = dev_get_drvdata(device);
	int ret;

	ret = reset_control_deassert(dev->rstc);
	if (ret) {
		dev_err(dev->dev, "Failed to deassert reset\n");
		return ret;
	}
	sunxi_venc_ccu_deassert_ve(dev);

	ret = clk_prepare_enable(dev->ahb_clk);
	if (ret) {
		dev_err(dev->dev, "Failed to enable AHB clock\n");
		goto err_rst;
	}

	ret = clk_prepare_enable(dev->mod_clk);
	if (ret) {
		dev_err(dev->dev, "Failed to enable MOD clock\n");
		goto err_ahb_clk;
	}

	ret = clk_prepare_enable(dev->ram_clk);
	if (ret) {
		dev_err(dev->dev, "Failed to enable RAM clock\n");
		goto err_mod_clk;
	}

	return 0;

err_mod_clk:
	clk_disable_unprepare(dev->mod_clk);
err_ahb_clk:
	clk_disable_unprepare(dev->ahb_clk);
err_rst:
	reset_control_assert(dev->rstc);

	return ret;
}

int sunxi_venc_hw_probe(struct sunxi_venc_dev *dev)
{
	struct resource *res;

	dev->ahb_clk = devm_clk_get(dev->dev, "ahb");
	if (IS_ERR(dev->ahb_clk))
		return dev_err_probe(dev->dev, PTR_ERR(dev->ahb_clk),
				     "Failed to get AHB clock\n");

	dev->mod_clk = devm_clk_get(dev->dev, "mod");
	if (IS_ERR(dev->mod_clk))
		return dev_err_probe(dev->dev, PTR_ERR(dev->mod_clk),
				     "Failed to get MOD clock\n");

	dev->ram_clk = devm_clk_get(dev->dev, "ram");
	if (IS_ERR(dev->ram_clk))
		return dev_err_probe(dev->dev, PTR_ERR(dev->ram_clk),
				     "Failed to get RAM clock\n");

	/*
	 * The VE reset line is held exclusively by the cedrus decoder driver,
	 * so it cannot be requested here (even a shared request is rejected
	 * against an exclusive holder). dev->rstc stays NULL —
	 * reset_control_assert()/deassert() are NULL-safe no-ops. M1 adds the
	 * CCU reset workaround (ioremap RST_BUS_VE directly) for the encode path.
	 */
	dev->rstc = NULL;

	/*
	 * The VE register block is shared with the cedrus decoder, which
	 * already owns the memory region. Map it without an exclusive
	 * request so both drivers can coexist.
	 */
	res = platform_get_resource(dev->pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -EINVAL;

	dev->base = devm_ioremap(dev->dev, res->start, resource_size(res));
	if (!dev->base) {
		dev_err(dev->dev, "Failed to map registers\n");
		return -ENOMEM;
	}

	/* Encoder IRQ is wired in M1; M0 runs without it. */
	dev->irq = platform_get_irq_optional(dev->pdev, 0);

	pm_runtime_enable(dev->dev);

	return 0;
}

void sunxi_venc_hw_remove(struct sunxi_venc_dev *dev)
{
	pm_runtime_disable(dev->dev);
	if (!pm_runtime_status_suspended(dev->dev))
		sunxi_venc_hw_suspend(dev->dev);
}

/* ---- M2M ---- */

/*
 * M2: encode one frame. Take one raw buffer off the OUTPUT queue and one
 * coded buffer off the CAPTURE queue, run the hardware encode, and report the
 * coded byte count. Frame type (IDR / P) follows the GOP pattern.
 */
static void sunxi_venc_device_run(void *priv)
{
	struct sunxi_venc_ctx *ctx = priv;
	struct vb2_v4l2_buffer *src, *dst;
	bool is_idr;
	int bytes;

	src = v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx);
	dst = v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx);

	if (!src || !dst) {
		if (src)
			v4l2_m2m_buf_done(src, VB2_BUF_STATE_ERROR);
		if (dst)
			v4l2_m2m_buf_done(dst, VB2_BUF_STATE_ERROR);
		v4l2_m2m_job_finish(ctx->dev->m2m_dev, ctx->fh.m2m_ctx);
		return;
	}

	v4l2_m2m_buf_copy_metadata(src, dst, false);

	is_idr = sunxi_venc_h264_frame_is_idr(ctx);
	/* Claim the shared VE engine for this encode (see cedrus_hw.c). */
	down(&sunxi_ve_sem);
	/*
	 * (M1) sunxi-venc no longer masks the cedrus VE IRQ around encode:
	 * the encode now WAITS on cedrus_irq via sunxi_venc_done completion
	 * (cedrus_irq dispatches encode-done back to us when cedrus_enc_inflight
	 * is set). sunxi_ve_irq_disable/enable helpers stay in cedrus_hw.c
	 * for any future diagnostic caller, but are intentionally not invoked
	 * on the encode hot path.
	 */
	bytes = sunxi_venc_h264_encode(ctx, &src->vb2_buf, &dst->vb2_buf);
	up(&sunxi_ve_sem);
	if (bytes < 0) {
		vb2_set_plane_payload(&dst->vb2_buf, 0, 0);
		v4l2_m2m_buf_done(src, VB2_BUF_STATE_ERROR);
		v4l2_m2m_buf_done(dst, VB2_BUF_STATE_ERROR);
	} else {
		vb2_set_plane_payload(&dst->vb2_buf, 0, bytes);
		dst->flags &= ~(V4L2_BUF_FLAG_KEYFRAME | V4L2_BUF_FLAG_PFRAME |
				V4L2_BUF_FLAG_BFRAME);
		dst->flags |= is_idr ? V4L2_BUF_FLAG_KEYFRAME
				     : V4L2_BUF_FLAG_PFRAME;
		v4l2_m2m_buf_done(src, VB2_BUF_STATE_DONE);
		v4l2_m2m_buf_done(dst, VB2_BUF_STATE_DONE);
		ctx->frame_num++;
	}

	v4l2_m2m_job_finish(ctx->dev->m2m_dev, ctx->fh.m2m_ctx);
}

static const struct v4l2_m2m_ops sunxi_venc_m2m_ops = {
	.device_run	= sunxi_venc_device_run,
};

/* ---- file ops ---- */

static int sunxi_venc_open(struct file *file)
{
	struct sunxi_venc_dev *dev = video_drvdata(file);
	struct sunxi_venc_ctx *ctx;
	int ret;

	if (mutex_lock_interruptible(&dev->dev_mutex))
		return -ERESTARTSYS;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx) {
		ret = -ENOMEM;
		goto err_unlock;
	}

	v4l2_fh_init(&ctx->fh, video_devdata(file));
	file->private_data = &ctx->fh;
	ctx->dev = dev;

	/* H264 encoder controls (bitrate, GOP, QP, profile/level, entropy). */
	ret = sunxi_venc_ctrls_setup(ctx);
	if (ret)
		goto err_free;

	ctx->fh.m2m_ctx = v4l2_m2m_ctx_init(dev->m2m_dev, ctx,
					    &sunxi_venc_queue_init);
	if (IS_ERR(ctx->fh.m2m_ctx)) {
		ret = PTR_ERR(ctx->fh.m2m_ctx);
		goto err_ctrls;
	}

	/* Default formats: 320x240 NV12 in, H264 out. */
	ctx->src_fmt.pixelformat = V4L2_PIX_FMT_NV12;
	ctx->src_fmt.width = 320;
	ctx->src_fmt.height = 240;
	sunxi_venc_prepare_format(&ctx->src_fmt);

	ctx->dst_fmt.pixelformat = V4L2_PIX_FMT_H264;
	ctx->dst_fmt.width = ctx->src_fmt.width;
	ctx->dst_fmt.height = ctx->src_fmt.height;
	sunxi_venc_prepare_format(&ctx->dst_fmt);

	v4l2_fh_add(&ctx->fh);

	mutex_unlock(&dev->dev_mutex);

	return 0;

err_ctrls:
	v4l2_ctrl_handler_free(&ctx->hdl);
err_free:
	kfree(ctx);
err_unlock:
	mutex_unlock(&dev->dev_mutex);

	return ret;
}

static int sunxi_venc_release(struct file *file)
{
	struct sunxi_venc_dev *dev = video_drvdata(file);
	struct sunxi_venc_ctx *ctx = container_of(file->private_data,
						  struct sunxi_venc_ctx, fh);

	mutex_lock(&dev->dev_mutex);

	v4l2_fh_del(&ctx->fh);
	v4l2_m2m_ctx_release(ctx->fh.m2m_ctx);
	v4l2_ctrl_handler_free(&ctx->hdl);
	v4l2_fh_exit(&ctx->fh);
	kfree(ctx);

	mutex_unlock(&dev->dev_mutex);

	return 0;
}

static const struct v4l2_file_operations sunxi_venc_fops = {
	.owner		= THIS_MODULE,
	.open		= sunxi_venc_open,
	.release	= sunxi_venc_release,
	.poll		= v4l2_m2m_fop_poll,
	.unlocked_ioctl	= video_ioctl2,
	.mmap		= v4l2_m2m_fop_mmap,
};

static const struct video_device sunxi_venc_video_device = {
	.name		= SUNXI_VENC_NAME,
	.vfl_dir	= VFL_DIR_M2M,
	.fops		= &sunxi_venc_fops,
	.ioctl_ops	= &sunxi_venc_ioctl_ops,
	.minor		= -1,
	.release	= video_device_release_empty,
	.device_caps	= V4L2_CAP_VIDEO_M2M | V4L2_CAP_STREAMING,
};

/* ---- platform driver ---- */

static int sunxi_venc_probe(struct platform_device *pdev)
{
	struct sunxi_venc_dev *dev;
	struct video_device *vfd;
	int ret;

	dev = devm_kzalloc(&pdev->dev, sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	platform_set_drvdata(pdev, dev);
	dev->vfd = sunxi_venc_video_device;
	dev->dev = &pdev->dev;
	dev->pdev = pdev;

	ret = sunxi_venc_hw_probe(dev);
	if (ret) {
		dev_err(&pdev->dev, "Failed to probe hardware\n");
		return ret;
	}

	mutex_init(&dev->dev_mutex);

	ret = v4l2_device_register(&pdev->dev, &dev->v4l2_dev);
	if (ret) {
		dev_err(&pdev->dev, "Failed to register V4L2 device\n");
		goto err_hw;
	}

	vfd = &dev->vfd;
	vfd->lock = &dev->dev_mutex;
	vfd->v4l2_dev = &dev->v4l2_dev;
	video_set_drvdata(vfd, dev);

	dev->m2m_dev = v4l2_m2m_init(&sunxi_venc_m2m_ops);
	if (IS_ERR(dev->m2m_dev)) {
		v4l2_err(&dev->v4l2_dev, "Failed to initialize V4L2 M2M device\n");
		ret = PTR_ERR(dev->m2m_dev);
		goto err_v4l2;
	}

	dev->mdev.dev = &pdev->dev;
	strscpy(dev->mdev.model, SUNXI_VENC_NAME, sizeof(dev->mdev.model));
	strscpy(dev->mdev.bus_info, "platform:" SUNXI_VENC_NAME,
		sizeof(dev->mdev.bus_info));
	media_device_init(&dev->mdev);
	dev->v4l2_dev.mdev = &dev->mdev;

	ret = video_register_device(vfd, VFL_TYPE_VIDEO, 0);
	if (ret) {
		v4l2_err(&dev->v4l2_dev, "Failed to register video device\n");
		goto err_m2m;
	}

	v4l2_info(&dev->v4l2_dev, "Device registered as /dev/video%d\n",
		  vfd->num);

	ret = v4l2_m2m_register_media_controller(dev->m2m_dev, vfd,
						 MEDIA_ENT_F_PROC_VIDEO_ENCODER);
	if (ret) {
		v4l2_err(&dev->v4l2_dev,
			 "Failed to initialize M2M media controller\n");
		goto err_video;
	}

	ret = media_device_register(&dev->mdev);
	if (ret) {
		v4l2_err(&dev->v4l2_dev, "Failed to register media device\n");
		goto err_m2m_mc;
	}

	return 0;

err_m2m_mc:
	v4l2_m2m_unregister_media_controller(dev->m2m_dev);
err_video:
	video_unregister_device(&dev->vfd);
err_m2m:
	v4l2_m2m_release(dev->m2m_dev);
err_v4l2:
	v4l2_device_unregister(&dev->v4l2_dev);
err_hw:
	sunxi_venc_hw_remove(dev);

	return ret;
}

static int sunxi_venc_remove(struct platform_device *pdev)
{
	struct sunxi_venc_dev *dev = platform_get_drvdata(pdev);

	if (media_devnode_is_registered(dev->mdev.devnode)) {
		media_device_unregister(&dev->mdev);
		v4l2_m2m_unregister_media_controller(dev->m2m_dev);
		media_device_cleanup(&dev->mdev);
	}

	v4l2_m2m_release(dev->m2m_dev);
	video_unregister_device(&dev->vfd);
	v4l2_device_unregister(&dev->v4l2_dev);
	sunxi_venc_hw_remove(dev);

	return 0;
}

static const struct of_device_id sunxi_venc_dt_match[] = {
	{ .compatible = "allwinner,sunxi-venc-h264" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, sunxi_venc_dt_match);

static const struct dev_pm_ops sunxi_venc_pm_ops = {
	SET_RUNTIME_PM_OPS(sunxi_venc_hw_suspend, sunxi_venc_hw_resume, NULL)
};

static struct platform_driver sunxi_venc_driver = {
	.probe	= sunxi_venc_probe,
	.remove	= sunxi_venc_remove,
	.driver	= {
		.name		= SUNXI_VENC_NAME,
		.of_match_table	= sunxi_venc_dt_match,
		.pm		= &sunxi_venc_pm_ops,
	},
};
module_platform_driver(sunxi_venc_driver);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Allwinner sunxi H264 hardware encoder driver");

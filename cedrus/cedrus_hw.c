// SPDX-License-Identifier: GPL-2.0
/*
 * Cedrus VPU driver
 *
 * Copyright (C) 2016 Florent Revest <florent.revest@free-electrons.com>
 * Copyright (C) 2018 Paul Kocialkowski <paul.kocialkowski@bootlin.com>
 * Copyright (C) 2018 Bootlin
 *
 * Based on the vim2m driver, that is:
 *
 * Copyright (c) 2009-2010 Samsung Electronics Co., Ltd.
 * Pawel Osciak, <pawel@osciak.com>
 * Marek Szyprowski, <m.szyprowski@samsung.com>
 */

#include <linux/platform_device.h>
#include <linux/of_reserved_mem.h>
#include <linux/of_device.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/clk.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/reset.h>
#include <linux/soc/sunxi/sunxi_sram.h>
#include <sunxi-iommu.h>

#include <media/videobuf2-core.h>
#include <media/v4l2-mem2mem.h>

#include "cedrus.h"
#include "cedrus_hw.h"
#include "cedrus_regs.h"

#include <linux/semaphore.h>

/*
 * The T527 VE is a single hardware engine shared by the cedrus decoder and
 * the sunxi-venc encoder. This counting semaphore serialises VE ownership:
 * a decode and an encode must never program the engine concurrently. It is
 * held by cedrus from device_run() until the completion IRQ, and by
 * sunxi-venc for the duration of one (synchronous, poll-mode) encode.
 */
DEFINE_SEMAPHORE(sunxi_ve_sem);
EXPORT_SYMBOL_GPL(sunxi_ve_sem);

/*
 * Tracks whether cedrus_device_run() has claimed sunxi_ve_sem and is still
 * waiting for the completion IRQ. Set to 1 right after down(), cleared by
 * cedrus_irq() before up(). Lets the stale-ctx path know whether it owes
 * an up() — calling up() unconditionally would corrupt the semaphore count
 * when cedrus_irq fires from a stray AVC interrupt with no decode pending.
 */
atomic_t cedrus_dec_inflight = ATOMIC_INIT(0);
EXPORT_SYMBOL_GPL(cedrus_dec_inflight);

/*
 * Encoder side of the shared VE IRQ line. sunxi-venc has no DT IRQ of its
 * own — the AVC engine asserts on the cedrus-owned IRQ 16. The encoder
 * pre-arms cedrus_enc_inflight before triggering, then waits on
 * sunxi_venc_done. cedrus_irq captures the AVC status word and signals
 * the completion so the encoder can read VLE_LENGTH after the wait.
 */
atomic_t cedrus_enc_inflight = ATOMIC_INIT(0);
EXPORT_SYMBOL_GPL(cedrus_enc_inflight);

DECLARE_COMPLETION(sunxi_venc_done);
EXPORT_SYMBOL_GPL(sunxi_venc_done);

u32 sunxi_venc_last_status;
EXPORT_SYMBOL_GPL(sunxi_venc_last_status);

/* Bound for cedrus decode-IRQ watchdog. 4K H264 should complete in ~25ms;
 * 250ms is well past the worst-case real frame and short enough that
 * userspace can recover before its own poll() timeout fires. */
#define CEDRUS_DEC_WATCHDOG_MS	250

/*
 * Recovery work scheduled by the decode watchdog timer. Runs in process
 * context (workqueue), where it is safe to call reset_control_assert/
 * deassert and the v4l2-m2m completion helpers.
 *
 * Why the deep reset matters: empirically a single mis-decoded frame
 * leaves the T527 VE engine internal state machine wedged — every
 * subsequent decode also fails to assert IRQ. Writing VE_MODE_DISABLED
 * alone is not enough. Asserting and deasserting the VE reset line
 * fully clears the wedge so the next frame can succeed.
 */
static void cedrus_dec_reset_work_fn(struct work_struct *w)
{
	struct cedrus_dev *dev = container_of(w, struct cedrus_dev,
					      dec_reset_work);
	struct cedrus_ctx *ctx;

	if (dev->rstc) {
		reset_control_assert(dev->rstc);
		udelay(10);
		reset_control_deassert(dev->rstc);
	} else {
		/* No reset control wired — at least drop the engine into the
		 * disabled mode so the next cedrus_engine_enable() has a clean
		 * slate to write. */
		cedrus_engine_disable(dev);
	}

	ctx = v4l2_m2m_get_curr_priv(dev->m2m_dev);
	if (ctx) {
		if (ctx->current_codec < CEDRUS_CODEC_LAST &&
		    dev->dec_ops[ctx->current_codec]) {
			dev->dec_ops[ctx->current_codec]->irq_disable(ctx);
			dev->dec_ops[ctx->current_codec]->irq_clear(ctx);
		}
		if (ctx->current_req) {
			v4l2_ctrl_request_complete(ctx->current_req, &ctx->hdl);
			ctx->current_req = NULL;
		}
		v4l2_m2m_buf_done_and_job_finish(dev->m2m_dev, ctx->fh.m2m_ctx,
						 VB2_BUF_STATE_ERROR);
	}

	if (atomic_xchg(&cedrus_dec_inflight, 0))
		up(&sunxi_ve_sem);
}

/*
 * Decode watchdog timer callback. Runs in soft-IRQ context when the
 * cedrus completion IRQ failed to fire within CEDRUS_DEC_WATCHDOG_MS
 * after cedrus_device_run() armed the timer. The recovery itself (HW
 * reset, buf_done, sem release) must run in sleepable context, so all
 * we do here is log and kick the reset workqueue.
 */
static void cedrus_dec_watchdog_fn(struct timer_list *t)
{
	struct cedrus_dev *dev = from_timer(dev, t, dec_watchdog);

	v4l2_err(&dev->v4l2_dev,
		 "decode watchdog: HW did not assert IRQ within %d ms — aborting + resetting VE\n",
		 CEDRUS_DEC_WATCHDOG_MS);

	schedule_work(&dev->dec_reset_work);
}

void cedrus_dec_watchdog_start(struct cedrus_dev *dev)
{
	mod_timer(&dev->dec_watchdog,
		  jiffies + msecs_to_jiffies(CEDRUS_DEC_WATCHDOG_MS));
}
EXPORT_SYMBOL_GPL(cedrus_dec_watchdog_start);

void cedrus_dec_watchdog_cancel(struct cedrus_dev *dev)
{
	del_timer(&dev->dec_watchdog);
}
EXPORT_SYMBOL_GPL(cedrus_dec_watchdog_cancel);

/*
 * The VE interrupt line belongs to cedrus. The poll-mode sunxi-venc encoder
 * still makes the AVC engine assert it on encode-done, and cedrus_irq cannot
 * clear an interrupt with no decode in flight — left unhandled it storms and
 * the kernel disables the line ("irq N: nobody cared"), killing decode. The
 * encoder masks the line for the duration of an encode via these helpers.
 */
static int cedrus_ve_irq = -1;

void sunxi_ve_irq_disable(void)
{
	if (cedrus_ve_irq > 0)
		disable_irq(cedrus_ve_irq);
}
EXPORT_SYMBOL_GPL(sunxi_ve_irq_disable);

void sunxi_ve_irq_enable(void)
{
	if (cedrus_ve_irq > 0)
		enable_irq(cedrus_ve_irq);
}
EXPORT_SYMBOL_GPL(sunxi_ve_irq_enable);

int cedrus_engine_enable(struct cedrus_ctx *ctx, enum cedrus_codec codec)
{
	u32 reg = 0;

	/*
	 * FIXME: This is only valid on 32-bits DDR's, we should test
	 * it on the A13/A33.
	 */
	reg |= VE_MODE_REC_WR_MODE_2MB;
	reg |= VE_MODE_DDR_MODE_BW_128;

	switch (codec) {
	case CEDRUS_CODEC_MPEG2:
		reg |= VE_MODE_DEC_MPEG;
		break;

	/* H.264 and VP8 both use the same decoding mode bit. */
	case CEDRUS_CODEC_H264:
	case CEDRUS_CODEC_VP8:
		reg |= VE_MODE_DEC_H264;
		break;

	case CEDRUS_CODEC_H265:
		reg |= VE_MODE_DEC_H265;
		break;

	default:
		return -EINVAL;
	}

	if (ctx->src_fmt.width == 4096)
		reg |= VE_MODE_PIC_WIDTH_IS_4096;
	if (ctx->src_fmt.width > 2048)
		reg |= VE_MODE_PIC_WIDTH_MORE_2048;

	cedrus_write(ctx->dev, VE_MODE, reg);

	return 0;
}

void cedrus_engine_disable(struct cedrus_dev *dev)
{
	cedrus_write(dev, VE_MODE, VE_MODE_DISABLED);
}

void cedrus_dst_format_set(struct cedrus_dev *dev,
			   struct v4l2_pix_format *fmt)
{
	unsigned int width = fmt->width;
	unsigned int height = fmt->height;
	u32 chroma_size;
	u32 reg;

	switch (fmt->pixelformat) {
	case V4L2_PIX_FMT_NV12:
		chroma_size = ALIGN(width, 16) * ALIGN(height, 16) / 2;

		/* Set both PRIMARY (output write) and SECONDARY_OUT_FMT_EXT
		 * (used for inter-frame ref read) to NV12 so the format the
		 * HW writes the reconstructed frame in matches what it later
		 * reads as a reference. Without this, secondary defaults to
		 * TILED_32 -> ref reads interpret NV12 storage as tiled =
		 * garbage motion comp on all P/B frames.
		 */
		reg = VE_PRIMARY_OUT_FMT_NV12 | VE_SECONDARY_OUT_FMT_EXT_NV12;
		cedrus_write(dev, VE_PRIMARY_OUT_FMT, reg);

		/* CHROMA_BUF_LEN: bits 31..30 select SECONDARY format.
		 * Set to EXT to point at the EXT bits we set above, OR
		 * the chroma buf length in bits 27..0.
		 */
		reg = (chroma_size / 2) | VE_SECONDARY_OUT_FMT_EXT;
		cedrus_write(dev, VE_CHROMA_BUF_LEN, reg);

		reg = chroma_size / 2;
		cedrus_write(dev, VE_PRIMARY_CHROMA_BUF_LEN, reg);

		reg = VE_PRIMARY_FB_LINE_STRIDE_LUMA(ALIGN(width, 16)) |
		      VE_PRIMARY_FB_LINE_STRIDE_CHROMA(ALIGN(width, 16) / 2);
		cedrus_write(dev, VE_PRIMARY_FB_LINE_STRIDE, reg);

		break;
	case V4L2_PIX_FMT_SUNXI_TILED_NV12:
	default:
		chroma_size = ALIGN(width, 16) * ALIGN(height, 16) / 2;

		reg = VE_PRIMARY_OUT_FMT_TILED_32_NV12;
		cedrus_write(dev, VE_PRIMARY_OUT_FMT, reg);

		reg = VE_SECONDARY_OUT_FMT_TILED_32_NV12 |
		      VE_CHROMA_BUF_LEN_SDRT(chroma_size / 2);
		cedrus_write(dev, VE_CHROMA_BUF_LEN, reg);

		cedrus_write(dev, VE_PRIMARY_CHROMA_BUF_LEN, chroma_size / 2);

		reg = VE_PRIMARY_FB_LINE_STRIDE_LUMA(ALIGN(width, 16)) |
		      VE_PRIMARY_FB_LINE_STRIDE_CHROMA(ALIGN(width, 16));
		cedrus_write(dev, VE_PRIMARY_FB_LINE_STRIDE, reg);

		break;
	}
}

static irqreturn_t cedrus_irq(int irq, void *data)
{
	struct cedrus_dev *dev = data;
	struct cedrus_ctx *ctx;
	enum vb2_buffer_state state;
	enum cedrus_irq_status status;

	/*
	 * Read + w1c-ack any latched AVC_STATUS bits up front. The AVC engine
	 * asserts the shared VE IRQ line whenever it finishes an encode; cedrus
	 * does not otherwise care about those bits. Capturing the value lets
	 * the (M1) encoder-completion dispatch below report SUCCESS / DONE bits
	 * back to sunxi-venc before the kernel clears them.
	 */
	{
		u32 avc_status = cedrus_read(dev, VE_AVC_STATUS);

		cedrus_write(dev, VE_AVC_STATUS, avc_status);

		/*
		 * If sunxi-venc pre-armed cedrus_enc_inflight before triggering
		 * its encode, this IRQ is the encode-done signal it is waiting
		 * for. Hand off the status word and complete; the encoder will
		 * read VLE_LENGTH and validate after wait_for_completion returns.
		 */
		if (atomic_xchg(&cedrus_enc_inflight, 0)) {
			sunxi_venc_last_status = avc_status;
			complete(&sunxi_venc_done);
			return IRQ_HANDLED;
		}
	}

	ctx = v4l2_m2m_get_curr_priv(dev->m2m_dev);
	if (!ctx) {
		/*
		 * Two scenarios reach here:
		 *  (a) ctx released during in-flight decode (cleanup race) —
		 *      cedrus_dec_inflight == 1, so we owe an up().
		 *  (b) Stray IRQ from a finished encode (encoder failed to
		 *      clear AVC_STATUS). cedrus_dec_inflight == 0; do NOT
		 *      up() — sem isn't held by us. AVC_STATUS already acked
		 *      above so the shared IRQ line is low.
		 */
		if (atomic_xchg(&cedrus_dec_inflight, 0))
			up(&sunxi_ve_sem);
		v4l2_err(&dev->v4l2_dev,
			 "Instance released before the end of transaction\n");
		return IRQ_NONE;
	}

	status = dev->dec_ops[ctx->current_codec]->irq_status(ctx);
	if (status == CEDRUS_IRQ_NONE) {
		/*
		 * Stray AVC IRQ while a decode is pending: AVC_STATUS was
		 * already acked above. Decode is still in flight — do NOT
		 * touch inflight or sem. Return IRQ_HANDLED so genirq does
		 * not count this as spurious (which would eventually mask
		 * the line via "nobody cared").
		 */
		return IRQ_HANDLED;
	}

	dev->dec_ops[ctx->current_codec]->irq_disable(ctx);
	dev->dec_ops[ctx->current_codec]->irq_clear(ctx);

	if (status == CEDRUS_IRQ_ERROR)
		state = VB2_BUF_STATE_ERROR;
	else
		state = VB2_BUF_STATE_DONE;

	/*
	 * Complete the request AFTER HW finishes, so userspace poll
	 * returns POLLPRI only once the frame is actually decoded.
	 * This prevents the next frame's device_run from overwriting
	 * VE registers while the current frame is still in progress.
	 */
	if (ctx->current_req)
		v4l2_ctrl_request_complete(ctx->current_req, &ctx->hdl);
	ctx->current_req = NULL;

	v4l2_m2m_buf_done_and_job_finish(ctx->dev->m2m_dev, ctx->fh.m2m_ctx,
					 state);

	/*
	 * HW completion landed — cancel the watchdog before releasing the
	 * engine so a delayed-fire of the timer can't run after up().
	 */
	del_timer(&dev->dec_watchdog);

	/*
	 * HW transaction done — clear the in-flight flag *before* up() so a
	 * subsequent stray IRQ doesn't observe (inflight==1, sem==released).
	 */
	atomic_set(&cedrus_dec_inflight, 0);
	up(&sunxi_ve_sem);

	return IRQ_HANDLED;
}

int cedrus_hw_suspend(struct device *device)
{
	struct cedrus_dev *dev = dev_get_drvdata(device);

	clk_disable_unprepare(dev->ram_clk);
	clk_disable_unprepare(dev->mod_clk);
	clk_disable_unprepare(dev->ahb_clk);

	reset_control_assert(dev->rstc);

	return 0;
}

int cedrus_hw_resume(struct device *device)
{
	struct cedrus_dev *dev = dev_get_drvdata(device);
	int ret;

	ret = reset_control_reset(dev->rstc);
	if (ret) {
		dev_err(dev->dev, "Failed to apply reset\n");

		return ret;
	}

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

int cedrus_hw_probe(struct cedrus_dev *dev)
{
	const struct cedrus_variant *variant;
	int irq_dec;
	int ret;

	variant = of_device_get_match_data(dev->dev);
	if (!variant)
		return -EINVAL;

	dev->capabilities = variant->capabilities;

	irq_dec = platform_get_irq(dev->pdev, 0);
	if (irq_dec <= 0)
		return irq_dec;
	ret = devm_request_irq(dev->dev, irq_dec, cedrus_irq,
			       0, dev_name(dev->dev), dev);
	if (ret) {
		dev_err(dev->dev, "Failed to request IRQ\n");

		return ret;
	}

	cedrus_ve_irq = irq_dec;

	timer_setup(&dev->dec_watchdog, cedrus_dec_watchdog_fn, 0);
	INIT_WORK(&dev->dec_reset_work, cedrus_dec_reset_work_fn);

	ret = of_reserved_mem_device_init(dev->dev);
	if (ret && ret != -ENODEV) {
		dev_err(dev->dev, "Failed to reserve memory\n");

		return ret;
	}

	if (!(dev->capabilities & CEDRUS_CAPABILITY_NO_SRAM)) {
		ret = sunxi_sram_claim(dev->dev);
		if (ret) {
			dev_err(dev->dev, "Failed to claim SRAM\n");

			goto err_mem;
		}
	}

	dev->ahb_clk = devm_clk_get(dev->dev, "ahb");
	if (IS_ERR(dev->ahb_clk)) {
		dev_err(dev->dev, "Failed to get AHB clock\n");

		ret = PTR_ERR(dev->ahb_clk);
		goto err_sram;
	}

	dev->mod_clk = devm_clk_get(dev->dev, "mod");
	if (IS_ERR(dev->mod_clk)) {
		dev_err(dev->dev, "Failed to get MOD clock\n");

		ret = PTR_ERR(dev->mod_clk);
		goto err_sram;
	}

	dev->ram_clk = devm_clk_get(dev->dev, "ram");
	if (IS_ERR(dev->ram_clk)) {
		dev_err(dev->dev, "Failed to get RAM clock\n");

		ret = PTR_ERR(dev->ram_clk);
		goto err_sram;
	}

	dev->rstc = devm_reset_control_get(dev->dev, NULL);
	if (IS_ERR(dev->rstc)) {
		dev_err(dev->dev, "Failed to get reset control\n");

		ret = PTR_ERR(dev->rstc);
		goto err_sram;
	}

	dev->base = devm_platform_ioremap_resource(dev->pdev, 0);
	if (IS_ERR(dev->base)) {
		dev_err(dev->dev, "Failed to map registers\n");

		ret = PTR_ERR(dev->base);
		goto err_sram;
	}

	{
		/* (M14) Optional VE mod clock override via cedrus.clk_hz module
		 * param — exported from cedrus.c. 0 = use variant default
		 * (600 MHz on T527). Lets us validate the silicon ceiling
		 * without recompiling. */
		extern unsigned long cedrus_clk_hz_override;
		unsigned long rate = cedrus_clk_hz_override ?
				     cedrus_clk_hz_override :
				     variant->mod_rate;

		ret = clk_set_rate(dev->mod_clk, rate);
		if (ret) {
			dev_err(dev->dev,
				"Failed to set VE clock rate %lu Hz: %d\n",
				rate, ret);
			goto err_sram;
		}
		dev_info(dev->dev,
			 "VE mod clock requested %lu Hz, got %lu Hz%s\n",
			 rate, clk_get_rate(dev->mod_clk),
			 cedrus_clk_hz_override ? " (clk_hz override)" : "");
	}

	pm_runtime_enable(dev->dev);
	if (!pm_runtime_enabled(dev->dev)) {
		ret = cedrus_hw_resume(dev->dev);
		if (ret)
			goto err_pm;
	}

	/* T527 VE has two MBUS masters: VE_MBUS0 (master 2) for output DMA
	 * writes, VE_MBUS1 (master 3) for MC reference reads. DTS iommus
	 * property binds only master 2 (sunxi_iommu_of_xlate stores one master
	 * per device). Explicitly enable master 3 so MC reads are translated. */
	sunxi_enable_device_iommu(3, true);

	return 0;

err_pm:
	pm_runtime_disable(dev->dev);
err_sram:
	sunxi_sram_release(dev->dev);
err_mem:
	of_reserved_mem_device_release(dev->dev);

	return ret;
}

void cedrus_hw_remove(struct cedrus_dev *dev)
{
	del_timer_sync(&dev->dec_watchdog);
	cancel_work_sync(&dev->dec_reset_work);

	sunxi_enable_device_iommu(3, false);
	pm_runtime_disable(dev->dev);
	if (!pm_runtime_status_suspended(dev->dev))
		cedrus_hw_suspend(dev->dev);

	sunxi_sram_release(dev->dev);

	of_reserved_mem_device_release(dev->dev);
}

/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Allwinner sunxi H264 hardware encoder driver — VE register definitions.
 *
 * Register values verified against the vendor libvenc_h264.so on the
 * Allwinner T527 (see path-a-cedar-bsp-enc/VE_REG_TRACE.md).
 */

#ifndef _SUNXI_VENC_REGS_H_
#define _SUNXI_VENC_REGS_H_

/* ---- Top-level VE registers ---- */
#define VE_CTRL				0x000
#define VE_MODE				0x004	/* vendor trace: 0x100 during AVC encode */
#define VE_VERSION			0x0f0

/*
 * VE_CTRL value the vendor encoder uses during AVC encode on the T527:
 * engine field 0x7, +0xc0 new-mode enable, +0xc0000000 HW top bits.
 * Without the 0xc0 bits, the AVC trigger (0x8) is silently ignored.
 */
#define VE_CTRL_AVC_ENCODE		0xc01300c7
#define VE_MODE_AVC_ENCODE		0x00000100

/*
 * Global setup registers the vendor encoder programs before an AVC encode
 * (VE_REG_TRACE.md snap 3). Purpose undocumented; replicated verbatim.
 */
#define VE_ENC_INIT_01C			0x01c
#define VE_ENC_INIT_030			0x030
#define VE_ENC_INIT_040			0x040
#define VE_ENC_INIT_080			0x080
#define VE_ENC_INIT_084			0x084
#define VE_ENC_INIT_088			0x088

/* ---- ISP (input pre-processor) block — address regs take the FULL addr ---- */
#define VE_ISP_INPUT_SIZE		0xa00	/* (mb_w*2)<<16 | (mb_h*2) */
#define VE_ISP_INPUT_STRIDE		0xa04
#define VE_ISP_CTRL			0xa08
#define VE_ISP_MB_WIDTH			0xa14
#define VE_ISP_INPUT_SIZE2		0xa2c	/* (mb_h*2)<<16 | (mb_w*2) */
#define VE_ISP_INPUT_LUMA		0xa78
#define VE_ISP_INPUT_CHROMA		0xa7c
#define VE_ISP_INPUT_CHROMA2		0xa80

/* ---- AVC encoder block — address regs take (addr >> 8) ---- */
#define VE_AVC_PIC_SIZE			0xb00	/* (mb_w*2)<<16 | (mb_h*2) */
#define VE_AVC_PARAM			0xb04	/* 0 = I-frame, 0x410 = P-frame */
#define VE_AVC_QP			0xb08
#define VE_AVC_MOTION_EST		0xb10
#define VE_AVC_CTRL			0xb14
#define VE_AVC_TRIGGER			0xb18	/* write 0x8 = encode frame */
#define VE_AVC_STATUS			0xb1c
#define VE_AVC_BASIC_BITS		0xb20	/* HW bit-writer input */
#define VE_AVC_RC_SETUP			0xb2c	/* rate-control setup (IDR: 0x80001900) */
#define VE_AVC_QP_TBL0			0xb30
#define VE_AVC_QP_TBL1			0xb34
#define VE_AVC_QP_TBL2			0xb38
#define VE_AVC_QP_TBL3			0xb3c
#define VE_AVC_RC_TBL0			0xb48
#define VE_AVC_RC_TBL1			0xb4c
#define VE_AVC_UNK_BUF			0xb60	/* >>8 */
#define VE_AVC_VLE_ADDR			0xb80	/* >>8 */
#define VE_AVC_VLE_END			0xb84	/* >>8 */
#define VE_AVC_VLE_OFFSET		0xb88
#define VE_AVC_VLE_MAX			0xb8c	/* bits */
#define VE_AVC_VLE_LENGTH		0xb90	/* bits written (read-only) */
#define VE_AVC_SCRATCH_BUF		0xb9c	/* >>8 */
#define VE_AVC_REF_LUMA			0xba0	/* >>8 */
#define VE_AVC_REF_CHROMA		0xba4	/* >>8 */
#define VE_AVC_REC_LUMA			0xbb0	/* >>8 */
#define VE_AVC_REC_CHROMA		0xbb4	/* >>8 */
#define VE_AVC_REF_SLUMA		0xbb8	/* >>8 */
#define VE_AVC_REC_SLUMA		0xbbc	/* >>8 */
#define VE_AVC_MB_INFO			0xbc0	/* >>8 */
#define VE_AVC_LINE_BUF			0xbc4	/* >>8 — wide-frame line buffer (width >2048) */
#define VE_AVC_UNK_BE4			0xbe4

/* VE_AVC_PARAM bits */
#define VE_AVC_PARAM_NO_EP		(0x1u << 31)	/* disable emulation prevention */
#define VE_AVC_PARAM_CABAC		0x100
/* VE_AVC_PARAM frame-type image (VE_REG_TRACE.md: I snap 3, P snap 6). */
#define VE_AVC_PARAM_ENCODE_I		0x000
#define VE_AVC_PARAM_ENCODE_P		0x410

/* VE_AVC_CTRL */
#define VE_AVC_CTRL_BITWRITER_EN	0xf		/* enable HW bit-writer */
#define VE_AVC_CTRL_ENCODE_IDR		0x13050007	/* AVC_CTRL just before IDR trigger */
#define VE_AVC_CTRL_ENCODE_P		0x12040007	/* AVC_CTRL just before P trigger */

/* VE_AVC_RC_SETUP (0xb2c) frame-type image. */
#define VE_AVC_RC_SETUP_I		0x80001900
#define VE_AVC_RC_SETUP_P		0xc000c852

/* AVC_TRIGGER commands */
#define VE_AVC_TRIGGER_ENCODE		0x8
#define VE_AVC_TRIGGER_BITS(n)		(0x1 | (((n) & 0x1f) << 8))

/* AVC_STATUS: encode-done indication (engine 0x7 path), w1c */
#define VE_AVC_STATUS_DONE		0x3
#define VE_AVC_STATUS_SUCCESS		0x1

#endif /* _SUNXI_VENC_REGS_H_ */

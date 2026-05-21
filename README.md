# sunxi-venc-t527

Full H264 transcode stack for the Allwinner T527 SoC (OrangePi 4A): open-source V4L2 M2M encoder driver (`sunxi-venc`) plus the cedrus stateless decoder with all changes required for concurrent decode+encode on the shared VE hardware.

Target: **OrangePi 4A** (Allwinner T527, BSP kernel 5.15-sun55iw3).

## Repository layout

```
cedrus/          — cedrus stateless decoder (all T527 patches including VE-sharing)
sunxi-venc/      — new M2M H264 encoder driver
```

## sunxi-venc encoder

New V4L2 M2M stateful H264 encoder driver binding `/dev/video2`. Drives the Allwinner VE AVC engine.

| File | Purpose |
|---|---|
| `sunxi_venc.c` | M2M device, VE semaphore sharing, IRQ mask, IRQ-based completion wait |
| `sunxi_venc.h` | Context, params, buffer field definitions |
| `sunxi_venc_regs.h` | VE AVC register definitions |
| `sunxi_venc_h264.c` | H264 encode: SPS/PPS headers, slice encode, rate control, PIC_SIZE, QP reg, `chroma_qp_index_offset=0`, 4K line buffer, AVC w1c, IRQ-based completion |
| `sunxi_venc_video.c` | V4L2 ioctls, `S_SELECTION` crop, `ENUM_FRAMESIZES` |
| `sunxi_venc_ctrls.c` | V4L2 control handler |

Key details:
- Encode resolution cap: **3840×2160 (4K)** — allocates a line buffer (8 bytes/luma-column) at `VE_AVC_LINE_BUF` (reg 0xbc4, `phys>>8`) for width > 2048; ≤2048 uses internal SRAM
- IRQ-based completion: replaces 300 ms `udelay` poll; encode done signal delivered through the cedrus-owned VE IRQ via `cedrus_enc_inflight` + `sunxi_venc_done` completion
- Shares the VE engine with cedrus via `sunxi_ve_sem` semaphore

### Encode performance (OrangePi 4A, 792 MHz VE, raw NV12 from tmpfs)

| Resolution | fps |
|---|---|
| 720p | ~183 fps |
| 1080p | ~81 fps |
| 4K | ~27 fps |

## cedrus changes for VE sharing

The `cedrus/` directory contains the decoder with two layers of changes:
1. **T527 decode patches** (same as [cedrus-t527](https://github.com/matheusants/cedrus-t527))
2. **VE-sharing patches** required by sunxi-venc

VE-sharing additions:

| Change | Purpose |
|---|---|
| `sunxi_ve_sem` semaphore | Serialise decode and encode — single half-duplex VE engine |
| `cedrus_dec_inflight` atomic | Distinguish in-flight decode from stray AVC IRQs during encode |
| Stray AVC ack at IRQ top | Prevent "nobody cared" genirq mask (would deadlock sem forever) |
| `cedrus_release` sem drain | UAF race fix: drain sem before `m2m_ctx_release` |
| 250ms watchdog + workqueue VE reset | Recover from VE state machine wedge across encode↔decode mode transitions; only `reset_control_assert/deassert` clears it |
| Encode completion dispatch | `cedrus_irq` captures `VE_AVC_STATUS` and signals `sunxi_venc_done` when `cedrus_enc_inflight` is set |

## Transcode performance (VAAPI zero-copy, 792 MHz VE)

| Scenario | fps | realtime |
|---|---|---|
| 4K H264 → 4K H264 | 26 | 1.08× |
| 4K H264 → 720p H264 | 37 | 1.54× |
| 1440×1080 HEVC → 1440×1080 H264 | 110 | 4.58× |
| 2× 4K → 720p (concurrent) | 20 + 20 | 0.83× each |

Practical Jellyfin policy: **1× realtime 4K transcode max**, or **2× ≤720p concurrent near-realtime**.

## Related repos

- [libva-v4l2-request](https://github.com/matheusants/libva-v4l2-request) — VAAPI driver (userspace)
- [cedrus-t527](https://github.com/matheusants/cedrus-t527) — cedrus decoder only (without VE-sharing patches)

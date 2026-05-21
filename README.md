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

## Kernel modules

### Required modules

| Module | File | Provides | Load order |
|---|---|---|---|
| `sunxi-cedrus` | `sunxi-cedrus.ko` | `/dev/video1`, `/dev/media1` (cedrus decoder) | 1st |
| `sunxi-venc` | `sunxi-venc.ko` | `/dev/video2` (H264 encoder) | 2nd (depends on cedrus) |

`sunxi-venc` declares a hard dependency on `sunxi-cedrus`, so `modprobe sunxi-venc` loads both. Loading cedrus first is still recommended for clarity.

Auto-loaded dependencies:

```
v4l2-mem2mem  videobuf2-v4l2  videobuf2-common
videobuf2-dma-contig  videobuf2-dma-sg
```

### Load manually

```sh
sudo modprobe sunxi-cedrus
sudo modprobe sunxi-venc
# Verify
ls /dev/video1 /dev/video2 /dev/media1
dmesg | grep -E "cedrus|sunxi-venc" | tail -10
```

### Load automatically at boot

```sh
echo -e "sunxi-cedrus\nsunxi-venc" | sudo tee -a /etc/modules-load.d/modules.conf
```

> **Note:** `sunxi-venc` must come after `sunxi-cedrus` in the file — modules-load.d applies entries in order.

### Optional: override VE clock at load time

```sh
sudo modprobe sunxi-cedrus clk_hz=720000000   # 720 MHz (conservative)
# Default is 792 MHz — validated ceiling for clean encode output
```

## Installation with orangepi-build

### Step 1 — Apply the DTS patches (required)

Two patches must be applied to the OrangePi 4A device tree before building the kernel.
Using [orangepi-build](https://github.com/orangepi-xunlong/orangepi-build):

```sh
# Copy both DTS patches into orangepi-build userpatches
cp dts-patches/0007-t527-dts.patch \
   dts-patches/0020-t527-venc-dts.patch \
   ~/orangepi-build/userpatches/kernel/sun55iw3-current/
```

| Patch | What it adds |
|---|---|
| `0007-t527-dts.patch` | cedrus node: register range, clock names, IOMMU master 2 binding, disables ve1 |
| `0020-t527-venc-dts.patch` | `video-encoder@1c0e000` node for sunxi-venc |

### Step 2 — Copy the kernel patches

This repo includes all 24 ready-to-apply patches in `kernel-patches/`. Copy them all into orangepi-build:

```sh
cp kernel-patches/*.patch \
   ~/orangepi-build/userpatches/kernel/sun55iw3-current/
```

orangepi-build applies patches alphabetically from `userpatches/kernel/sun55iw3-current/` before each build.

**Group A — cedrus decode (0001-0009):** T527 variant, 792 MHz clock, NV12/TILED formats, IOMMU master binding, vb2_dma_sg mmap, cedrus DT node.

**Group B — sunxi-venc + cedrus VE-sharing (0010-0024):** new encoder driver files, venc DT node, cedrus coordination patches (semaphore, watchdog, IRQ dispatch, UAF fix).

> The DTS patches (`0007`, `0020`) are duplicated in `dts-patches/` for convenience.

### Step 3 — Enable the sunxi-venc Kconfig option

Add to your kernel config (e.g. `~/orangepi-build/external/config/kernel/linux-5.15-sun55iw3-current.config`):

```
CONFIG_VIDEO_SUNXI_VENC=m
```

### Step 4 — Rebuild and install the kernel

```sh
cd ~/orangepi-build
sudo ./build.sh BOARD=orangepi4a BRANCH=current BUILD_OPT=kernel KERNEL_CONFIGURE=no
sudo dpkg -i output/debs/linux-image-*.deb output/debs/linux-dtb-*.deb
sudo reboot
```

After reboot, verify both devices appear:

```sh
ls /dev/video*   # should include /dev/video1 (cedrus) and /dev/video2 (sunxi-venc)
```

### Step 5 — Build and install the VAAPI driver

```sh
git clone https://github.com/matheusants/libva-v4l2-request
cd libva-v4l2-request
meson setup build && ninja -C build && sudo ninja -C build install
```

### Step 6 — Test

```sh
# Decode test
LIBVA_DRIVER_NAME=v4l2_request LIBVA_DRIVERS_PATH=/usr/lib/aarch64-linux-gnu/dri \
  ffmpeg -hwaccel vaapi -hwaccel_device /dev/dri/renderD128 \
  -i input.mp4 -vframes 30 -f null -

# 4K → 720p transcode
LIBVA_DRIVER_NAME=v4l2_request LIBVA_DRIVERS_PATH=/usr/lib/aarch64-linux-gnu/dri \
  ffmpeg -hwaccel vaapi -hwaccel_device /dev/dri/renderD128 \
  -hwaccel_output_format vaapi -i input_4k.mp4 \
  -vf scale_vaapi=w=1280:h=720 -c:v h264_vaapi -b:v 6M output_720p.mp4
```

## Related repos

- [libva-v4l2-request](https://github.com/matheusants/libva-v4l2-request) — VAAPI driver (userspace)
- [cedrus-t527](https://github.com/matheusants/cedrus-t527) — cedrus decoder only (without VE-sharing patches)

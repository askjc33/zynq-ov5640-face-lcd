# StudyBoard Linux staged capture/display VDMA driver

This teaching driver now exposes the same PL capture path in two deliberately
separate ways:

1. the original sysfs bring-up path, which owns three fixed coherent buffers
   and can feed the LCD through VDMA1; and
2. a standard V4L2 capture node (`/dev/videoX`), which uses videobuf2 MMAP
   buffers and VDMA0 to return completed frames to userspace.

Only one path may own VDMA0 at a time. Opening `/dev/videoX` while the sysfs
capture/display path is active returns `EBUSY`; starting the sysfs path while
the V4L2 node is open does the same. This explicit ownership rule prevents two
APIs from programming the same DMA channel concurrently.

For 640x480 packed 24-bit pixels:

```text
stride       = 640 * 3       = 1920 bytes
frame_bytes  = 1920 * 480    = 921600 bytes
total_bytes  = 921600 * 3    = 2764800 bytes
```

No fixed physical DDR address is used.  The Xilinx DMAEngine driver remains
the sole owner of the VDMA registers.

The module exposes these attributes under its platform device:

- `geometry`: dimensions and allocation sizes;
- `running`: read/write control; write `1` to start after OV5640 setup and
  write `0` to stop both display and capture;
- `display_running`: read/write control for VDMA1 MM2S; capture must already
  be running before this can be set to `1`;
- `callback_count`: completed setup-descriptor count (normally reaches 4:
  three parked address-programming frames plus one circular-mode frame;
  continuous operation is proved by the VDMA IRQ counter, not by expecting
  this value to grow forever);
- `last_frame`: last callback context index;
- `display_callback_count` and `display_last_frame`: the corresponding VDMA1
  setup observability values;
- `dma_buffers`: DMA addresses allocated by the kernel;
- `sample_digests`: sampled FNV-1a activity digest for each frame.
- `snapshot_frame`: when both channels are stopped, write `0`, `1` or `2` to
  copy that DMA frame into an independent, CPU-only frozen snapshot;
- `snapshot_raw`: read-only binary file containing the frozen snapshot.  Its
  size is exactly `frame_bytes` (921600 bytes for 640x480 BGR24).

The PL signal is logically `TDATA[23:16]=R`, `[15:8]=G`, `[7:0]=B`.  Zynq is
little-endian, so the CPU sees each pixel in increasing DDR byte-address order
as `B,G,R`. Three visually complete captures made on 2026-09-21 confirmed
that the userspace contract is therefore **BGR24**, even though the RTL signal
is conventionally described as RGB888. The V4L2 node therefore advertises
`V4L2_PIX_FMT_BGR24` (`BGR3`).

`sample_digests` is deliberately only an activity indicator.  The VDMA can be
writing a frame while sysfs samples it, so this is not a frame-integrity API.
`snapshot_raw` is the next staged userspace interface: it never exposes the
live coherent DMA allocation.  Instead the user stops both channels, selects
one of the three frame stores, and the driver copies that store into separate
CPU memory.  This proves Linux can export all pixel bytes before a V4L2/MMAP
queue is introduced.

Stopping VDMA makes the selected memory stable, but `terminate_all()` can halt
S2MM part-way through its current frame.  The driver does not yet know the
live hardware frame-store index, so one of the three stores can be partial at
the instant capture stops.  Export all three during validation; normally the
other two contain completed older frames. The V4L2 path below adds completed
frame ownership: VDMA writes only a buffer that userspace has queued, and
`DQBUF` returns it only after the DMA completion callback.

In continuous capture and legacy circular display, `frm_cnt_en` remains zero. AXI VDMA uses that bit for finite
frame-count mode; zero is the endless-transfer setting.  The separate
interrupt coalescing threshold is one, so the active S2MM and MM2S IRQs should
still advance once per completed frame.

## Userspace annotated-frame LCD interface

The additional `lcd_frame` (binary, write-only), `lcd_commit` (write-only),
and `lcd_status` (read-only) attributes accept processed BGR24 frames while
`/dev/videoX` is open. They are a board-specific display API, not a standard
V4L2 output node or Linux framebuffer API.

Write exactly `frame_bytes` sequential bytes starting at offset zero, then
write `1` to `lcd_commit` from the same process. Chunked writes are supported;
partial frames and mismatched offsets cannot be committed. The writing
process holds a referenced PID as session owner; another process gets
`EBUSY`. Writing `0` to `lcd_commit` releases that session. Closing the unique
V4L2 file also stops/releases it, including on process exit. As an explicit
administrator recovery action, `display_running=0` stops either display mode.
Do not issue legacy start/stop commands while the inference app is running.

This mode uses the original coherent allocation for LCD front/back storage,
never the V4L2 vb2 buffers. Legacy capture cannot start while V4L2 is open. An
offset-zero write selects the non-displayed staging buffer and abandons any
previous partial frame. Commit is synchronous and must run in the inference/
output thread, not the camera capture loop.

Board testing proved that finite one-frame MM2S completion was not sufficient
to produce a visible LCD update. The corrected path uses `frm_cnt_en=0`,
programs every hardware frame store with one stable front-buffer address, and
keeps VDMA1 running continuously. Sysfs writes assemble a complete frame in a
back buffer; commit copies it to the front without terminating or resetting
MM2S. This module-only fallback can expose one short transition while memcpy
overlaps a read; a strict tear-free park-frame handoff requires an additional
frame-boundary switching API in the lower Xilinx VDMA driver.

`lcd_status` reports `mode=continuous-copy`, a logical active session
(`running`), successful published frame count, staging byte count, front/back
indices and owner PID. `display_running` reads 1 for
either display mode, but writes 1 still select the old legacy display mode.
Neither status is proof that the LCD physically displayed a correct image;
colors, line alignment, repeated publication and restart need board testing.

The design follows [AXI VDMA PG020's MM2S frame-count semantics](https://docs.amd.com/r/en-US/pg020_axi_vdma/MM2S_VDMACR-MM2S-VDMA-Control-Register-Offset-00h)
and the project's [linux-xlnx 2020.2 direct-register driver](https://github.com/Xilinx/linux-xlnx/blob/xlnx_rebase_v5.4_2020.2/drivers/dma/xilinx/xilinx_dma.c).

The 2020.2 Xilinx register-direct VDMA driver programs one frame-store start
address for each descriptor it starts.  Startup therefore parks and completes
one descriptor for frame0, frame1 and frame2 in sequence.  Only after all
three hardware address registers are valid does a fourth descriptor switch
the channel to circular mode.  This prevents VDMA from rotating into an
uninitialized address zero.

Linux 5.4 defines `dma_interleaved_template::sgl` as a flexible array.  The
driver therefore allocates `sizeof(template) + sizeof(sgl[0])` bytes before
filling its single VDMA data chunk.  Do not replace this with a plain stack
instance: writing `sgl[0]` would overwrite the kernel stack canary.

The device-tree modalias can make `udev` load this module automatically even
without `KERNEL_MODULE_AUTOLOAD`.  Probe therefore only reserves the DMA
channel and allocates buffers; it deliberately does not start VDMA.

Expected manual order on the board:

```sh
ov5640-init
modprobe studyboard-frame-capture 2>/dev/null || true
echo 1 > /sys/bus/platform/devices/studyboard-frame-capture/running
echo 1 > /sys/bus/platform/devices/studyboard-frame-capture/display_running
```

Stop display only, leaving capture active:

```sh
echo 0 > /sys/bus/platform/devices/studyboard-frame-capture/display_running
```

Stop both paths without unloading the driver:

```sh
echo 0 > /sys/bus/platform/devices/studyboard-frame-capture/running
```

Freeze and export all three stopped BGR24 frame stores:

```sh
P=/sys/bus/platform/devices/studyboard-frame-capture
echo 0 > "$P/running"

for N in 0 1 2; do
    echo "$N" > "$P/snapshot_frame"
    cat "$P/snapshot_raw" > "/tmp/frame${N}.raw"
done

wc -c /tmp/frame0.raw /tmp/frame1.raw /tmp/frame2.raw
sha256sum /tmp/frame0.raw /tmp/frame1.raw /tmp/frame2.raw
```

Every file must be exactly 921600 bytes.  A different hash for changing scene
content proves the userspace read path is active, but the raw image must still
be converted/viewed to validate line geometry and RGB/BGR byte order.

Copy a raw file back to the Ubuntu development machine and generate both byte
order interpretations with the included standard-library-only tool:

```sh
python3 raw_to_ppm.py frame0.raw frame0.ppm
```

The converter now defaults to the measured BGR input order.  `--order rgb`
remains available as a diagnostic and visibly swaps red and blue.  A
geometrically correct image confirms width, height and stride.  `--flip-y` can
be added only if rows are vertically reversed.  Do not hide a stride error by
using the flip or byte-swap options: colour reversal and row displacement are
different faults.

Unload with:

```sh
rmmod studyboard_frame_capture
```

The recipe does not request unconditional module autoload, but `udev` may
still load it from the device-tree modalias.  Startup ordering remains
explicit because probe leaves `running=0` until userspace writes `1`.

The external-module `Makefile` contains explicit `all`, `modules_install` and
`clean` targets because the PetaLinux 2020.2 `module` class invokes `make` in
the recipe work directory and passes `KERNEL_SRC` to it.

The two VDMAs are PS-DDR masters in the same 32-bit Zynq physical address
space.  VDMA1 therefore reads the same coherent frame addresses allocated for
VDMA0.  This is deliberately board-specific and is not a generic IOMMU-safe
buffer-sharing design.

The sysfs capture/display path still independently rotates over the same three
fixed addresses. It is useful for LCD bring-up but can tear because it has no
producer/consumer ownership. Use `/dev/videoX` for later OpenCV/YOLO work.

## Standard V4L2/MMAP capture

Kernel prerequisites already verified in this PetaLinux 2020.2 project are
`CONFIG_MEDIA_SUPPORT`, `CONFIG_VIDEO_V4L2`, the videobuf2 core/V4L2 helpers,
and `CONFIG_VIDEOBUF2_DMA_CONTIG`. In `petalinux-config -c rootfs`, enable the
`v4l-utils` package so the target root filesystem contains `v4l2-ctl`.
`media-ctl` is optional and is not required by this single-node teaching
driver because it does not yet register an OV5640 subdevice/media graph.

The current V4L2 contract is intentionally fixed and small:

- capture type: single-planar `VIDEO_CAPTURE`;
- memory model: `MMAP`;
- format: 640x480 packed BGR24 (`BGR3`), 1920 bytes per line;
- payload: 921600 bytes per completed frame;
- at least three queued buffers before `STREAMON`;
- VDMA0 only; the V4L2 test does not start the LCD/VDMA1 path.

The linux-xlnx 2020.2 VDMA channel must remain continuously enabled while
these queued buffers are recycled.  In particular, the driver leaves
`FRAMECNT_EN` disabled: enabling the finite frame counter stops this VDMA
version after the first few descriptors and makes `DQBUF` wait forever.
Park mode is still used, but the Xilinx DMAEngine interrupt handler advances
`PARK_PTR` to the next pending buffer after every completed frame.

After installing the rebuilt image, initialize the OV5640 and make sure the
legacy sysfs path is stopped before opening the V4L2 device:

```sh
P=/sys/bus/platform/devices/studyboard-frame-capture
echo 0 > "$P/running"
ov5640-init

ls -l /dev/video*
v4l2-ctl --list-devices
v4l2-ctl -d /dev/video0 --all
v4l2-ctl -d /dev/video0 --list-formats-ext
```

The format listing must contain `BGR3`, 640x480. Capture exactly one complete
frame first:

```sh
v4l2-ctl -d /dev/video0 \
    --set-fmt-video=width=640,height=480,pixelformat=BGR3 \
    --stream-mmap=3 --stream-count=1 \
    --stream-to=/tmp/v4l2-frame0.raw

wc -c /tmp/v4l2-frame0.raw
```

The size must be exactly 921600 bytes. Then test sustained queue recycling:

```sh
v4l2-ctl -d /dev/video0 \
    --set-fmt-video=width=640,height=480,pixelformat=BGR3 \
    --stream-mmap=4 --stream-count=100 \
    --stream-to=/tmp/v4l2-100.raw

wc -c /tmp/v4l2-100.raw
dmesg | tail -n 50
```

The expected size is 92160000 bytes and the command must finish without DMA,
timeout, or kernel errors. `v4l2-ctl` automatically executes the standard
sequence `REQBUFS -> mmap -> QBUF -> STREAMON -> DQBUF/re-QBUF -> STREAMOFF`.
Closing it releases VDMA0, after which the legacy LCD test may be started
again with the sysfs commands above.

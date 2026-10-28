# StudyBoard NCNN YOLOv8n face detector

For the LCD stage, `start-face-lcd.sh` consolidates the verified post-reboot
startup sequence. The application also accepts an optional `preview` mode
after the LCD sysfs directory: the default `matched` mode is the proven
inference-matched image, while preview publishes every other captured frame
with the latest detection boxes. A 9000-frame board trial on 2026-10-13
captured 9000/9000 frames over 314.828 seconds at 28.59 fps with zero
sequence gaps/bad payloads, published 4485 LCD frames and exited with code 0.
The physical screen kept updating throughout. The user also confirmed manual
startup and visible detection after both a normal reboot and a power cycle;
automatic startup is not installed.
`build-preview-app.sh` rebuilds only the application;
`install-preview.sh` creates a separate SD-card trial and preserves the
known-good bundle. See `LCD_部署说明.md` for the exact commands.

This application is the first board-side inference milestone.  It combines
the already validated `/dev/video0` V4L2 MMAP capture path with the exported
Ultralytics NCNN model.

## Fixed model contract

- camera: 640x480 `BGR24`;
- letterbox: resize to 320x240, pad 40 rows above and below with value 114;
- model input: `in0`, RGB float32 CHW, `[1, 3, 320, 320]`, scale `1/255`;
- model output: `out0`, `[1, 5, 2100]`;
- output rows: `cx`, `cy`, `width`, `height`, `face confidence`;
- postprocess: confidence filter, one-class NMS, undo letterbox, clamp to
  640x480 coordinates.

The capture and inference threads use a two-slot latest-frame mailbox.  Slow
inference skips old frames instead of retaining V4L2 buffers and increasing
latency.

## Optional PC live video over Ethernet

The network sender is **off by default**. Set `STUDYBOARD_STREAM_PORT=5000`
when starting the existing face/LCD application to enable a one-PC TCP server.
`STUDYBOARD_STREAM_STRIDE=6` (the default) samples about 4.75 raw BGR24
frames/s from a 28.5-fps camera; the LCD and detector continue to use their
original path. The stream contains the raw camera image, while the LCD keeps
its detection boxes. The sender owns two independent frame slots, sends on a
separate thread, and replaces old pending network frames if the PC is slow.
PC disconnects and send timeouts do not stop the capture or inference threads;
a bind/listen failure at startup is reported as an error.

The wire format is a 20-byte network-byte-order header
`!4sHH4sII`: `SBV1`, width, height, `BGR3`, source V4L2 sequence,
payload byte count, followed by exactly width × height × 3 BGR bytes. Use
`pc_video_viewer.py` on Windows to connect to `192.168.10.2:5000` and display
the frames. Do not expose this unauthenticated raw-video port to the Internet.
The driver allows only one V4L2 owner, so do not run a second `/dev/video0`
reader alongside the face/LCD application. See `NETWORK_部署说明.md` for the
build, trial deployment and checks.

## Version lock

The supplied model was exported by:

- Ultralytics `8.4.135`;
- pnnx `20260526`;
- ncnn Python package `1.0.20260526`.

Build the target runtime from NCNN tag `20260526` so converter and runtime are
not silently mixed.

## Build

Open a clean Ubuntu shell, source the installed PetaLinux SDK, then build NCNN:

```sh
unset LD_LIBRARY_PATH
source /home/ubuntu/petalinux/ALIENTEK-ZYNQ/images/linux/sdk/environment-setup-cortexa9t2hf-neon-xilinx-linux-gnueabi

chmod +x build-ncnn-zynq.sh
./build-ncnn-zynq.sh /home/ubuntu/src/ncnn /home/ubuntu/ncnn-zynq
```

The included `petalinux-zynq-toolchain.cmake` first imports the SDK-provided
`OEToolchainConfig.cmake`, then normalises `CMAKE_SYSTEM_PROCESSOR` to `arm`.
Without that wrapper NCNN can misclassify this SDK's processor string as x86
and incorrectly add `-msse/-msse2` to the ARM compiler command.  A correct
configure log must contain `Target arch: arm 32bit`.

Build this application with the same SDK environment:

```sh
make clean
make -j2 NCNN_INSTALL=/home/ubuntu/ncnn-zynq
file studyboard-ncnn-face
```

The Makefile links `libncnn.a` and `libgcc` into the application.  This SDK
does not ship a static `libstdc++.a`, so `libstdc++.so.6` remains dynamic.  The
binary contains an `$ORIGIN/lib` runtime search path: when the target rootfs
does not already provide the required C++ runtime, copy the SDK's
`libstdc++.so.6` and `libgcc_s.so.1` into a `lib` directory beside the
executable.  NCNN itself is built without Vulkan and without OpenMP.

## Dual-core performance build

The minimal PetaLinux 2020.2 SDK may not contain GNU `omp.h` or `libgomp`.
If its compiler reports `omp.h: No such file or directory`, use NCNN's
pthread-based `simpleomp` runtime.  Keep the verified serial install intact
and build a separate NCNN variant:

```sh
unset LD_LIBRARY_PATH
source /home/ubuntu/petalinux/ALIENTEK-ZYNQ/images/linux/sdk/environment-setup-cortexa9t2hf-neon-xilinx-linux-gnueabi

NCNN_OPENMP=ON NCNN_SIMPLEOMP=ON ./build-ncnn-zynq.sh \
  /home/ubuntu/src/ncnn \
  /home/ubuntu/ncnn-zynq-simpleomp

make clean
make -j2 \
  NCNN_INSTALL=/home/ubuntu/ncnn-zynq-simpleomp \
  OPENMP=simple
```

The final executable must *not* list `libgomp.so.1`; its parallel runtime is
already included in the static NCNN library:

```sh
arm-xilinx-linux-gnueabi-readelf -d studyboard-ncnn-face | \
  grep -E 'NEEDED|RPATH|RUNPATH'
```

Run the application with `threads=2` and check for `parallel_runtime: NCNN
simpleomp enabled`.  Compare its `average_ms` and `effective_inference_fps`
against the serial baseline.  Do not compare only wall-clock time, because
the capture thread continues at camera rate and intentionally skips stale
frames.

## Deploy and run

Place these files on the board:

```text
/home/root/studyboard-ncnn-face
/home/root/face-model/model.ncnn.param
/home/root/face-model/model.ncnn.bin
```

Stop both legacy DMA paths, initialize OV5640, then capture 300 camera frames:

```sh
P=/sys/bus/platform/devices/studyboard-frame-capture
echo 0 > "$P/display_running"
echo 0 > "$P/running"
ov5640-init

chmod +x /home/root/studyboard-ncnn-face
/home/root/studyboard-ncnn-face \
  /dev/video0 \
  /home/root/face-model/model.ncnn.param \
  /home/root/face-model/model.ncnn.bin \
  300 1 0.25 0.45 \
  /home/root/face-last.ppm
```

Use `0` instead of `300` to run until `Ctrl-C`.  The program prints detection
scores, source-image boxes, preprocessing/inference time, skipped stale-frame
count and capture integrity.  `face-last.ppm` contains the final processed
640x480 frame with red face rectangles.

## Annotated LCD output

An optional ninth positional argument enables the new driver-specific LCD
output, e.g. `/sys/bus/platform/devices/studyboard-frame-capture`. Omitting it
or supplying `-` preserves capture/inference-only behavior. `lcd_output.cpp`
writes a complete BGR24 image to `lcd_frame`, then writes `1` to `lcd_commit`.
The driver uses its legacy coherent allocation exclusively for display while
V4L2 owns its separate vb2 capture allocation. Legacy sysfs capture and this
LCD mode cannot run together.

Board testing proved that this design's LCD AXI sink requires continuous
MM2S. The driver therefore starts VDMA1 once with `frm_cnt_en=0` and makes all
hardware frame stores repeat one stable front buffer. Sysfs writes build a
complete image in a separate back buffer; `lcd_commit` copies that complete
image to the front without stopping or resetting VDMA1. A strict tear-free
frame-boundary switch remains a later enhancement to the lower Xilinx driver.

The displayed image is the exact source image used by the detector, including
its boxes. It is not a 28.5-fps live preview with old boxes. With the measured
~868-ms inference, expect approximately one new annotated image per second,
not 28.5 detections/s. Full-frame LCD upload adds time and needs measurement.

The deployment bundle includes `build-lcd-bundle.sh`, `load-lcd-driver.sh`,
and `run-face-lcd.sh`. The first script builds only the changed kernel module
and application using the existing SIMPLEOMP NCNN library. It does not rebuild
NCNN or generate a new rootfs/SD image. Source backups are created before
updating the actual PetaLinux module recipe. The board script hot-loads the
module after checking checksums and kernel release; it does not overwrite the
installed module. A reboot restores the installed version. The run script
records the kernel boot ID and initializes OV5640 only once per boot; set
`STUDYBOARD_FORCE_SENSOR_INIT=1` only when a deliberate sensor reconfiguration
is required.

Host tests cover the userspace transport and application syntax, not VDMA or
LCD timing. Accept the board version only after repeated runs show correct
colors/line alignment, changing image/boxes, clean capture statistics, and
successful Ctrl-C followed by restart. See the bundle's Chinese deployment
guide for the concrete commands and rollback path.

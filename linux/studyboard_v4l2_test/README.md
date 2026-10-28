# Studyboard V4L2 MMAP test

This small userspace program is the readable C equivalent of the
`v4l2-ctl --stream-mmap` test.  It verifies the fixed 640x480 BGR24 capture
contract and explicitly performs `REQBUFS`, `QUERYBUF`, `mmap`, `QBUF`,
`STREAMON`, repeated `DQBUF/QBUF`, and `STREAMOFF`.

For every dequeued frame it checks:

- `bytesused == 640 * 480 * 3 == 921600`;
- the V4L2 sequence number has no gap;
- all payload bytes are CPU-readable by calculating a 32-bit FNV-1a digest.

It intentionally does not save all frames, so a 1000-frame test does not
consume about 922 MB of storage.

Run on the board after `ov5640-init` and after stopping the legacy sysfs path:

```sh
P=/sys/bus/platform/devices/studyboard-frame-capture
echo 0 > "$P/display_running"
echo 0 > "$P/running"
ov5640-init
studyboard-v4l2-test /dev/video0 1000
echo "return=$?"
```

A successful summary has `frames=1000/1000`, `sequence_gaps=0`,
`bad_payloads=0`, and exit status zero. `digest_changes` is informational:
a live sensor normally changes between frames because of motion and sensor
noise, but an unchanged digest is not itself a transport error.

## Latest-frame producer/consumer example

`studyboard-v4l2-latest` separates fast capture from slower processing. The
main thread copies every dequeued MMAP frame into a two-slot CPU mailbox and
immediately requeues the V4L2 buffer. A worker hashes only the newest complete
mailbox frame. If the worker is slower than the camera, stale frames are
intentionally replaced and counted as `dropped_old`; capture should still stay
close to the sensor rate.

```sh
studyboard-v4l2-latest /dev/video0 1000 0
```

The optional last argument adds a simulated inference delay. For example,
80 ms is deliberately slower than a roughly 35 ms camera period:

```sh
studyboard-v4l2-latest /dev/video0 1000 80
```

`capture_summary` should remain close to 28.6 fps with no sequence gap or bad
payload. `processing_summary` will report fewer processed frames and a larger
`dropped_old`, demonstrating a low-latency latest-frame policy instead of
blocking capture behind a slow algorithm.

## CPU preprocessing benchmark

`studyboard-v4l2-preprocess` keeps the same latest-frame capture policy and
implements the input path normally placed immediately before an inference
engine:

1. 640x480 BGR24 to RGB24 channel conversion;
2. bilinear resize (224x224 by default);
3. interleaved RGB uint8 to planar CHW float32 normalized to `[0, 1]`;
4. per-stage timing and output-tensor range/mean statistics.

The optional PPM path saves only the last resized RGB image, so it verifies the
preprocessing result without continuously writing the SD card:

```sh
studyboard-v4l2-preprocess /dev/video0 300 224 224 \
    /home/root/preprocess-last.ppm
```

`sequence_gaps=0` and `bad_payloads=0` validate capture. `average_ms` provides
the Cortex-A9 CPU preprocessing baseline. `dropped_old` is allowed: it means
the worker intentionally skipped stale images while capture continued.

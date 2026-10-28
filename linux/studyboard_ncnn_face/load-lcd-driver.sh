#!/bin/sh
# Board-side, one-time hot reload. Does not replace the installed module on disk.
set -eu
BUNDLE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
P=/sys/bus/platform/devices/studyboard-frame-capture
test "$(id -u)" = 0
test -f "$BUNDLE/studyboard-frame-capture.ko"
test -x /sbin/insmod || command -v insmod >/dev/null
(cd "$BUNDLE" && sha256sum -c SHA256SUMS)
if [ "$(cat "$BUNDLE/KERNEL-RELEASE")" != "$(uname -r)" ]; then
  echo 'Module kernel version differs from the running kernel. Existing driver was not changed.' >&2
  exit 1
fi
if [ -d "$P" ]; then
  # Do not force removal. If another app holds /dev/videoX, rmmod must fail.
  if [ -r /sys/module/studyboard_frame_capture/refcnt ] &&
     [ "$(cat /sys/module/studyboard_frame_capture/refcnt)" -ne 0 ]; then
    echo 'Close other V4L2 applications before reloading the driver.' >&2
    exit 1
  fi
  echo 0 > "$P/display_running"
  echo 0 > "$P/running"
  rmmod studyboard_frame_capture
fi
if ! insmod "$BUNDLE/studyboard-frame-capture.ko"; then
  echo 'New module could not load. Attempting to restore the installed version.' >&2
  modprobe studyboard_frame_capture || true
  exit 1
fi
test -w "$P/lcd_frame"
test -w "$P/lcd_commit"
cat "$P/geometry" "$P/lcd_status"
echo 'LCD-enabled module loaded. Next: sh ./run-face-lcd.sh 300'
echo 'Rollback without replacing disk files: rmmod studyboard_frame_capture; modprobe studyboard_frame_capture'

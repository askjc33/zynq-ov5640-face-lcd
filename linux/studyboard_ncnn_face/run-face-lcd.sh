#!/bin/sh
# Board-side. 300 = about ten seconds of capture; 0 = run until Ctrl-C.
set -eu
BUNDLE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
P=/sys/bus/platform/devices/studyboard-frame-capture
FRAMES=${1:-300}
LCD_MODE=${STUDYBOARD_LCD_MODE:-matched}
PREVIEW_STRIDE=${STUDYBOARD_PREVIEW_STRIDE:-2}
MODEL_SIZE=${STUDYBOARD_MODEL_SIZE:-320}
SENSOR_STATE=${STUDYBOARD_SENSOR_STATE:-/tmp/studyboard-ov5640-boot-id}
SENSOR_INIT=${STUDYBOARD_SENSOR_INIT:-ov5640-init}
if [ -x "$BUNDLE/ov5640-init" ]; then
  SENSOR_INIT=$BUNDLE/ov5640-init
fi
case "$LCD_MODE" in
  matched|preview) ;;
  *) echo 'STUDYBOARD_LCD_MODE must be matched or preview' >&2; exit 1 ;;
esac
case "$MODEL_SIZE" in
  320) MODEL_DIR="$BUNDLE/face-model" ;;
  192|224|256) MODEL_DIR="$BUNDLE/face-model-$MODEL_SIZE" ;;
  *) echo 'STUDYBOARD_MODEL_SIZE must be 192, 224, 256 or 320' >&2; exit 1 ;;
esac
test -w "$P/lcd_frame" || {
  echo 'First run sh ./load-lcd-driver.sh in this directory.' >&2
  exit 1
}
test -f "$BUNDLE/studyboard-ncnn-face-lcd"
test -f "$MODEL_DIR/model.ncnn.param"
test -f "$MODEL_DIR/model.ncnn.bin"
chmod +x "$BUNDLE/studyboard-ncnn-face-lcd"
echo 0 > "$P/display_running"
echo 0 > "$P/running"

# Reprogramming OV5640 before every V4L2 session can leave the PL receive path
# waiting for a new frame boundary.  The sensor configuration survives driver
# reloads, so initialise it only once per Linux boot.  boot_id makes this safe
# even when /tmp lives on persistent storage.
BOOT_ID=$(cat /proc/sys/kernel/random/boot_id 2>/dev/null || echo unknown)
INITIALISED_BOOT=$(cat "$SENSOR_STATE" 2>/dev/null || true)
if [ "${STUDYBOARD_FORCE_SENSOR_INIT:-0}" = 1 ] || \
   [ "$INITIALISED_BOOT" != "$BOOT_ID" ]; then
  "$SENSOR_INIT"
  printf '%s\n' "$BOOT_ID" > "$SENSOR_STATE"
else
  echo "OV5640 already initialized for this boot; skipping ov5640-init."
fi

if [ "$LCD_MODE" = preview ]; then
  exec "$BUNDLE/studyboard-ncnn-face-lcd" /dev/video0 \
    "$MODEL_DIR/model.ncnn.param" \
    "$MODEL_DIR/model.ncnn.bin" \
    "$FRAMES" 2 0.25 0.45 "$BUNDLE/face-lcd-last.ppm" "$P" \
    preview "$PREVIEW_STRIDE" "$MODEL_SIZE"
fi
exec "$BUNDLE/studyboard-ncnn-face-lcd" /dev/video0 \
  "$MODEL_DIR/model.ncnn.param" \
  "$MODEL_DIR/model.ncnn.bin" \
  "$FRAMES" 2 0.25 0.45 "$BUNDLE/face-lcd-last.ppm" "$P" \
  matched "$PREVIEW_STRIDE" "$MODEL_SIZE"

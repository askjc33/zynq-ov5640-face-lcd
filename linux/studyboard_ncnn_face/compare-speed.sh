#!/bin/sh
# Four matched-condition capture/display runs; first and last 320 check drift.
set -eu

BUNDLE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
FRAMES=${1:-600}
case "$FRAMES" in
  ''|*[!0-9]*|0) echo 'usage: sh ./compare-speed.sh [positive-frame-count]' >&2; exit 1 ;;
esac
test -f "$BUNDLE/start-face-lcd.sh"
test -f "$BUNDLE/face-model-192/model.ncnn.param"
test -f "$BUNDLE/face-model-224/model.ncnn.param"
test -f "$BUNDLE/face-model-256/model.ncnn.param"

run_one()
{
  size=$1
  label=$2
  log="$BUNDLE/speed-$label.log"
  echo "===== $label: $FRAMES camera frames, model input ${size}x${size} ====="
  if STUDYBOARD_LCD_MODE=preview STUDYBOARD_MODEL_SIZE="$size" \
      sh "$BUNDLE/start-face-lcd.sh" "$FRAMES" > "$log" 2>&1; then
    grep -E '^(model:|capture_summary:|inference_summary:|average_ms:|lcd_summary:)' "$log" || true
    echo "full_log=$log"
  else
    code=$?
    tail -n 35 "$log"
    echo "FAILED: $label return=$code full_log=$log" >&2
    exit "$code"
  fi
}

run_one 320 320-first
run_one 224 224
run_one 256 256
run_one 192 192
run_one 320 320-last
echo 'All comparisons completed; inspect image/boxes and both 320 runs before selecting a model.'

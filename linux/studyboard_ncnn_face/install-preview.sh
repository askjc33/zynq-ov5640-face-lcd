#!/bin/sh
# Board-side: install a preview trial alongside the known-good SD bundle.
set -eu
UPDATE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
SD_ROOT=/media/sd-mmcblk0p2
BASE="$SD_ROOT/face-lcd-recovery-20261012-01"
TRIAL="$SD_ROOT/face-lcd-preview-20261012-01"

[ "$(id -u)" -eq 0 ] || { echo 'run as root' >&2; exit 1; }
[ -d "$BASE" ] || { echo "baseline missing: $BASE" >&2; exit 1; }
[ ! -e "$TRIAL" ] || { echo "trial already exists; preserving: $TRIAL" >&2; exit 1; }
[ -f "$BASE/ov5640-init" ] || { echo 'baseline sensor init missing' >&2; exit 1; }
(cd "$UPDATE" && sha256sum -c SHA256SUMS)

STAGING=$(mktemp -d "$SD_ROOT/.face-lcd-preview.XXXXXX")
echo "Copying known-good runtime to $STAGING"
cp -a "$BASE/." "$STAGING/"
cp "$UPDATE/studyboard-ncnn-face-lcd" "$UPDATE/run-face-lcd.sh" \
  "$UPDATE/start-face-lcd.sh" "$STAGING/"
(
  cd "$STAGING"
  sha256sum studyboard-frame-capture.ko studyboard-ncnn-face-lcd \
    face-model/model.ncnn.param face-model/model.ncnn.bin \
    lib/libstdc++.so.6 lib/libgcc_s.so.1 \
    load-lcd-driver.sh run-face-lcd.sh start-face-lcd.sh ov5640-init \
    KERNEL-RELEASE > SHA256SUMS
  sha256sum -c SHA256SUMS
)
sync
mv "$STAGING" "$TRIAL"
echo "Trial ready: $TRIAL"
echo "Known-good baseline unchanged: $BASE"
echo "Test: cd '$TRIAL' && STUDYBOARD_LCD_MODE=preview sh ./start-face-lcd.sh 300"

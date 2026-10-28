#!/bin/sh
# Install a separate SD trial; never overwrite the tested preview bundle.
set -eu

UPDATE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
SD_ROOT=/media/sd-mmcblk0p2
BASE="$SD_ROOT/face-lcd-preview-20261012-01"
TRIAL="$SD_ROOT/face-lcd-speed-20261013-01"

[ "$(id -u)" -eq 0 ] || { echo 'run as root on the board' >&2; exit 1; }
[ -d "$BASE" ] || { echo "tested preview bundle missing: $BASE" >&2; exit 1; }
[ ! -e "$TRIAL" ] || { echo "trial already exists; preserving: $TRIAL" >&2; exit 1; }
(cd "$UPDATE" && sha256sum -c SHA256SUMS)

STAGING=$(mktemp -d "$SD_ROOT/.face-lcd-speed.XXXXXX")
echo "Copying known-good runtime to $STAGING"
cp -a "$BASE/." "$STAGING/"
cp "$UPDATE/studyboard-ncnn-face-lcd" "$UPDATE/run-face-lcd.sh" \
  "$UPDATE/compare-speed.sh" "$STAGING/"
mkdir -p "$STAGING/face-model-192" "$STAGING/face-model-224" "$STAGING/face-model-256"
for size in 192 224 256; do
  cp "$UPDATE/model-$size/model.ncnn.param" \
    "$UPDATE/model-$size/model.ncnn.bin" "$STAGING/face-model-$size/"
done
(
  cd "$STAGING"
  sha256sum studyboard-frame-capture.ko studyboard-ncnn-face-lcd \
    face-model/model.ncnn.param face-model/model.ncnn.bin \
    face-model-192/model.ncnn.param face-model-192/model.ncnn.bin \
    face-model-224/model.ncnn.param face-model-224/model.ncnn.bin \
    face-model-256/model.ncnn.param face-model-256/model.ncnn.bin \
    lib/libstdc++.so.6 lib/libgcc_s.so.1 \
    load-lcd-driver.sh run-face-lcd.sh start-face-lcd.sh \
    compare-speed.sh ov5640-init KERNEL-RELEASE > SHA256SUMS
  sha256sum -c SHA256SUMS
)
sync
mv "$STAGING" "$TRIAL"
echo "Trial ready: $TRIAL"
echo "Known-good preview unchanged: $BASE"
echo "Test: cd '$TRIAL' && sh ./compare-speed.sh"

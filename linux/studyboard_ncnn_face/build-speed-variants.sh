#!/bin/bash
# Build one user-space binary and stage both smaller NCNN models for comparison.
set -euo pipefail

BUNDLE=$(cd -- "$(dirname -- "$0")" && pwd)
PL_PROJECT=${1:-/home/ubuntu/petalinux/ALIENTEK-ZYNQ}
NCNN_INSTALL=${2:-/home/ubuntu/ncnn-zynq-20260526-simpleomp}
SDK_ENV="$PL_PROJECT/images/linux/sdk/environment-setup-cortexa9t2hf-neon-xilinx-linux-gnueabi"
UPDATE="$BUNDLE/speed-update-20261013-01"

test -f "$SDK_ENV"
test -f "$NCNN_INSTALL/lib/libncnn.a"
grep -qE '^#define NCNN_SIMPLEOMP +1' "$NCNN_INSTALL/include/ncnn/platform.h"
test -f "$BUNDLE/app/face_detector.cpp"
test -f "$BUNDLE/app/studyboard-ncnn-face.cpp"
test -f "$BUNDLE/run-face-lcd.sh"
test -f "$BUNDLE/install-speed-variants.sh"
test -f "$BUNDLE/compare-speed.sh"
for size in 192 224 256; do
  test -f "$BUNDLE/models/$size/model.ncnn.param"
  test -f "$BUNDLE/models/$size/model.ncnn.bin"
done
if [ -n "${SDKTARGETSYSROOT:-}" ]; then
  echo 'Use a fresh Ubuntu shell without another SDK environment sourced.' >&2
  exit 1
fi

mkdir -p "$UPDATE/model-192" "$UPDATE/model-224" "$UPDATE/model-256"
(
  unset LD_LIBRARY_PATH
  set +u
  source "$SDK_ENV"
  set -u
  cd "$BUNDLE/app"
  make clean APP=studyboard-ncnn-face-lcd
  make -j2 APP=studyboard-ncnn-face-lcd NCNN_INSTALL="$NCNN_INSTALL" OPENMP=simple
  file studyboard-ncnn-face-lcd | grep -q 'ELF 32-bit.*ARM'
  cp studyboard-ncnn-face-lcd "$UPDATE/"
  arm-xilinx-linux-gnueabi-readelf -d studyboard-ncnn-face-lcd | \
    grep -E 'NEEDED|RPATH|RUNPATH'
)
cp "$BUNDLE/run-face-lcd.sh" "$BUNDLE/install-speed-variants.sh" \
  "$BUNDLE/compare-speed.sh" "$UPDATE/"
for size in 192 224 256; do
  cp "$BUNDLE/models/$size/model.ncnn.param" \
    "$BUNDLE/models/$size/model.ncnn.bin" "$UPDATE/model-$size/"
done
(
  cd "$UPDATE"
  sha256sum studyboard-ncnn-face-lcd run-face-lcd.sh \
    install-speed-variants.sh compare-speed.sh \
    model-192/model.ncnn.param model-192/model.ncnn.bin \
    model-224/model.ncnn.param model-224/model.ncnn.bin \
    model-256/model.ncnn.param model-256/model.ncnn.bin > SHA256SUMS
  sha256sum -c SHA256SUMS
)
file "$UPDATE/studyboard-ncnn-face-lcd"
echo "Ready: $UPDATE"
echo 'Only the application was rebuilt; NCNN, the kernel, and the 320 model remain unchanged.'

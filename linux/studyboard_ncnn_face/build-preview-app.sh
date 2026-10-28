#!/bin/bash
# Rebuild only the user-space NCNN application; preserve the working kernel.
set -euo pipefail
BUNDLE=$(cd -- "$(dirname -- "$0")" && pwd)
PL_PROJECT=${1:-/home/ubuntu/petalinux/ALIENTEK-ZYNQ}
NCNN_INSTALL=${2:-/home/ubuntu/ncnn-zynq-20260526-simpleomp}
SDK_ENV="$PL_PROJECT/images/linux/sdk/environment-setup-cortexa9t2hf-neon-xilinx-linux-gnueabi"
UPDATE="$BUNDLE/preview-update-20261012-01"

test -f "$SDK_ENV"
test -f "$NCNN_INSTALL/lib/libncnn.a"
grep -qE '^#define NCNN_SIMPLEOMP +1' "$NCNN_INSTALL/include/ncnn/platform.h"
test -f "$BUNDLE/app/studyboard-ncnn-face.cpp"
test -f "$BUNDLE/run-face-lcd.sh"
test -f "$BUNDLE/start-face-lcd.sh"
test -f "$BUNDLE/install-preview.sh"
if [ -n "${SDKTARGETSYSROOT:-}" ]; then
  echo 'Open a fresh Ubuntu terminal without another SDK sourced, then rerun.' >&2
  exit 1
fi

mkdir -p "$UPDATE"
(
  unset LD_LIBRARY_PATH
  set +u
  source "$SDK_ENV"
  set -u
  cd "$BUNDLE/app"
  make clean APP=studyboard-ncnn-face-lcd
  make -j2 APP=studyboard-ncnn-face-lcd NCNN_INSTALL="$NCNN_INSTALL" OPENMP=simple
  cp studyboard-ncnn-face-lcd "$UPDATE/"
  arm-xilinx-linux-gnueabi-readelf -d studyboard-ncnn-face-lcd | \
    grep -E 'NEEDED|RPATH|RUNPATH'
)
cp "$BUNDLE/run-face-lcd.sh" "$BUNDLE/start-face-lcd.sh" \
  "$BUNDLE/install-preview.sh" "$UPDATE/"
(
  cd "$UPDATE"
  sha256sum studyboard-ncnn-face-lcd run-face-lcd.sh start-face-lcd.sh \
    install-preview.sh > SHA256SUMS
)
file "$UPDATE/studyboard-ncnn-face-lcd"
echo "Ready: $UPDATE"
echo 'Only the user-space application was rebuilt; module, NCNN and model were not rebuilt.'

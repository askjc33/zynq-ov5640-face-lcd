#!/bin/bash
# Run in a fresh PetaLinux shell, NOT a shell that has sourced the target SDK.
set -euo pipefail
BUNDLE=$(cd -- "$(dirname -- "$0")" && pwd)
PL_PROJECT=${1:-/home/ubuntu/petalinux/ALIENTEK-ZYNQ}
NCNN_INSTALL=${2:-/home/ubuntu/ncnn-zynq-20260526-simpleomp}
MODEL_SRC=${3:-"$BUNDLE/../studyboard_ncnn_face/model"}
SDK_ENV="$PL_PROJECT/images/linux/sdk/environment-setup-cortexa9t2hf-neon-xilinx-linux-gnueabi"
SENSOR_SOURCE="$BUNDLE/sensor"
if [ ! -f "$SENSOR_SOURCE/ov5640-init.c" ]; then
  SENSOR_SOURCE="$BUNDLE/../ov5640_init"
fi

if [ -n "${SDKTARGETSYSROOT:-}" ]; then
  echo 'Open a fresh Ubuntu terminal, source PetaLinux settings.sh only, then rerun.' >&2
  exit 1
fi
command -v petalinux-build >/dev/null || {
  echo 'First source /opt/pkg/petalinux/2020.2/settings.sh in this terminal.' >&2
  exit 1
}
test -f "$SDK_ENV"
test -f "$NCNN_INSTALL/lib/libncnn.a"
test -f "$NCNN_INSTALL/include/ncnn/simpleomp.h"
grep -qE '^#define NCNN_SIMPLEOMP +1' "$NCNN_INSTALL/include/ncnn/platform.h" || {
  echo 'NCNN install must be the already verified SIMPLEOMP build.' >&2
  exit 1
}
test -f "$MODEL_SRC/model.ncnn.param"
test -f "$MODEL_SRC/model.ncnn.bin"
test -f "$SENSOR_SOURCE/ov5640-init.c"
test -f "$SENSOR_SOURCE/ov5640-init-table.h"

# Discover the actual recipe, so updating an unused source directory cannot
# silently yield an unchanged module. Refuse ambiguous copies.
mapfile -t RECIPES < <(find "$PL_PROJECT/project-spec/meta-user" \
  -type f -name studyboard-frame-capture.bb)
if [ "${#RECIPES[@]}" -ne 1 ]; then
  echo 'Expected exactly one studyboard-frame-capture.bb under meta-user.' >&2
  exit 1
fi
MODULE_FILES="$(dirname -- "${RECIPES[0]}")/files"
test -f "$MODULE_FILES/studyboard-frame-capture.c"
SOURCE_BACKUP=$(mktemp -d "$PL_PROJECT/lcd-source-backup.XXXXXX")
cp -a "$MODULE_FILES/studyboard-frame-capture.c" "$MODULE_FILES/Makefile" "$SOURCE_BACKUP/"
echo "Previous kernel module source saved at $SOURCE_BACKUP"
cp "$BUNDLE/driver/studyboard-frame-capture.c" "$BUNDLE/driver/Makefile" "$MODULE_FILES/"
# A normal recipe build may run do_rm_work after packaging, leaving no .ko in
# WORKDIR even though BitBake reports success.  Force only do_compile and copy
# the module immediately; this does not clean or rebuild the whole project.
(cd "$PL_PROJECT" && \
  petalinux-build -c studyboard-frame-capture -x compile -f)
# Resolve only the module recipe's work directory.  Following every symlink in
# build/tmp/work is unsafe because Yocto deliberately creates self-referential
# links such as linux-xlnx/source/oe-workdir.
WORK_ROOT=$(readlink -f "$PL_PROJECT/build/tmp/work")
test -d "$WORK_ROOT"
shopt -s nullglob
RECIPE_WORKDIRS=(
  "$WORK_ROOT"/*/studyboard-frame-capture/*
  "$WORK_ROOT"/*/studyboard_frame_capture/*
)
shopt -u nullglob
if [ "${#RECIPE_WORKDIRS[@]}" -eq 0 ]; then
  echo "No studyboard-frame-capture recipe work directory under $WORK_ROOT" >&2
  echo 'Matching recipe directories seen:' >&2
  find "$WORK_ROOT" -mindepth 2 -maxdepth 4 -type d \
    \( -name '*studyboard*frame*capture*' -o -name '*frame*capture*' \) \
    -print >&2
  exit 1
fi

# Kbuild may keep '-' in the file name or normalise it to '_'.  Inspect every
# uncompressed kernel module in this recipe only, newest-first, and accept the
# one that contains this revision's continuous-copy LCD implementation.
mapfile -t MODULE_CANDIDATES < <(
  find -H "${RECIPE_WORKDIRS[@]}" -type f -name '*.ko' \
    ! -path '*/.debug/*' -printf '%T@ %p\n' | \
  sort -rn | cut -d' ' -f2-
)
MODULE_KO=
for candidate in "${MODULE_CANDIDATES[@]}"; do
  if grep -a -q 'mode=continuous-copy' "$candidate"; then
    MODULE_KO=$candidate
    break
  fi
done
if [ -z "$MODULE_KO" ]; then
  echo 'Build succeeded, but no LCD-enabled module candidate was found.' >&2
  printf 'Recipe work directories:\n' >&2
  printf '  %s\n' "${RECIPE_WORKDIRS[@]}" >&2
  echo 'Candidates seen:' >&2
  if [ "${#MODULE_CANDIDATES[@]}" -eq 0 ]; then
    echo '  (no uncompressed .ko files in this recipe work directory)' >&2
  else
    for candidate in "${MODULE_CANDIDATES[@]}"; do
      if grep -a -q 'mode=continuous-copy' "$candidate"; then
	marker='continuous-copy=yes'
      else
	marker='continuous-copy=no'
      fi
      printf '  %s (%s)\n' "$candidate" "$marker" >&2
    done
  fi
  exit 1
fi
echo "Selected module: $MODULE_KO"

mkdir -p "$BUNDLE/board/face-model" "$BUNDLE/board/lib"
cp "$MODULE_KO" "$BUNDLE/board/studyboard-frame-capture.ko"
strings "$MODULE_KO" | sed -n 's/^vermagic=\([^ ]*\).*/\1/p' > "$BUNDLE/board/KERNEL-RELEASE"
test -s "$BUNDLE/board/KERNEL-RELEASE"
cp "$MODEL_SRC/model.ncnn.param" "$MODEL_SRC/model.ncnn.bin" "$BUNDLE/board/face-model/"
(
  # Keep the SDK environment out of the PetaLinux/BitBake parent shell.
  unset LD_LIBRARY_PATH
  set +u
  source "$SDK_ENV"
  set -u
  cd "$BUNDLE/app"
  make clean APP=studyboard-ncnn-face-lcd
  make -j2 APP=studyboard-ncnn-face-lcd NCNN_INSTALL="$NCNN_INSTALL" OPENMP=simple
  cp studyboard-ncnn-face-lcd "$BUNDLE/board/"
  # This small standalone C tool is rebuilt with the same SDK, including
  # per-register SCCB retries; the board startup script requires this copy.
  $CC -O2 -Wall -Wextra -std=gnu11 \
    "$SENSOR_SOURCE/ov5640-init.c" -o "$BUNDLE/board/ov5640-init"
  cp -L "$SDKTARGETSYSROOT/usr/lib/libstdc++.so.6" "$BUNDLE/board/lib/"
  cp -L "$SDKTARGETSYSROOT/lib/libgcc_s.so.1" "$BUNDLE/board/lib/"
  arm-xilinx-linux-gnueabi-readelf -d studyboard-ncnn-face-lcd | \
    grep -E 'NEEDED|RPATH|RUNPATH'
)
cp "$BUNDLE/load-lcd-driver.sh" "$BUNDLE/run-face-lcd.sh" \
  "$BUNDLE/start-face-lcd.sh" "$BUNDLE/board/"
(
  cd "$BUNDLE/board"
  sha256sum studyboard-frame-capture.ko studyboard-ncnn-face-lcd \
    face-model/model.ncnn.param face-model/model.ncnn.bin \
    lib/libstdc++.so.6 lib/libgcc_s.so.1 \
    load-lcd-driver.sh run-face-lcd.sh start-face-lcd.sh ov5640-init \
    KERNEL-RELEASE > SHA256SUMS
)
file "$BUNDLE/board/studyboard-ncnn-face-lcd"
echo "Ready: $BUNDLE/board"
echo 'Only the module and application were built; no NCNN rebuild, image.ub update or SD-card rewrite.'

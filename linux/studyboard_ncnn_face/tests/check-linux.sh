#!/bin/bash
# Host-only transport tests and syntax checks. NOT an ARM or kernel build.
set -euo pipefail
APP_DIR=$(cd -- "$(dirname -- "$0")/.." && pwd)
TEST_DIR=$(mktemp -d /tmp/studyboard-lcd-check.XXXXXX)
echo "Test artifacts: $TEST_DIR"
g++ -std=gnu++11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -g -I"$APP_DIR" "$APP_DIR/lcd_output.cpp" \
  "$APP_DIR/tests/lcd_output_test.cpp" -o "$TEST_DIR/lcd-output-test"
"$TEST_DIR/lcd-output-test"
echo 'LCD transport tests passed (regular-file backend, not real DMA).'
g++ -std=gnu++11 -Wall -Wextra -Werror -pthread -fsyntax-only \
  "$APP_DIR/video_stream_server.cpp"
echo 'Network sender syntax check passed (board runtime still requires testing).'
if [ "$#" -eq 1 ]; then
  mkdir "$TEST_DIR/ncnn"
  tar -xzf "$1" -C "$TEST_DIR/ncnn"
  cmake -S "$TEST_DIR/ncnn" -B "$TEST_DIR/ncnn-build" \
    -DNCNN_VULKAN=OFF -DNCNN_OPENMP=ON -DNCNN_SIMPLEOMP=ON \
    -DNCNN_BUILD_TOOLS=OFF -DNCNN_BUILD_EXAMPLES=OFF \
    -DNCNN_BUILD_BENCHMARK=OFF > "$TEST_DIR/ncnn-configure.log" 2>&1
  g++ -std=gnu++11 -Wall -Wextra -Werror -fsyntax-only \
    -I"$TEST_DIR/ncnn/src" -I"$TEST_DIR/ncnn-build/src" \
    "$APP_DIR/studyboard-ncnn-face.cpp" "$APP_DIR/face_detector.cpp" \
    "$APP_DIR/lcd_output.cpp" "$APP_DIR/video_stream_server.cpp"
  echo 'Complete application syntax check passed with actual NCNN headers.'
fi

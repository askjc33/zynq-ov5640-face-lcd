#!/bin/sh
# Run the SD-card face/LCD bundle after boot. 0 = run until Ctrl-C.
set -eu

BUNDLE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
P=/sys/bus/platform/devices/studyboard-frame-capture
STATE=/tmp/studyboard-ov5640-boot-id
ATTEMPT_STATE=/tmp/studyboard-ov5640-init-attempt-boot-id
FRAMES=${1:-0}

fail()
{
	echo "start-face-lcd: $*" >&2
	exit 1
}

test_capture()
{
	echo "start-face-lcd: checking 5 captured frames ($1 module)"
	studyboard-v4l2-test /dev/video0 5
}

restore_installed_module()
{
	echo 'start-face-lcd: restoring installed capture module' >&2
	rmmod studyboard_frame_capture 2>/dev/null || true
	modprobe studyboard_frame_capture || true
}

[ "$(id -u)" -eq 0 ] || fail 'run as root on the development board'
case "$FRAMES" in
	''|*[!0-9]*) fail 'usage: sh ./start-face-lcd.sh [frame-count; 0 = continuous]' ;;
esac
[ -f "$BUNDLE/studyboard-frame-capture.ko" ] || fail 'LCD module is missing'
[ -f "$BUNDLE/KERNEL-RELEASE" ] || fail 'KERNEL-RELEASE is missing'
[ -x "$BUNDLE/ov5640-init" ] || fail 'retry-capable bundle-local ov5640-init is missing'
[ -f "$BUNDLE/studyboard-ncnn-face-lcd" ] || fail 'face application is missing'
[ -f "$BUNDLE/run-face-lcd.sh" ] || fail 'run-face-lcd.sh is missing'
[ -f "$BUNDLE/face-model/model.ncnn.param" ] || fail 'model parameter file is missing'
[ -f "$BUNDLE/face-model/model.ncnn.bin" ] || fail 'model weights are missing'
command -v studyboard-v4l2-test >/dev/null 2>&1 || fail 'studyboard-v4l2-test is missing'
[ "$(cat "$BUNDLE/KERNEL-RELEASE")" = "$(uname -r)" ] ||
	fail 'LCD module and running kernel have different releases; no module was changed'

if [ ! -d "$P" ]; then
	modprobe studyboard_frame_capture || fail 'cannot load installed capture module'
fi
[ -r "$P/running" ] || fail 'capture sysfs device is missing'
[ "$(cat "$P/running")" = 0 ] || fail 'capture is already running; stop it first'
[ "$(cat "$P/display_running")" = 0 ] || fail 'display is already running; stop it first'

BOOT_ID=$(cat /proc/sys/kernel/random/boot_id) || fail 'cannot read Linux boot ID'
INITIALISED_BOOT=$(cat "$STATE" 2>/dev/null || true)
if [ "$INITIALISED_BOOT" != "$BOOT_ID" ]; then
	# Reprogramming the sensor after the LCD module is active has sometimes
	# left this board waiting indefinitely for frame zero.
	[ ! -e "$P/lcd_frame" ] ||
		fail 'sensor state is unknown with LCD module loaded; cleanly reboot, then rerun'
	[ "$(cat "$ATTEMPT_STATE" 2>/dev/null || true)" != "$BOOT_ID" ] ||
		fail 'sensor initialization was already attempted this boot; cleanly reboot before retrying'
	printf '%s\n' "$BOOT_ID" > "$ATTEMPT_STATE" || fail 'cannot save init attempt marker'
	echo 'start-face-lcd: initializing OV5640 once for this boot'
	"$BUNDLE/ov5640-init" || fail 'sensor initialization failed; module was not changed'
	if ! test_capture installed; then
		fail 'installed module received no frames; LCD module was not loaded. Check camera/PL, then cleanly reboot before retrying'
	fi
	printf '%s\n' "$BOOT_ID" > "$STATE" || fail 'cannot save sensor boot marker'
elif [ ! -e "$P/lcd_frame" ]; then
	if ! test_capture installed; then
		fail 'installed module received no frames; LCD module was not loaded'
	fi
fi

if [ ! -e "$P/lcd_frame" ]; then
	echo 'start-face-lcd: loading LCD-enabled module'
	rmmod studyboard_frame_capture || fail 'cannot unload installed module; close other camera users'
	if ! insmod "$BUNDLE/studyboard-frame-capture.ko"; then
		modprobe studyboard_frame_capture || true
		fail 'LCD module load failed; attempted to restore installed module'
	fi
	if [ ! -w "$P/lcd_frame" ] || [ ! -w "$P/lcd_commit" ]; then
		restore_installed_module
		fail 'LCD module lacks the required sysfs interfaces'
	fi
fi

if ! test_capture LCD; then
	restore_installed_module
	fail 'LCD module received no frames; installed module was restored'
fi

echo "start-face-lcd: capture and LCD module ready; starting detection for $FRAMES frames"
exec sh "$BUNDLE/run-face-lcd.sh" "$FRAMES"

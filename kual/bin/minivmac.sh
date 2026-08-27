#!/bin/sh
# Mini vMac launcher for Kindle (KUAL scriptlet).
#
# Locates its own extension directory (KUAL runs actions from the
# extension dir), sets DISPLAY, finds the pt_mt touch node, then boots
# mini vMac with the System 6.0.8 disks and docks wario-companion below.
# Safe to run over SSH too.

SELF_DIR="$(cd "$(dirname "$0")/.." && pwd)"

export DISPLAY=:0

# Detect the touchscreen (pt_mt) evdev node; fall back to perfmgr, then
# to /dev/input/event1.
if [ -z "${WARIO_TOUCH_DEV:-}" ] && [ -r /proc/bus/input/devices ]; then
	WARIO_TOUCH_DEV=$(awk 'BEGIN{RS=""; FS="\n"} /N: Name="pt_mt"/ {
		for (i=1;i<=NF;i++) if ($i ~ /^H: Handlers=/) {
			if (match($i, /event[0-9]+/)) print "/dev/input/" substr($i, RSTART, RLENGTH)
		}
	}' /proc/bus/input/devices | head -n 1)
	[ -z "$WARIO_TOUCH_DEV" ] && WARIO_TOUCH_DEV=$(awk 'BEGIN{RS=""; FS="\n"} /perfmgr/ {
		for (i=1;i<=NF;i++) if ($i ~ /^H: Handlers=/) {
			if (match($i, /event[0-9]+/)) print "/dev/input/" substr($i, RSTART, RLENGTH)
		}
	}' /proc/bus/input/devices | head -n 1)
	export WARIO_TOUCH_DEV
fi
export WARIO_TOUCH_DEV=${WARIO_TOUCH_DEV:-/dev/input/event1}

# Stop any existing instance
pkill -9 minivmac 2>/dev/null
pkill -9 wario-companion 2>/dev/null
sleep 1

# Keep the screen awake for the session
lipc-set-prop -i com.lab126.powerd preventScreenSaver 1 2>/dev/null

# Restore the screen-saver state no matter how the session ends.
trap 'lipc-set-prop -i com.lab126.powerd preventScreenSaver 0 2>/dev/null' INT TERM HUP EXIT

# Build the disk list: startup disk first, then up to 2 selected extras
# (System 6.0.8 mounts at most 3 disks). Selection: positional args >
# WARIO_DISKS env > default (macpaint + MacWrite).
DEFAULT_EXTRAS="macpaint.dsk MacWrite.dsk"
if [ "$#" -gt 0 ]; then
	SELECTED="$@"
elif [ -n "${WARIO_DISKS:-}" ]; then
	SELECTED="$WARIO_DISKS"
else
	SELECTED="$DEFAULT_EXTRAS"
fi

set -- "$SELF_DIR/MacOS_6.0.8_System_Startup.img"
for d in $SELECTED; do
	[ "$#" -ge 3 ] && break
	case "$d" in
		/*) p="$d" ;;
		*)  p="$SELF_DIR/$d" ;;
	esac
	if [ -f "$p" ]; then
		set -- "$@" "$p"
	fi
done

# Boot the Mac
cd "$SELF_DIR"
"$SELF_DIR/minivmac" "$@" > /tmp/minivmac.log 2>&1 &
sleep 4

"$SELF_DIR/wario-companion" > /tmp/wario-companion.log 2>&1 &
COMPANION_PID=$!

# Stay alive until the session ends (companion exits), then the EXIT
# trap restores the screen-saver state.
wait "$COMPANION_PID" || true
exit 0

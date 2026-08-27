#!/bin/sh
# launch-wario.sh -- standard startup for the wario mini vMac setup.
#
# Boots mini vMac with the System 6.0.8 startup disk plus up to two
# selectable extra disks, and launches wario-companion docked below it.
# System 6.0.8 only mounts three disks at once, so that's the cap.
#
# Disk selection (in priority order):
#   1. Positional args:  ./launch-wario.sh macpaint.dsk "Excel/disk01.img"
#   2. WARIO_DISKS env:  WARIO_DISKS="macpaint.dsk MacWrite.dsk" ./launch-wario.sh
#   3. Default:          macpaint.dsk MacWrite.dsk
# Each selection mounts the startup disk first, then the extra disk(s)
# (paths are resolved relative to this script's directory).
#
# Deploy: scp to the Kindle's minivmac extension dir, chmod +x, run over
# SSH with DISPLAY=:0. On the PW5 the touch node is auto-detected from
# /proc/bus/input/devices; override with WARIO_TOUCH_DEV=/dev/input/eventN.

set -eu

BIN_DIR="$(cd "$(dirname "$0")" && pwd)"
MINIVMAC="$BIN_DIR/minivmac"
COMPANION="$BIN_DIR/wario-companion"

# ---- Known disks (all optional except the startup disk) ----
SYSTEM_DISK="$BIN_DIR/MacOS_6.0.8_System_Startup.img"
ADDITIONS_DISK="$BIN_DIR/MacOS_6.0.8_System_Additions.img"
MACPAINT_DISK="$BIN_DIR/macpaint.dsk"
MACWRITE_DISK="$BIN_DIR/MacWrite.dsk"
MACWRITEPRO_DISK1="$BIN_DIR/MacWritePro/disk01.img"
MACWRITEPRO_DISK2="$BIN_DIR/MacWritePro/disk02.img"
MACWRITEPRO_DISK3="$BIN_DIR/MacWritePro/disk03.img"
ARTFULTYPE_DISK="$BIN_DIR/ArtfulType-800K.dsk"
# Excel install set: $BIN_DIR/Excel/disk01.img .. disk13.img

# Default extras when no args and no WARIO_DISKS:
DEFAULT_EXTRAS="macpaint.dsk MacWrite.dsk"

export DISPLAY=:0

# mini vMac looks up vMac.ROM relative to its working directory, so run
# from this directory (disk paths below are already absolute).
cd "$BIN_DIR"

# Auto-detect the touchscreen evdev node (Kindle touch driver reports
# "pt_mt" with a "perfmgr" handler in /proc/bus/input/devices).
if [ -z "${WARIO_TOUCH_DEV:-}" ] && [ -r /proc/bus/input/devices ]; then
	detect() {
		awk 'BEGIN{RS=""; FS="\n"}
		     '"$1"' {
			for (i=1;i<=NF;i++) if ($i ~ /^H: Handlers=/) {
				if (match($i, /event[0-9]+/))
					print "/dev/input/" substr($i, RSTART, RLENGTH)
			}
		     }' /proc/bus/input/devices | head -n 1
	}
	WARIO_TOUCH_DEV=$(detect '/N: Name="pt_mt"/')
	[ -z "$WARIO_TOUCH_DEV" ] && WARIO_TOUCH_DEV=$(detect '/perfmgr/')
	export WARIO_TOUCH_DEV
fi
export WARIO_TOUCH_DEV=${WARIO_TOUCH_DEV:-/dev/input/event1}

lipc-set-prop -i com.lab126.powerd preventScreenSaver 1

# Restore the screen-saver state no matter how the session ends (close
# button, stop.sh, crash, Ctrl+C) -- otherwise the device stays awake
# forever. The launcher stays alive waiting for the companion, so the
# EXIT trap fires only when the session actually ends.
trap 'lipc-set-prop -i com.lab126.powerd preventScreenSaver 0 2>/dev/null' INT TERM HUP EXIT

pkill -9 minivmac 2>/dev/null || true
pkill -9 wario-companion 2>/dev/null || true
sleep 1

# ---- Build the disk list: startup disk first, then up to 2 extras ----
if [ "$#" -gt 0 ]; then
	SELECTED="$@"
elif [ -n "${WARIO_DISKS:-}" ]; then
	SELECTED="$WARIO_DISKS"
else
	SELECTED="$DEFAULT_EXTRAS"
fi

set -- "$SYSTEM_DISK"
for d in $SELECTED; do
	[ "$#" -ge 3 ] && break          # System 6 mounts at most 3 disks
	case "$d" in
		/*) p="$d" ;;
		*)  p="$BIN_DIR/$d" ;;
	esac
	if [ -f "$p" ]; then
		set -- "$@" "$p"
	fi
done

"$MINIVMAC" "$@" > /tmp/minivmac.log 2>&1 &
sleep 4

"$COMPANION" > /tmp/wario-companion.log 2>&1 &
COMPANION_PID=$!

# Stay alive until the session ends (companion exits), then the EXIT
# trap restores the screen-saver state.
wait "$COMPANION_PID" || true
exit 0

#!/bin/sh
# launch-wario.sh -- standard startup for the wario mini vMac setup.
#
# Boots mini vMac with the System 6.0.8 startup disk plus MacPaint and
# MacWrite floppies attached by default (Ryan's request, 2026-08-22),
# and launches wario-companion docked below it. Run this instead of
# invoking minivmac/wario-companion by hand so the disk set stays
# consistent across sessions.
#
# Deploy: scp to kindle-ts:/mnt/us/launch-wario.sh, chmod +x, run over
# SSH with DISPLAY=:0 (no chroot, no KUAL wiring yet -- see README's
# "Operational polish" backlog for that). On the PW5 the touch node is
# auto-detected from /proc/bus/input/devices; override with
# WARIO_TOUCH_DEV=/dev/input/eventN if detection fails.

set -eu

BIN_DIR=/mnt/us
MINIVMAC="$BIN_DIR/minivmac"
COMPANION="$BIN_DIR/wario-companion"
SYSTEM_DISK="$BIN_DIR/system608.img"
MACPAINT_DISK="$BIN_DIR/macpaint.img"
MACWRITE_DISK="$BIN_DIR/macwrite.dsk"

export DISPLAY=:0

# mini vMac looks up vMac.ROM relative to its working directory, so run
# from the deploy dir (the floppy paths below are already absolute).
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

pkill -9 minivmac 2>/dev/null || true
pkill -9 wario-companion 2>/dev/null || true
sleep 1

"$MINIVMAC" "$SYSTEM_DISK" "$MACPAINT_DISK" "$MACWRITE_DISK" \
	> /tmp/minivmac.log 2>&1 &
sleep 4

"$COMPANION" > /tmp/wario-companion.log 2>&1 &

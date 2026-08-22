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
# "Operational polish" backlog for that).

set -eu

BIN_DIR=/mnt/us
MINIVMAC="$BIN_DIR/minivmac"
COMPANION="$BIN_DIR/wario-companion"
SYSTEM_DISK="$BIN_DIR/system608.img"
MACPAINT_DISK="$BIN_DIR/macpaint.img"
MACWRITE_DISK="$BIN_DIR/macwrite.dsk"

export DISPLAY=:0

lipc-set-prop -i com.lab126.powerd preventScreenSaver 1

pkill -9 minivmac 2>/dev/null || true
pkill -9 wario-companion 2>/dev/null || true
sleep 1

"$MINIVMAC" "$SYSTEM_DISK" "$MACPAINT_DISK" "$MACWRITE_DISK" \
	> /tmp/minivmac.log 2>&1 &
sleep 4

"$COMPANION" > /tmp/wario-companion.log 2>&1 &

#!/bin/sh
# Stop the Mini vMac session (kills minivmac + wario-companion) and
# restore the screen-saver state the launcher disabled.
lipc-set-prop -i com.lab126.powerd preventScreenSaver 0 2>/dev/null
pkill -9 minivmac 2>/dev/null
pkill -9 wario-companion 2>/dev/null
exit 0

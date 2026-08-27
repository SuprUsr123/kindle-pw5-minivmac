#!/bin/sh
# Stop the Mini vMac session (kills minivmac + wario-companion).
pkill -9 minivmac 2>/dev/null
pkill -9 wario-companion 2>/dev/null
exit 0

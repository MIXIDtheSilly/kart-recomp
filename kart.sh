#!/bin/sh
# Linux: set up, build and play (tools/linux.py; ./kart.sh with no arguments lists the steps).
cd "$(dirname "$0")" && exec python3 tools/linux.py "$@"

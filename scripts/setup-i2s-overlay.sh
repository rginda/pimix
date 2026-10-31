#!/usr/bin/env bash
set -euo pipefail

DTS_FILE="$HOME/src/pimix/pi5/overlays/pimix-rp1-i2s-8ch.dts"
DTBO_FILE="/tmp/pimix-rp1-i2s-8ch.dtbo"

echo "=== [1/3] Compiling pimix-rp1-i2s-8ch.dts ==="
dtc -@ -I dts -O dtb -o "$DTBO_FILE" "$DTS_FILE"
sudo cp "$DTBO_FILE" /boot/firmware/overlays/pimix-rp1-i2s-8ch.dtbo

echo "=== [2/3] Loading pimix-rp1-i2s-8ch overlay ==="
if dtoverlay -l | grep -q "pimix-rp1-i2s-8ch"; then
    echo "Overlay pimix-rp1-i2s-8ch is already loaded."
else
    sudo dtoverlay pimix-rp1-i2s-8ch
fi
sleep 1

echo "=== [3/3] Verifying ALSA Capture Devices (arecord -l) ==="
arecord -l

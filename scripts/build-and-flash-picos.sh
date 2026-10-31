#!/usr/bin/env bash
set -euo pipefail

FW_DIR="$HOME/src/pimix/firmware/pico-uac2-i2s"
BUILD_DIR="$FW_DIR/build"

echo "=== [1/4] Updating udev rules for PiMix UAC2 (0xcafe) & RP2040 (0x2e8a) ==="
sudo tee /etc/udev/rules.d/99-rp2040.rules >/dev/null <<'EOF'
SUBSYSTEM=="usb", ATTR{idVendor}=="2e8a", MODE="0666", GROUP="plugdev"
SUBSYSTEM=="usb", ATTR{idVendor}=="cafe", MODE="0666", GROUP="plugdev"
EOF
sudo udevadm control --reload-rules
sudo udevadm trigger

echo "=== [2/4] Building pimix_uac2_i2s firmware ==="
mkdir -p "$BUILD_DIR"
cmake -B "$BUILD_DIR" -S "$FW_DIR" \
    -GNinja \
    -DPICO_SDK_PATH="$HOME/src/pico-sdk" \
    -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD_DIR"

UF2_FILE="$BUILD_DIR/pimix_uac2_i2s.uf2"
ls -lh "$UF2_FILE"

echo "=== [3/4] Flashing all connected RP2040 devices in BOOTSEL mode ==="
# Find all bus/address pairs for 2e8a:0003
mapfile -t BOOT_DEVS < <(lsusb -d 2e8a:0003 | awk '{gsub(":", "", $4); print int($2) " " int($4)}')

if [ "${#BOOT_DEVS[@]}" -eq 0 ]; then
    echo "No devices in BOOTSEL (2e8a:0003) found. Rebooting running PiMix (cafe:4010) devices into BOOTSEL..."
    "$HOME/src/pimix/scripts/pimix-usb-ctl.py" bootsel
    sleep 2
    mapfile -t BOOT_DEVS < <(lsusb -d 2e8a:0003 | awk '{gsub(":", "", $4); print int($2) " " int($4)}')
fi

for dev in "${BOOT_DEVS[@]}"; do
    read -r bus addr <<<"$dev"
    echo "Flashing RP2040 at Bus $bus, Address $addr..."
    picotool load -x "$UF2_FILE" --bus "$bus" --address "$addr"
done

echo "=== [4/4] Waiting for USB Audio enumeration ==="
sleep 4
lsusb
echo "=== ALSA Playback Devices (aplay -l) ==="
aplay -l

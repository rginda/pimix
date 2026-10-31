#!/usr/bin/env bash
set -euo pipefail

echo "=== [1/5] Creating ~/src/pimix directory structure ==="
mkdir -p "$HOME/src/pimix"

echo "=== [2/5] Installing ARM toolchain and build dependencies ==="
sudo apt-get update
sudo apt-get install -y \
    gcc-arm-none-eabi \
    libnewlib-arm-none-eabi \
    libstdc++-arm-none-eabi-newlib \
    build-essential \
    cmake \
    ninja-build \
    pkg-config \
    libusb-1.0-0-dev \
    device-tree-compiler \
    alsa-utils \
    git \
    curl \
    jq \
    python3-pip \
    python3-venv

if apt-cache show picotool >/dev/null 2>&1; then
    echo "Installing picotool from apt..."
    sudo apt-get install -y picotool
fi

echo "=== [3/5] Setting up pico-sdk in ~/src/pico-sdk ==="
if [ ! -d "$HOME/src/pico-sdk" ]; then
    git clone --depth 1 --branch 2.1.1 https://github.com/raspberrypi/pico-sdk.git "$HOME/src/pico-sdk"
    git -C "$HOME/src/pico-sdk" submodule update --init --depth 1
else
    echo "pico-sdk already exists at $HOME/src/pico-sdk"
fi

if ! command -v picotool >/dev/null 2>&1; then
    echo "Building picotool from source..."
    if [ ! -d "$HOME/src/picotool" ]; then
        git clone --depth 1 --branch 2.1.1 https://github.com/raspberrypi/picotool.git "$HOME/src/picotool"
    fi
    cmake -B "$HOME/src/picotool/build" -S "$HOME/src/picotool" \
        -GNinja \
        -DPICO_SDK_PATH="$HOME/src/pico-sdk"
    cmake --build "$HOME/src/picotool/build"
    sudo cmake --install "$HOME/src/picotool/build"
fi

echo "=== [4/5] Configuring udev rules for RP2040 BOOTSEL & USB devices ==="
sudo tee /etc/udev/rules.d/99-rp2040.rules >/dev/null <<'EOF'
SUBSYSTEM=="usb", ATTR{idVendor}=="2e8a", MODE="0666", GROUP="plugdev"
EOF
sudo udevadm control --reload-rules
sudo udevadm trigger

echo "=== [5/5] Installing CamillaDSP (aarch64) ==="
if ! command -v camilladsp >/dev/null 2>&1; then
    CAMILLA_VERSION="v3.0.0"
    CAMILLA_URL="https://github.com/HEnquist/camilladsp/releases/download/${CAMILLA_VERSION}/camilladsp-linux-aarch64.tar.gz"
    echo "Downloading CamillaDSP ${CAMILLA_VERSION}..."
    curl -fL "$CAMILLA_URL" -o /tmp/camilladsp.tar.gz
    sudo tar -xzf /tmp/camilladsp.tar.gz -C /usr/local/bin camilladsp
    sudo chmod +x /usr/local/bin/camilladsp
    rm -f /tmp/camilladsp.tar.gz
else
    echo "camilladsp already installed: $(command -v camilladsp)"
fi

echo "=== Verification ==="
echo "arm-none-eabi-gcc: $(arm-none-eabi-gcc --version | head -n 1)"
echo "picotool:          $(picotool version || true)"
echo "camilladsp:        $(camilladsp --version || true)"
echo "PICO_SDK_PATH:     $HOME/src/pico-sdk"
echo "=== Connected RP2040 devices ==="
picotool info -a || true

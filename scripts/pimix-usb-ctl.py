#!/usr/bin/env python3
"""
Send vendor control requests to running PiMix RP2040 UAC2 sound cards (VID 0xcafe, PID 0x4010)
using standard Linux USBDEVFS_CONTROL ioctl (no external dependencies required).

Usage:
  ./pimix-usb-ctl.py bootsel          # Reboot all connected PiMix USB sound cards into BOOTSEL
  ./pimix-usb-ctl.py tone 440         # Enable 440 Hz digital test tone on I2S output
  ./pimix-usb-ctl.py tone 0           # Disable test tone (normal USB audio)
"""
import ctypes
import fcntl
import glob
import os
import struct
import sys

USBDEVFS_CONTROL = 0xC0185500  # _IOWR('U', 0, struct usbdevfs_ctrltransfer)
REQ_BOOTSEL = 0xBB
REQ_TEST_TONE = 0xBC
REQ_DIAG = 0xBD
REQ_FORCE_SDATA = 0xBE


def find_pimix_usb_paths():
    paths = []
    for dev_dir in glob.glob("/sys/bus/usb/devices/*"):
        vid_file = os.path.join(dev_dir, "idVendor")
        pid_file = os.path.join(dev_dir, "idProduct")
        if not (os.path.exists(vid_file) and os.path.exists(pid_file)):
            continue
        with open(vid_file) as f:
            vid = f.read().strip().lower()
        with open(pid_file) as f:
            pid = f.read().strip().lower()
        if vid == "cafe" and pid == "4010":
            with open(os.path.join(dev_dir, "busnum")) as f:
                bus = int(f.read().strip())
            with open(os.path.join(dev_dir, "devnum")) as f:
                dev = int(f.read().strip())
            prod_file = os.path.join(dev_dir, "product")
            product = open(prod_file).read().strip() if os.path.exists(prod_file) else "PiMix USB"
            paths.append((f"/dev/bus/usb/{bus:03d}/{dev:03d}", product))
    return sorted(paths)


def send_vendor_req(dev_path, bRequest, wValue, wIndex=0):
    # struct usbdevfs_ctrltransfer { uint8 bRequestType, bRequest; uint16 wValue, wIndex, wLength; uint32 timeout; void *data; }
    ctrl = struct.pack("BBHHHIIP", 0x40, bRequest, wValue, wIndex, 0, 1000, 0, 0)
    fd = os.open(dev_path, os.O_RDWR)
    try:
        fcntl.ioctl(fd, USBDEVFS_CONTROL, ctrl)
    except OSError:
        # Device resetting into BOOTSEL disconnects immediately during the control transfer
        pass
    finally:
        os.close(fd)


def read_diag(dev_path):
    buf = ctypes.create_string_buffer(32)
    ctrl = struct.pack("BBHHHIIP", 0xC0, REQ_DIAG, 0, 0, 32, 1000, 0, ctypes.addressof(buf))
    fd = os.open(dev_path, os.O_RDWR)
    try:
        fcntl.ioctl(fd, USBDEVFS_CONTROL, ctrl)
    finally:
        os.close(fd)
    return struct.unpack("<8I", buf.raw)


def format_pins(mask):
    return [p for p in range(30) if (mask & (1 << p))]


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)

    cmd = sys.argv[1]
    devs = find_pimix_usb_paths()
    if not devs:
        print("No running PiMix USB devices (cafe:4010) found.")
        return

    for path, name in devs:
        if cmd == "bootsel":
            print(f"Rebooting {name} ({path}) into BOOTSEL mode...")
            send_vendor_req(path, REQ_BOOTSEL, 0xB007)
        elif cmd == "tone":
            freq = int(sys.argv[2]) if len(sys.argv) > 2 else 440
            print(f"Setting {name} ({path}) test tone frequency to {freq} Hz...")
            send_vendor_req(path, REQ_TEST_TONE, freq)
        elif cmd == "sdata":
            mode = int(sys.argv[2]) if len(sys.argv) > 2 else 0
            target = sys.argv[3] if len(sys.argv) > 3 else ""
            if target and target not in name:
                continue
            print(f"Setting {name} ({path}) GPIO 9 SDATA mode to {mode} (0=PIO, 1=LOW, 2=HIGH)...")
            send_vendor_req(path, REQ_FORCE_SDATA, mode)
        elif cmd == "diag":
            g_now, g_or, g_and, dma_cnt, dma_a, dma_b, pio_st, buf_a0 = read_diag(path)
            toggling = g_or ^ g_and
            print(f"=== {name} ({path}) ===")
            print(f"  Toggling GPIOs : {format_pins(toggling)} (mask=0x{toggling:08x})")
            print(f"  High GPIOs     : {format_pins(g_and)} (now=0x{g_now:08x})")
            print(f"  GPIO 13/14/15  : GP13/SDATA={(g_now>>13)&1} (tog={(toggling>>13)&1}), GP14/BCLK={(g_now>>14)&1} (tog={(toggling>>14)&1}), GP15/LRCLK={(g_now>>15)&1} (tog={(toggling>>15)&1})")
            print(f"  DMA Completions: {dma_cnt} (ch_a_rem={dma_a}, ch_b_rem={dma_b}, buf_a[0]=0x{buf_a0:08x})")
            print(f"  PIO0 SM0 State : PC={pio_st >> 16}, FSTAT=0x{pio_st & 0xFFFF:04x}")
        else:
            print(f"Unknown command: {cmd}")
            sys.exit(1)


if __name__ == "__main__":
    main()

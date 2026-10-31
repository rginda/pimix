#!/usr/bin/env python3
"""
End-to-end hardware loopback verification for PiMix:
  Stage 1: Pico internal digital test tone -> I2S (SDI0 & SDI1) -> Pi 5 RP1 ALSA capture
  Stage 2: Pi 5 ALSA USB Playback (hw:P1 & hw:P2) -> Pico UAC2 -> I2S -> Pi 5 RP1 ALSA capture
"""
import fcntl
import glob
import math
import os
import struct
import subprocess
import sys
import time

USBDEVFS_CONTROL = 0xC0185500
REQ_TEST_TONE = 0xBC
SAMPLE_RATE = 48000


def find_pimix_usb_devices():
    devs = {}
    for dev_dir in glob.glob("/sys/bus/usb/devices/*"):
        vid_file = os.path.join(dev_dir, "idVendor")
        pid_file = os.path.join(dev_dir, "idProduct")
        if not (os.path.exists(vid_file) and os.path.exists(pid_file)):
            continue
        vid = open(vid_file).read().strip().lower()
        pid = open(pid_file).read().strip().lower()
        if vid == "cafe" and pid == "4010":
            bus = int(open(os.path.join(dev_dir, "busnum")).read().strip())
            dev = int(open(os.path.join(dev_dir, "devnum")).read().strip())
            prod_file = os.path.join(dev_dir, "product")
            product = open(prod_file).read().strip() if os.path.exists(prod_file) else "PiMix USB"
            devs[product] = f"/dev/bus/usb/{bus:03d}/{dev:03d}"
    return devs


def set_pico_test_tone(dev_path, freq_hz):
    ctrl = struct.pack("BBHHHIIP", 0x40, REQ_TEST_TONE, freq_hz, 0, 0, 1000, 0, 0)
    fd = os.open(dev_path, os.O_RDWR)
    try:
        fcntl.ioctl(fd, USBDEVFS_CONTROL, ctrl)
    finally:
        os.close(fd)


def generate_s24_3le_wav(filename, duration_sec, freq_l, freq_r, amp=0.5):
    num_frames = int(SAMPLE_RATE * duration_sec)
    data_bytes = bytearray(num_frames * 6)
    max_val = int((2**23 - 1) * amp)

    for i in range(num_frames):
        t = i / SAMPLE_RATE
        val_l = int(max_val * math.sin(2.0 * math.pi * freq_l * t))
        val_r = int(max_val * math.sin(2.0 * math.pi * freq_r * t))
        if val_l < 0:
            val_l += 1 << 24
        if val_r < 0:
            val_r += 1 << 24
        idx = i * 6
        data_bytes[idx + 0] = val_l & 0xFF
        data_bytes[idx + 1] = (val_l >> 8) & 0xFF
        data_bytes[idx + 2] = (val_l >> 16) & 0xFF
        data_bytes[idx + 3] = val_r & 0xFF
        data_bytes[idx + 4] = (val_r >> 8) & 0xFF
        data_bytes[idx + 5] = (val_r >> 16) & 0xFF

    data_size = len(data_bytes)
    with open(filename, "wb") as f:
        f.write(b"RIFF")
        f.write(struct.pack("<I", 36 + data_size))
        f.write(b"WAVEfmt ")
        f.write(struct.pack("<IHHIIHH", 16, 1, 2, SAMPLE_RATE, SAMPLE_RATE * 6, 6, 24))
        f.write(b"data")
        f.write(struct.pack("<I", data_size))
        f.write(data_bytes)


def analyze_raw_s32le(filename, num_channels=4, skip_sec=0.25):
    raw = open(filename, "rb").read()
    frame_bytes = num_channels * 4
    total_frames = len(raw) // frame_bytes
    start_frame = min(int(skip_sec * SAMPLE_RATE), total_frames // 2)
    analyzed_frames = total_frames - start_frame

    if analyzed_frames <= 0:
        print("Error: No frames captured!")
        return

    channels = [[] for _ in range(num_channels)]
    for i in range(start_frame, total_frames):
        offset = i * frame_bytes
        vals = struct.unpack_from(f"<{num_channels}i", raw, offset)
        for ch in range(num_channels):
            channels[ch].append(vals[ch])

    full_scale = float(2**31 - 1)
    lane_names = [
        "Ch 0 (SDI0 / GPIO 20 Left )",
        "Ch 1 (SDI0 / GPIO 20 Right)",
        "Ch 2 (SDI1 / GPIO 22 Left )",
        "Ch 3 (SDI1 / GPIO 22 Right)",
    ]

    for ch in range(num_channels):
        samples = channels[ch]
        peak = max(abs(x) for x in samples)
        sum_sq = sum(float(x) * float(x) for x in samples)
        rms = math.sqrt(sum_sq / len(samples))
        rms_dbfs = 20.0 * math.log10(rms / full_scale) if rms > 0 else -120.0
        peak_dbfs = 20.0 * math.log10(peak / full_scale) if peak > 0 else -120.0

        # Estimate frequency from positive-going zero crossings (with hysteresis)
        crossings = []
        threshold = max(peak * 0.05, 256)
        armed = False
        for idx, s in enumerate(samples):
            if s < -threshold:
                armed = True
            elif armed and s > threshold:
                crossings.append(idx)
                armed = False

        if len(crossings) >= 2:
            avg_period = (crossings[-1] - crossings[0]) / (len(crossings) - 1)
            freq_hz = SAMPLE_RATE / avg_period
        else:
            freq_hz = 0.0

        first_hex = " ".join(f"{(x & 0xFFFFFFFF):08x}" for x in samples[:4])
        label = lane_names[ch] if ch < len(lane_names) else f"Ch {ch}"
        print(
            f"  {label}: RMS={rms_dbfs:6.1f} dBFS | Peak={peak_dbfs:6.1f} dBFS | "
            f"Freq={freq_hz:7.1f} Hz | Samples[0..3]=[{first_hex}]"
        )


def main():
    devs = find_pimix_usb_devices()
    print("Found PiMix USB devices:", devs)
    if "PiMix USB 1" not in devs or "PiMix USB 2" not in devs:
        print("ERROR: Expected both 'PiMix USB 1' and 'PiMix USB 2' on USB bus!")
        sys.exit(1)

    print("\n=== STAGE 1: Direct Pico -> Pi 5 I2S Hardware Link Test (Internal Test Tone) ===")
    print("Setting PiMix USB 1 -> 440 Hz tone, PiMix USB 2 -> 1000 Hz tone...")
    set_pico_test_tone(devs["PiMix USB 1"], 440)
    set_pico_test_tone(devs["PiMix USB 2"], 1000)

    try:
        stage1_raw = "/tmp/pimix_stage1.raw"
        subprocess.run(
            [
                "arecord",
                "-D", "hw:CARD=PiMixRP18ch,DEV=0",
                "-c", "4",
                "-r", str(SAMPLE_RATE),
                "-f", "S32_LE",
                "-t", "raw",
                "-d", "1",
                stage1_raw,
            ],
            check=True,
        )
        print("Stage 1 Captured 4-channel I2S analysis:")
        analyze_raw_s32le(stage1_raw, num_channels=4, skip_sec=0.1)
    finally:
        set_pico_test_tone(devs["PiMix USB 1"], 0)
        set_pico_test_tone(devs["PiMix USB 2"], 0)

    print("\n=== STAGE 2: Full USB Audio Playback -> Pico UAC2 -> I2S -> Pi 5 Capture Loopback ===")
    wav1 = "/tmp/pimix_usb1_test.wav"
    wav2 = "/tmp/pimix_usb2_test.wav"
    print("Generating 24-bit test WAV files:")
    print("  PiMix USB 1 (hw:P1): Left = 375 Hz, Right =  750 Hz (-6 dBFS)")
    print("  PiMix USB 2 (hw:P2): Left = 600 Hz, Right = 1200 Hz (-6 dBFS)")
    generate_s24_3le_wav(wav1, duration_sec=3.0, freq_l=375.0, freq_r=750.0, amp=0.5)
    generate_s24_3le_wav(wav2, duration_sec=3.0, freq_l=600.0, freq_r=1200.0, amp=0.5)

    stage2_raw = "/tmp/pimix_stage2.raw"
    # Start recording first so Pi 5 BCLK/LRCLK is running before USB playback begins
    rec_proc = subprocess.Popen(
        [
            "arecord",
            "-D", "hw:CARD=PiMixRP18ch,DEV=0",
            "-c", "4",
            "-r", str(SAMPLE_RATE),
            "-f", "S32_LE",
            "-t", "raw",
            "-d", "2",
            stage2_raw,
        ]
    )
    time.sleep(0.2)
    p1_proc = subprocess.Popen(["aplay", "-D", "hw:CARD=P1,DEV=0", wav1])
    p2_proc = subprocess.Popen(["aplay", "-D", "hw:CARD=P2,DEV=0", wav2])

    rec_proc.wait()
    p1_proc.terminate()
    p2_proc.terminate()
    p1_proc.wait()
    p2_proc.wait()

    print("Stage 2 Captured 4-channel I2S analysis:")
    analyze_raw_s32le(stage2_raw, num_channels=4, skip_sec=0.5)


if __name__ == "__main__":
    main()

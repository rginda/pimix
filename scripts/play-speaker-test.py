#!/usr/bin/env python3
"""
PiMix Live Speaker Verification Script
1. Ensures CamillaDSP is running to mix the 4-channel RP1 I2S capture
   (hw:PiMixRP18ch,0: Ch 0-1 = PiMix USB 1, Ch 2-3 = PiMix USB 2)
   to the external USB speaker (hw:Device,0).
2. Synthesizes two 14-second, 24-bit / 48 kHz stereo musical test tracks:
   - [0.0s -  4.0s] PiMix USB 1 Solo: Warm Cmaj9 stereo arpeggio (Left -> Right)
   - [4.0s -  8.0s] PiMix USB 2 Solo: Bell-like melody (Right -> Left)
   - [8.0s - 14.0s] Both Picos Mixed Simultaneously: Warm chords on USB 1 + melody on USB 2
3. Streams both WAV files simultaneously into hw:P1,0 (PiMix USB 1) and hw:P2,0 (PiMix USB 2).
"""
import math
import os
import struct
import subprocess
import time

SAMPLE_RATE = 48000
DURATION_SEC = 14.0
NUM_FRAMES = int(SAMPLE_RATE * DURATION_SEC)


def note_env(t: float, start: float, dur: float) -> float:
    """Smooth attack + exponential decay envelope for a note."""
    dt = t - start
    if dt < 0.0 or dt >= dur:
        return 0.0
    attack = min(1.0, dt / 0.015)
    release = min(1.0, (dur - dt) / 0.04)
    decay = math.exp(-dt * 2.2)
    return attack * release * decay


def pad_env(t: float, start: float, dur: float) -> float:
    """Smooth sustain pad envelope."""
    dt = t - start
    if dt < 0.0 or dt >= dur:
        return 0.0
    attack = min(1.0, dt / 0.15)
    release = min(1.0, (dur - dt) / 0.35)
    return attack * release


def tone_epiano(freq: float, t: float) -> float:
    """Warm Rhodes-like electric piano timbre (fundamental + gentle 2nd/3rd harmonics)."""
    w = 2.0 * math.pi * freq * t
    return 0.75 * math.sin(w) + 0.18 * math.sin(2.0 * w) + 0.07 * math.sin(3.0 * w)


def tone_bell(freq: float, t: float) -> float:
    """Clear bell/chime timbre for Pico 2."""
    w = 2.0 * math.pi * freq * t
    return 0.65 * math.sin(w) + 0.25 * math.sin(2.0 * w) + 0.10 * math.sin(4.0 * w)


def write_s24_3le_wav(filename: str, left_samples: list[float], right_samples: list[float]):
    """Write 24-bit packed (S24_3LE) stereo WAV file."""
    n = len(left_samples)
    data_size = n * 6
    buf = bytearray(n * 6)
    scale = 8388607.0 * 0.45  # ~-7 dBFS peak headroom

    for i in range(n):
        l_val = max(-8388607, min(8388607, int(left_samples[i] * scale)))
        r_val = max(-8388607, min(8388607, int(right_samples[i] * scale)))
        if l_val < 0:
            l_val += 1 << 24
        if r_val < 0:
            r_val += 1 << 24
        idx = i * 6
        buf[idx + 0] = l_val & 0xFF
        buf[idx + 1] = (l_val >> 8) & 0xFF
        buf[idx + 2] = (l_val >> 16) & 0xFF
        buf[idx + 3] = r_val & 0xFF
        buf[idx + 4] = (r_val >> 8) & 0xFF
        buf[idx + 5] = (r_val >> 16) & 0xFF

    with open(filename, "wb") as f:
        f.write(b"RIFF")
        f.write(struct.pack("<I", 36 + data_size))
        f.write(b"WAVEfmt ")
        f.write(struct.pack("<IHHIIHH", 16, 1, 2, SAMPLE_RATE, SAMPLE_RATE * 6, 6, 24))
        f.write(b"data")
        f.write(struct.pack("<I", data_size))
        f.write(buf)


def generate_test_tracks(wav1_path: str, wav2_path: str):
    p1_l = [0.0] * NUM_FRAMES
    p1_r = [0.0] * NUM_FRAMES
    p2_l = [0.0] * NUM_FRAMES
    p2_r = [0.0] * NUM_FRAMES

    # Section 1 (0.0s - 4.0s): PiMix USB 1 Solo Arpeggio (Cmaj9 panning Left -> Right)
    # Notes: (start_sec, duration_sec, freq_hz, pan_0_left_to_1_right)
    sec1_notes = [
        (0.10, 0.9, 261.63, 0.05),  # C4 (Hard Left)
        (0.55, 0.9, 329.63, 0.25),  # E4
        (1.00, 0.9, 392.00, 0.45),  # G4
        (1.45, 0.9, 493.88, 0.65),  # B4
        (1.90, 0.9, 587.33, 0.85),  # D5
        (2.35, 1.5, 783.99, 0.95),  # G5 (Hard Right)
        # Warm stereo chord resolve at 2.8s
        (2.80, 1.1, 261.63, 0.30),
        (2.80, 1.1, 329.63, 0.70),
        (2.80, 1.1, 392.00, 0.40),
        (2.80, 1.1, 493.88, 0.60),
    ]

    # Section 2 (4.0s - 8.0s): PiMix USB 2 Solo Melody (Right -> Left)
    sec2_notes = [
        (4.10, 0.8, 880.00, 0.95),  # A5 (Hard Right)
        (4.55, 0.8, 783.99, 0.75),  # G5
        (5.00, 0.8, 659.25, 0.55),  # E5
        (5.45, 0.8, 587.33, 0.35),  # D5
        (5.90, 0.8, 523.25, 0.15),  # C5
        (6.35, 1.5, 659.25, 0.05),  # E5 (Hard Left)
        # Sparkling stereo triad at 6.8s
        (6.80, 1.1, 440.00, 0.35),
        (6.80, 1.1, 523.25, 0.65),
        (6.80, 1.1, 659.25, 0.50),
    ]

    # Section 3 (8.0s - 14.0s): Both Picos Together!
    # PiMix USB 1 plays warm progression: Fmaj7 -> G6 -> Cmaj9
    sec3_p1_chords = [
        # Fmaj7 (8.0 - 9.8s)
        (8.00, 1.8, 174.61, 0.40),  # F3
        (8.00, 1.8, 261.63, 0.30),  # C4
        (8.00, 1.8, 349.23, 0.70),  # F4
        (8.00, 1.8, 440.00, 0.60),  # A4
        # G6 (9.8 - 11.4s)
        (9.80, 1.6, 196.00, 0.40),  # G3
        (9.80, 1.6, 293.66, 0.65),  # D4
        (9.80, 1.6, 392.00, 0.35),  # G4
        (9.80, 1.6, 493.88, 0.70),  # B4
        # Cmaj9 (11.4 - 13.8s)
        (11.40, 2.4, 130.81, 0.50), # C3
        (11.40, 2.4, 261.63, 0.30), # C4
        (11.40, 2.4, 329.63, 0.70), # E4
        (11.40, 2.4, 392.00, 0.35), # G4
        (11.40, 2.4, 587.33, 0.65), # D5
    ]

    # PiMix USB 2 plays melodic lead over the chords (8.0 - 13.8s)
    sec3_p2_melody = [
        (8.10, 0.6, 523.25, 0.30),  # C5
        (8.55, 0.6, 587.33, 0.70),  # D5
        (9.00, 0.8, 659.25, 0.40),  # E5
        (9.90, 0.6, 783.99, 0.65),  # G5
        (10.35, 0.6, 659.25, 0.35), # E5
        (10.80, 0.6, 587.33, 0.60), # D5
        (11.40, 2.3, 783.99, 0.45), # G5
        (11.65, 2.0, 1046.50, 0.55),# C6
    ]

    for i in range(NUM_FRAMES):
        t = i / SAMPLE_RATE

        if t < 4.05:
            val_l = 0.0
            val_r = 0.0
            for start, dur, freq, pan in sec1_notes:
                env = note_env(t, start, dur)
                if env > 0.0:
                    s = env * tone_epiano(freq, t) * 0.45
                    val_l += s * math.cos(pan * math.pi * 0.5)
                    val_r += s * math.sin(pan * math.pi * 0.5)
            p1_l[i] = val_l
            p1_r[i] = val_r

        elif t < 8.05:
            val_l = 0.0
            val_r = 0.0
            for start, dur, freq, pan in sec2_notes:
                env = note_env(t, start, dur)
                if env > 0.0:
                    s = env * tone_bell(freq, t) * 0.45
                    val_l += s * math.cos(pan * math.pi * 0.5)
                    val_r += s * math.sin(pan * math.pi * 0.5)
            p2_l[i] = val_l
            p2_r[i] = val_r

        else:
            v1_l, v1_r = 0.0, 0.0
            for start, dur, freq, pan in sec3_p1_chords:
                env = pad_env(t, start, dur)
                if env > 0.0:
                    s = env * tone_epiano(freq, t) * 0.25
                    v1_l += s * math.cos(pan * math.pi * 0.5)
                    v1_r += s * math.sin(pan * math.pi * 0.5)
            p1_l[i] = v1_l
            p1_r[i] = v1_r

            v2_l, v2_r = 0.0, 0.0
            for start, dur, freq, pan in sec3_p2_melody:
                env = note_env(t, start, dur)
                if env > 0.0:
                    s = env * tone_bell(freq, t) * 0.42
                    v2_l += s * math.cos(pan * math.pi * 0.5)
                    v2_r += s * math.sin(pan * math.pi * 0.5)
            p2_l[i] = v2_l
            p2_r[i] = v2_r

    write_s24_3le_wav(wav1_path, p1_l, p1_r)
    write_s24_3le_wav(wav2_path, p2_l, p2_r)


def main():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    repo_dir = os.path.dirname(script_dir)
    camilla_cfg = os.path.join(repo_dir, "pi5", "camilladsp", "pimix-4ch-usb-out.yml")

    # 1. Ensure USB Audio Device speaker volume is unmuted and at a comfortable 75% (-11 dB)
    subprocess.run(["amixer", "-c", "Device", "sset", "Speaker", "22", "unmute"], check=False)

    # 2. Stop PipeWire/WirePlumber from locking the raw ALSA devices if active, and start CamillaDSP
    subprocess.run(["pkill", "-f", "camilladsp"], check=False)
    time.sleep(0.3)

    print(f"Starting CamillaDSP live mixer ({camilla_cfg})...")
    log_file = open("/tmp/camilladsp.log", "w")
    camilla_proc = subprocess.Popen(
        ["/usr/local/bin/camilladsp", "-p", "1234", camilla_cfg],
        stdout=log_file,
        stderr=subprocess.STDOUT,
        start_new_session=True,
    )
    time.sleep(1.0)
    if camilla_proc.poll() is not None:
        with open("/tmp/camilladsp.log", "r") as f:
            out = f.read()
        raise RuntimeError(f"CamillaDSP exited early:\n{out}")

    wav1 = "/tmp/pimix_speaker_usb1.wav"
    wav2 = "/tmp/pimix_speaker_usb2.wav"
    print("Synthesizing 14-second stereo musical test tracks for PiMix USB 1 and PiMix USB 2...")
    generate_test_tracks(wav1, wav2)

    print("\n=== PLAYING LIVE AUDIO THROUGH PICOS -> I2S -> PI 5 -> CAMILLADSP -> SPEAKERS ===")
    print("  [ 0.0s -  4.0s] PiMix USB 1 SOLO: Warm electric piano arpeggio (panning Left -> Right)")
    print("  [ 4.0s -  8.0s] PiMix USB 2 SOLO: Bell counter-melody (panning Right -> Left)")
    print("  [ 8.0s - 14.0s] BOTH PICOS MIXED: Chords on PiMix USB 1 + Melody on PiMix USB 2")

    p1 = subprocess.Popen(["aplay", "-D", "hw:P1,0", wav1])
    p2 = subprocess.Popen(["aplay", "-D", "hw:P2,0", wav2])

    p1.wait()
    p2.wait()
    print("=== Playback finished! Leaving CamillaDSP mixer running in the background. ===")


if __name__ == "__main__":
    main()

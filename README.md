# PiMix — 6-Input Desk Audio Mixer & Parametric EQ

**PiMix** is a hardware and software desk audio mixer and parametric equalizer built around a **Raspberry Pi 5** and **4× Raspberry Pi Picos (RP2040/RP2040-W)**:
- **4× Isolated USB Audio Class 2.0 (UAC2) Inputs:** Four Picos present as independent USB sound cards to separate computers, feeding the Pi 5's RP1 4-lane I2S bus (`SDI0`–`SDI3` on `GPIO 20, 22, 24, 26`) across `ISO7741` / `ADuM1401` digital isolators.
- **1× Stereo Line Input & Master Stereo Output:** External USB Audio Interface connected to the Pi 5.
- **1× Bluetooth A2DP Input:** Onboard Pi 5 Bluetooth via BlueZ and PipeWire.
- **DSP Engine:** CamillaDSP 12-channel input → per-channel 4-band Parametric EQ → 12×2 Summing Mixer → 10-band Master Parametric EQ → Lookahead Peak Limiter.
- **User Interface:** Touchscreen Kiosk + LAN Web App (`http://pimix.local:8080`).

## Repository Layout

- [`docs/architecture.md`](docs/architecture.md) — Complete hardware, firmware, Linux, DSP, and UI architecture specification.
- [`docs/build_log.md`](docs/build_log.md) — Chronological engineering build log and bring-up notes.
- `firmware/pico-uac2-i2s/` — RP2040 C/C++ firmware (TinyUSB UAC2 + explicit feedback → PIO I2S slave transmitter).
- `firmware/patches/tinyusb-rp2040/` — TinyUSB RP2040 isochronous endpoint reactivation fix.
- `pi5/overlays/` — Custom Raspberry Pi 5 RP1 8-channel I2S Device Tree overlay (`pimix-rp1-i2s-8ch.dts`).
- `pi5/camilladsp/` — CamillaDSP 12-in / 2-out pipeline configuration.
- `pi5/wireplumber/` — PipeWire and WirePlumber deterministic routing scripts.
- `ui/` — Web UI and local WebSocket control daemon.
- `scripts/` — Build, sync, and flash helper scripts for `rpi5-dev`.

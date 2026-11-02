# PiMix Build Log

A chronological engineering log tracking the architecture, firmware, hardware bring-up, debugging, and software milestones of the **PiMix** 6-input desk audio mixer and parametric EQ.

---

## 2026-10-31 — Initial Architecture, RP1 8-Channel I2S Overlay, RP2040 UAC2 Firmware, and 4-Channel Loopback Verification

### 1. System Architecture & Design Decisions
- **Overview:** Designed a 6-stereo-input (12-channel) desk audio mixer and parametric EQ built around a **Raspberry Pi 5** (`rpi5-dev`) and **4× Raspberry Pi Picos (RP2040 / Pico W)** acting as independent USB Audio Class 2.0 (UAC2) sound cards for separate host computers.
- **4-Lane Stereo I2S vs. Shared Single-Wire TDM8:**
  - Rather than tri-stating 4 Picos onto a single 12.288 MHz TDM8 data wire, we took advantage of the Pi 5 RP1 southbridge's **4 parallel I2S data lanes** (`i2s0_sdi0`–`i2s0_sdi3` on `GPIO 20, 22, 24, 26`) sharing a master 3.072 MHz `BCLK` (`GPIO 18`) and 48.0 kHz `LRCLK` (`GPIO 19`).
  - Each Pico drives its own dedicated point-to-point stereo I2S data line at 3.072 MHz (`S32_LE` slots carrying 24-bit MSB-aligned PCM).
- **Galvanic Isolation Plan:**
  - In the final multi-host installation, each Pico is powered from its respective host PC's `VBUS` and separated from the Pi 5 across a quad digital isolator (`ISO7741` or `ADuM1401`, 3 forward + 1 reverse channel) to prevent cross-host ground loops and back-powering.
  - For initial bench bring-up and loopback testing, two Pico Ws are plugged directly into the Pi 5's USB ports (sharing the Pi 5 ground domain).
- **Full Spec:** Documented in [`docs/architecture.md`](architecture.md) and synchronized to the project Google Doc.

### 2. `rpi5-dev` Development Environment Setup
- Configured passwordless SSH and automated workspace sync ([`scripts/sync-to-rpi5.sh`](../scripts/sync-to-rpi5.sh)) to `rpi5-dev` (running Debian 13 Trixie arm64 with real-time kernel `Linux 6.12.75-v8-16k+ #1 SMP PREEMPT_RT`).
- Installed the RP2040 bare-metal toolchain (`arm-none-eabi-gcc 14.2.1`, `cmake`, `ninja-build`), cloned `pico-sdk v2.1.1` with TinyUSB submodules to `~/src/pico-sdk`, and built `picotool v2.1.1` from source (`/usr/local/bin/picotool`).
- Added `/etc/udev/rules.d/99-rp2040.rules` for non-root access to RP2040 `BOOTSEL` (`2e8a:0003`) and PiMix UAC2 (`2e8a:4001`–`4004`) USB devices.
- Installed **CamillaDSP v3.0.0** (`aarch64-unknown-linux-gnu`) to `/usr/local/bin/camilladsp`.

### 3. Custom Raspberry Pi 5 RP1 8-Channel I2S Master Overlay
- Authored [`pi5/overlays/pimix-rp1-i2s-8ch.dts`](../pi5/overlays/pimix-rp1-i2s-8ch.dts) and setup script [`scripts/setup-i2s-overlay.sh`](../scripts/setup-i2s-overlay.sh):
  - Muxes `gpio18` (`BCLK`), `gpio19` (`LRCLK`), and `gpio20, gpio22, gpio24, gpio26` (`SDI0`–`SDI3`) to RP1 function `i2s0`.
  - Binds `rp1_i2s0` as the I2S bitclock and frame master (`simple-audio-card,format = "i2s"`) against `linux,snd-soc-dummy`.
- Compiled and loaded the overlay dynamically via `dtoverlay`, exposing ALSA capture device `hw:PiMixRP18ch,0` (`card 4: PiMixRP18ch [PiMix-RP1-8ch]`).
- Verified via `pinctrl get 18,19,20,22,24,26` that `GPIO 18` and `GPIO 19` actively drive the 3.072 MHz bit clock and 48.0 kHz frame clock whenever the ALSA capture stream is open.

### 4. RP2040 UAC2-to-I2S Slave Firmware (`firmware/pico-uac2-i2s`)
- Built the dual-function RP2040 firmware in [`firmware/pico-uac2-i2s/`](../firmware/pico-uac2-i2s/):
  - **USB Audio Class 2.0 Stereo Sink:** 48.0 kHz, 24-bit packed (`S24_3LE`) stereo playback endpoint + explicit asynchronous feedback endpoint, per-channel volume/mute controls, and automatic identity assignment (`PiMix USB 1` through `PiMix USB 4`) based on either `GP2`/`GP3` strapping pins or the Pico's unique 64-bit NOR flash ID (`E66164084335BC2E` → `PiMix USB 1`, `E66164084354A62E` → `PiMix USB 2`).
  - **PIO External-Clocked I2S Slave Transmitter ([`i2s_slave.pio`](../firmware/pico-uac2-i2s/src/i2s_slave.pio)):** Waits for `LRCLK` high-to-low transition (`wait 1 jmp_pin` → `wait 0 jmp_pin`), honors the 1-`BCLK` standard I2S delay, and shifts out 64 bits per frame (32-bit Left + 32-bit Right, 24-bit MSB-aligned) on falling `BCLK` edges so `SDATA` is rock-solid on rising `BCLK` edges at the Pi 5.
  - **Ping-Pong DMA Engine:** Two chained RP2040 DMA channels (`dma_ch_a` and `dma_ch_b`) continuously feed the PIO TX FIFO from 48-frame (1 ms) stereo buffers, outputting clean digital silence (`0x00000000`) whenever USB audio is idle or muted.
  - **Out-of-Band USB Vendor Control Interface (`bRequest 0xBB / 0xBC / 0xBD`) & Watchdog:**
    - Added a 2-second hardware watchdog (`watchdog_enable(2000, 1)`) so the Pico automatically recovers if USB or DMA ever wedges.
    - Implemented vendor control requests on Interface 0 controllable via [`scripts/pimix-usb-ctl.py`](../scripts/pimix-usb-ctl.py):
      - `0xBB`: Software reboot into `BOOTSEL` mode (eliminating the need to physically hold the `BOOTSEL` button during firmware updates).
      - `0xBC`: Internal deterministic triangle-wave test generator (`--freq <Hz>`).
      - `0xBD`: Real-time hardware diagnostic readout (reporting live `GPIO 0..29` pad states, DMA completion counts, PIO `PC`/`TX_FIFO` level, and USB streaming status).

### 5. Debugging & Fixing the TinyUSB RP2040 Isochronous Endpoint Panic
- **Symptom:** During Linux enumeration, `dmesg` reported `usb 1-1.x: 1:1: usb_set_interface failed (-110)` (`ETIMEDOUT`), after which the Pico stopped responding to all USB control transfers.
- **Root Cause:** In Linux `sound/usb/stream.c`, `snd_usb_parse_audio_interface()` calls `usb_set_interface(chip->dev, iface_no, altno)` (`alt=1`) at the end of probe to leave the interface initialized, without submitting ISO OUT packets. When `snd-usb-audio` or WirePlumber subsequently called `SET_INTERFACE(1, 1)` again, TinyUSB's `audiod_set_interface()` invoked `dcd_edpt_iso_activate()` (`TUP_DCD_EDPT_ISO_ALLOC`). In TinyUSB 0.18.0 (`dcd_rp2040.c` / `rp2040_usb.c`), `dcd_edpt_iso_activate()` did not clear `USB_BUF_CTRL_AVAIL` on an already-active endpoint before re-preparing the transfer, triggering `panic("ep %02X was already available", ep->ep_addr)` and halting the RP2040 CPU.
- **Fix:** Patched `dcd_edpt_iso_activate()` in [`firmware/patches/tinyusb-rp2040/dcd_rp2040.c`](../firmware/patches/tinyusb-rp2040/dcd_rp2040.c) and `_hw_endpoint_buffer_control_update32()` in [`firmware/patches/tinyusb-rp2040/rp2040_usb.c`](../firmware/patches/tinyusb-rp2040/rp2040_usb.c) to cleanly disarm `buffer_control` before re-activating an isochronous endpoint. Both Picos now enumerate cleanly with zero kernel errors.

### 6. Physical Wiring Discovery & 4-Channel Loopback Verification
- Using `pimix-usb-ctl.py diag` while toggling pulls on the Pi 5 GPIO pins (`pinctrl set 18,19,20,22 pu/pd`), we inspected the live GPIO pad bitmask on both Picos and mapped the exact header pins wired on the bench:
  - Both Picos use the bottom-left 4 pins of the Pico header (**Physical Pins 17–20**):
    - **Pin 17 (`GP13`):** `SDATA` output (`PiMix USB 1` → Pi 5 `GPIO 20` / `SDI0`; `PiMix USB 2` → Pi 5 `GPIO 22` / `SDI1`)
    - **Pin 18 (`GND`):** Ground reference to Pi 5
    - **Pin 19 (`GP14`):** `BCLK` input (from Pi 5 `GPIO 18`, Pin 12)
    - **Pin 20 (`GP15`):** `LRCLK` input (from Pi 5 `GPIO 19`, Pin 35)
- Updated the firmware pin definitions to `GP13` / `GP14` / `GP15` and ran [`scripts/verify-loopback.py`](../scripts/verify-loopback.py):
  - **Result:** Simultaneous 24-bit / 48 kHz stereo USB playback (`aplay` on `hw:P1` with 375 Hz L / 750 Hz R and `hw:P2` with 600 Hz L / 1200 Hz R at `-6.0 dBFS`) captured via `arecord` on `hw:PiMixRP18ch` measured `-6.0 dBFS` peak, `-9.0 dBFS` RMS, and exact frequencies (`375.0 Hz`, `750.0 Hz`, `600.0 Hz`, `1200.0 Hz`) on all 4 channels with zero bit-slip (`0x00` in the bottom 8 padding bits of every 32-bit word).

### 7. Live CamillaDSP Mixing & Speaker Output Verification
- Connected a USB Audio DAC/Speaker interface (`hw:Device,0`, `1b3f:2008` Generalplus USB Audio Device, 48 kHz `S16_LE` stereo playback) to `rpi5-dev`.
- Created [`pi5/camilladsp/pimix-4ch-usb-out.yml`](../pi5/camilladsp/pimix-4ch-usb-out.yml) to capture all 4 I2S channels (`hw:PiMixRP18ch,0`, `S32_LE` @ 48 kHz), sum `PiMix USB 1` (Ch 0–1) and `PiMix USB 2` (Ch 2–3) into a stereo output bus with asynchronous rate adaptation (`enable_rate_adjust: true`), and play out to `hw:Device,0`.
- Created [`scripts/play-speaker-test.py`](../scripts/play-speaker-test.py) to launch CamillaDSP (`-p 1234` WebSocket control port enabled) and play a 14-second stereo musical test sequence across both Picos (`PiMix USB 1` solo arpeggio panning Left → Right, `PiMix USB 2` solo counter-melody panning Right → Left, and both Picos playing chords + melody simultaneously). Verified clean, glitch-free stereo audio from the speakers.

---

## 2026-11-01 — Real-Time Web UI, CamillaDSP Control Server, and 72-Band Live Spectrum Analyzer

### 1. Zero-Dependency Backend Control Daemon ([`ui/backend/server.py`](../ui/backend/server.py))
- Built an `asyncio` HTTP and RFC 6455 WebSocket server (`http://0.0.0.0:8080`) plus an async CamillaDSP WebSocket client (`ws://127.0.0.1:1234`) using only the Python standard library, `PyYAML`, and `NumPy`.
- Dynamically generates and hot-reloads the CamillaDSP v3.0.0 configuration (`pi5/camilladsp/pimix-live.yml`) via `SetConfigJson` without interrupting audio playback:
  - Per-channel 4-band Parametric EQ (`Lowshelf`, `2× Peaking`, `Highshelf` biquads).
  - Stereo summing matrix supporting per-channel Fader Gain (`-60` to `+12 dB`), constant-power Balance/Pan (`L100`..`R100`), Mute, Solo, and Polarity Invert (`Ø`).
  - Master 10-band Parametric/Graphic EQ (`31.5 Hz` – `16 kHz`), Master Trim, Soft Peak Limiter (`-0.5 dBFS`), and hardware USB DAC speaker volume (`amixer -c Device`).
  - Persistent state and named preset storage (`pi5/camilladsp/mixer_state.json`) plus a built-in test audio generator triggerable from the browser.

### 2. Digital Mixer & Interactive Parametric EQ Frontend ([`ui/frontend/`](../ui/frontend/))
- Created [`ui/frontend/index.html`](../ui/frontend/index.html), [`ui/frontend/styles.css`](../ui/frontend/styles.css), and [`ui/frontend/app.js`](../ui/frontend/app.js):
  - **6 Input Channel Strips (`USB 1`–`USB 4`, `Line In`, `Bluetooth`) + `Master Out` Strip** with real-time 25 FPS stereo L/R VU meters and peak dBFS readouts.
  - **Interactive Parametric EQ Canvas (`20 Hz – 20 kHz`):** Computes exact RBJ Audio EQ Cookbook biquad magnitude responses at 48.0 kHz for individual bands and the composite sum, with pointer/touch node dragging for frequency and gain.

### 3. Concurrent ALSA `dsnoop` Tap & 72-Band Real-Time Spectrum Analyzer (RTA)
- Configured `~/.asoundrc` with `pcm.pimix_snoop` (`type dsnoop` on `hw:PiMixRP18ch,0`) so CamillaDSP and a real-time FFT analyzer process (`arecord -D pimix_snoop`) read the 4-channel 32-bit / 48 kHz I2S capture buffer simultaneously with zero contention.
- Added a 4096-point Hann-windowed `numpy.fft.rfft` pipeline in `ui/backend/server.py` mapping FFT bins into 72 logarithmically spaced 1/6-octave bands (`20 Hz – 20 kHz`) for `USB 1`, `USB 2`, and the summed `Master` bus.
- Rendered **Pre-EQ Input Spectrum**, **Post-EQ Output Spectrum**, and a decaying **Peak-Hold** trace at 60 FPS directly behind the Parametric EQ curve on `#eq-canvas`.

# Multi-Input USB, Line & Bluetooth Desk Audio Mixer & EQ (PiMix)

This implementation plan defines the target architecture, hardware pinouts, galvanic isolation strategy, firmware design, Linux/PipeWire/CamillaDSP audio topology, and Web UI architecture for the **PiMix** 6-input desk audio mixer and equalizer.

## 1. System Architecture Overview

The PiMix architecture integrates six stereo audio sources—four independent USB host computers, one analog stereo line input, and one Bluetooth wireless stream—into a centralized, low-latency Digital Signal Processing (DSP) and routing hub powered by a Raspberry Pi 5.

- **Central Processor:** Raspberry Pi 5 acting as the system coordinator, I2S master clock generator, PipeWire + CamillaDSP host, and Web UI server.
- **Wired USB Host Inputs (4× Stereo):** 4× Raspberry Pi Pico (RP2040) microcontrollers, each configured as an independent USB Audio Class 2.0 (UAC2) asynchronous stereo sound card (24-bit / 48 kHz) presenting to separate host computers (macOS, Windows 10/11, and Linux).
- **Galvanic Isolation:** 4× quad-channel digital isolators (`ISO7741` or `ADuM1401`) placed on the 3.3V I2S serial lines between each RP2040 and the Raspberry Pi 5. Each Pico is bus-powered directly by its connected host computer, while the digital isolator prevents ground loops and parasitic USB backfeeding across hosts.
- **Digital Audio Routing (4-Lane I2S):** The Raspberry Pi 5's RP1 I/O controller (`snps,designware-i2s`) operates as I2S Clock Master (`BCLK` = 3.072 MHz, `LRCLK` = 48.0 kHz) and ingests 8 channels (4 stereo pairs) simultaneously over four dedicated serial data input pins (`SDI0`–`SDI3` on `GPIO 20, 22, 24, 26`). Dedicated per-Pico data lanes eliminate shared-bus tri-state contention.
- **Line Input & Master Output:** A single external USB Audio Interface connected to the Raspberry Pi 5 provides both the stereo **Analog Line Input** (ADC capture channels) and the stereo **Master Output** (DAC playback channels) on a single hardware sample clock.
- **Wireless Input:** 1× Bluetooth A2DP Sink operating on the Raspberry Pi 5 onboard Cypress/Infineon wireless module via BlueZ and PipeWire (supporting AAC, SBC-XQ, and LDAC).
- **Audio Plumbing & DSP Engine:** PipeWire and WirePlumber aggregate and sample-rate-adapt all six stereo sources into a unified 12-channel capture stream feeding **CamillaDSP**. CamillaDSP executes real-time per-channel trim, mute/solo, per-channel parametric EQ, stereo summing, master parametric/graphic EQ, and master peak limiting before outputting to the USB Audio Interface playback channels.
- **User Interface:** A responsive Local Web Application (Svelte / React) running in Kiosk mode on the Pi 5 touchscreen and simultaneously accessible over the local network (`http://pimix.local`) from any connected computer's web browser.

| Subsystem | Component | Role | Interface to Pi 5 |
| --- | --- | --- | --- |
| **Central Coordinator** | Raspberry Pi 5 (RP1) | I2S Clock Master, PipeWire & CamillaDSP Host, UI Server | Internal PCIe / RP1 Audio & USB 3.0 |
| **USB Host Input 1** | Raspberry Pi Pico #1 + `ISO7741` | UAC2 to Isolated I2S Bridge (Ch 0–1) | `GPIO 20` (`SDI0`), shared `BCLK`/`LRCLK` |
| **USB Host Input 2** | Raspberry Pi Pico #2 + `ISO7741` | UAC2 to Isolated I2S Bridge (Ch 2–3) | `GPIO 22` (`SDI1`), shared `BCLK`/`LRCLK` |
| **USB Host Input 3** | Raspberry Pi Pico #3 + `ISO7741` | UAC2 to Isolated I2S Bridge (Ch 4–5) | `GPIO 24` (`SDI2`), shared `BCLK`/`LRCLK` |
| **USB Host Input 4** | Raspberry Pi Pico #4 + `ISO7741` | UAC2 to Isolated I2S Bridge (Ch 6–7) | `GPIO 26` (`SDI3`), shared `BCLK`/`LRCLK` |
| **Line In & Master Out** | External USB Audio Interface | Stereo Line ADC (Ch 8–9) & Master Stereo DAC Out | Pi 5 USB Port (UAC2 Asynchronous) |
| **Wireless Streaming** | Pi 5 Onboard Bluetooth (BlueZ) | A2DP Audio Sink (Ch 10–11) | Onboard SDIO/UART → PipeWire |
| **DSP Core** | CamillaDSP | 12-In → 2-Out Mixing, Parametric EQ, Limiting | PipeWire / ALSA + WebSocket (`:1234`) |
| **User Interface** | Touchscreen + LAN Web Browsers | Faders, 60 FPS Meters, Parametric EQ Editor, Presets | Chromium Kiosk + HTTP/WebSocket |

## 2. Hardware Wiring & Galvanic Isolation Specification

Rather than multiplexing multiple microcontrollers onto a single high-speed TDM wire with tri-state switching, PiMix leverages the Raspberry Pi 5 RP1 controller's native **4-lane I2S architecture** (`SDI0`–`SDI3`). Each RP2040 receives the master clocks from the Pi 5 and drives its own dedicated stereo data pin.

### Galvanic Isolation Architecture (`ISO7741` / `ADuM1401`)

Connecting four separate host computers (e.g., two MacBooks and a dual-boot Windows/Linux desktop) to a common electrical ground creates severe ground-loop hum and risks backfeeding 5V VBUS power between hosts.

To isolate every host cleanly:
- **Host Side (Side 1 of Isolator):** Each Raspberry Pi Pico is powered solely via its micro-USB / USB-C connector from its host computer. The Pico's regulated `3V3(OUT)` (Pin 36) and `GND` (Pin 38) power `VCC1` and `GND1` of its dedicated `ISO7741F` (or `ADuM1401`) quad digital isolator.
- **Pi 5 Side (Side 2 of Isolator):** The Raspberry Pi 5's `3.3V` rail (Pin 1 / Pin 17) and `GND` (Pins 6, 9, 14, 20, 25, 30, 34, 39) power `VCC2` and `GND2` of all four isolators. **Do not tie Pico GND to Pi 5 GND.**
- **Signal Direction across Each Isolator (3 Forward, 1 Reverse):**
  - `BCLK` (Pi 5 → Pico): Isolator Channel A
  - `LRCLK / WS` (Pi 5 → Pico): Isolator Channel B
  - `SDATA` (Pico → Pi 5): Isolator Channel D (reverse channel)
  - Unused forward Channel C: Tie input to `GND2` on the Pi 5 side.

```
Host PC (USB) ──> [ Pico #n ] (3V3_PICO / GND_PICO)
                     │   ▲
       SDATA (Out)   │   │  BCLK & LRCLK (In)
                     ▼   │
              ┌─────────────────┐
              │  ISO7741 / 1401 │  <── Galvanic Isolation Barrier (5000 Vrms)
              └─────────────────┘
                     │   ▲
       SDI_n (In)    │   │  GPIO 18 (BCLK) & GPIO 19 (LRCLK)
                     ▼   │
                  [ Raspberry Pi 5 ] (3V3_PI / GND_PI)
```

*Figure 1: Per-channel galvanic isolation boundary between each host-powered RP2040 and the Raspberry Pi 5.*

### Bus Signals and Pinout

- **Bit Clock (`BCLK`):** Pi 5 `GPIO 18` (Header Pin 12) outputs the master 3.072 MHz bit clock, fanned out to the Side-2 `INA` inputs of all four digital isolators via 33 Ω series damping resistors.
- **Word Select / Frame Sync (`LRCLK` / `WS`):** Pi 5 `GPIO 19` (Header Pin 35) outputs the master 48.0 kHz word select clock, fanned out to the Side-2 `INB` inputs of all four digital isolators via 33 Ω series damping resistors.
- **Dedicated Serial Data Inputs (`SDI0`–`SDI3`):** Each Pico's isolated `OUTD` data output connects point-to-point to its assigned RP1 I2S data input pin on the Pi 5 header.
- **Decoupling:** Place a 100 nF ceramic capacitor directly across `VCC1`–`GND1` and `VCC2`–`GND2` within 2 mm of each isolator IC.

| Signal Name | Direction | Pi 5 Header Pin | Pi 5 GPIO (RP1 Mux) | Isolator Channel | Pico GPIO (Default) | Electrical Specs |
| --- | --- | --- | --- | --- | --- | --- |
| **BCLK** | Pi 5 → Picos 1–4 | Pin 12 | `GPIO 18` (`i2s0_sclk`) | Ch A (Pi → Pico) | `GPIO 14` (Pin 19, PIO In) | 3.072 MHz, 3.3V LVCMOS |
| **LRCLK (WS)** | Pi 5 → Picos 1–4 | Pin 35 | `GPIO 19` (`i2s0_ws`) | Ch B (Pi → Pico) | `GPIO 15` (Pin 20, PIO In) | 48.000 kHz, 3.3V LVCMOS |
| **SDI0 (Host 1)** | Pico #1 → Pi 5 | Pin 38 | `GPIO 20` (`i2s0_sdi0`) | #1 Ch D (Pico → Pi) | `GPIO 13` (Pin 17, PIO Out) | Stereo I2S Ch 0–1, 3.3V |
| **SDI1 (Host 2)** | Pico #2 → Pi 5 | Pin 15 | `GPIO 22` (`i2s0_sdi1`) | #2 Ch D (Pico → Pi) | `GPIO 13` (Pin 17, PIO Out) | Stereo I2S Ch 2–3, 3.3V |
| **SDI2 (Host 3)** | Pico #3 → Pi 5 | Pin 18 | `GPIO 24` (`i2s0_sdi2`) | #3 Ch D (Pico → Pi) | `GPIO 13` (Pin 17, PIO Out) | Stereo I2S Ch 4–5, 3.3V |
| **SDI3 (Host 4)** | Pico #4 → Pi 5 | Pin 37 | `GPIO 26` (`i2s0_sdi3`) | #4 Ch D (Pico → Pi) | `GPIO 13` (Pin 17, PIO Out) | Stereo I2S Ch 6–7, 3.3V |
| **3V3_PI / GND_PI** | Pi 5 → Isolators Side 2 | Pins 1, 17 / 6, 9, 14, 20 | Power Rail | `VCC2` / `GND2` | *Isolated* | 3.3V DC (Pi Domain) |
| **3V3_PICO / GND** | Pico #n → Isolator #n Side 1 | *Isolated* | *Isolated* | `VCC1` / `GND1` | Pin 36 (`3V3`) / Pin 18 (`GND`) | 3.3V DC (Host #n Domain) |

### Bus Protocol Definition: 4-Lane Stereo I2S

- **Sample Rate:** 48.0 kHz
- **Slot Width:** 32 bits per channel (24-bit signed PCM audio MSB-aligned, lower 8 bits zero-padded `S32_LE`)
- **Channels per Data Lane:** 2 (Left when `LRCLK` is low, Right when `LRCLK` is high, standard I2S 1-BCLK delay after `LRCLK` transition)
- **Total Parallel Channels:** 4 lanes × 2 channels = 8 channels
- **Bit Clock Frequency:** 48,000 Hz × 32 bits × 2 channels = **3.072 MHz**

## 3. RP2040 Firmware (USB UAC2 to I2S Slave Bridge)

All four Raspberry Pi Picos run identical firmware images, differentiating their USB descriptor index (`PiMix USB 1` through `PiMix USB 4`) and unique USB serial numbers via a 2-pin GPIO solder-jumper strap (`GPIO 2` and `GPIO 3`) or their onboard NOR flash unique ID.

### Firmware Architecture Stack

- **Foundation:** Pico C/C++ SDK (`pico-sdk`) with TinyUSB Device Stack (`tud_audio`).
- **USB Audio Class Configuration:**
  - **Primary Mode (UAC2 Asynchronous Stereo Sink):** 2-channel, 24-bit (in 3-byte or 4-byte container) at 48.0 kHz over USB Full-Speed (12 Mbps, 288 bytes per 1 ms frame).
  - **Explicit Asynchronous Feedback Endpoint:** Reports the actual sample consumption rate (governed by the Pi 5's master `LRCLK`) back to the host OS in 16.16 fixed-point format (`samples per 1 ms frame`, nominal `48.0000`). This ensures macOS, Windows 10/11, and Linux dynamically pace their USB packets to match the Pi 5 clock without buffer underruns, overruns, or pitch artifacts.
- **Dual-Core Task Split:**
  - **Core 0:** Runs the TinyUSB interrupt and task loop (`tud_task()`), parses incoming USB isochronous `OUT` packets, unpacks 24-bit samples into 32-bit MSB-aligned words, and pushes frames into a lockless SPSC (Single Producer, Single Consumer) ring buffer.
  - **Core 1 / DMA + PIO:** Feeds the PIO TX FIFO from the ring buffer via ping-pong DMA channels and calculates the real-time ring buffer fill level for the USB feedback endpoint.

### Programmable I/O (PIO) I2S Slave Driver

Because each Pico has its own dedicated `SDI` line to the Pi 5, the PIO state machine operates as a pure **external-clocked I2S Slave Transmitter**:
- The PIO program waits for `LRCLK` (`GPIO 15`, Pin 20) to transition high-to-low (start of Left channel frame), observes the standard 1-bit I2S delay, and shifts out 64 bits (32-bit Left + 32-bit Right) on `GPIO 13` (Pin 17) synchronized to falling edges of the external `BCLK` (`GPIO 14`, Pin 19) so data is stable on rising `BCLK` edges at the Pi 5.
- If the USB host is disconnected, muted, or not streaming, the DMA/PIO subsystem continuously outputs zeros (`0x00000000`) in lockstep with the Pi 5 clock so the I2S lane always carries valid silent PCM frames.

## 4. Raspberry Pi 5 Linux & Audio Plumbing Configuration

The Raspberry Pi 5 uses the RP1 southbridge, which exposes a Synopsys DesignWare I2S controller (`raspberrypi,rp1-i2s`) capable of 8-channel multi-lane capture.

### Custom Device Tree Overlay (`pimix-rp1-i2s-8ch.dts`)

To configure the Pi 5 as a continuous 8-channel I2S Clock Master (`i2s0`) with all four input lanes enabled (`SDI0`–`SDI3`), a custom Device Tree Overlay muxes `GPIO 18, 19, 20, 22, 24, 26` to function `i2s0` and binds `simple-audio-card` to a dummy 8-channel codec (`snd-soc-dummy`):

```dts
/dts-v1/;
/plugin/;

/ {
    compatible = "brcm,bcm2712";

    fragment@0 {
        target = <&rp1_gpio>;
        __overlay__ {
            rp1_i2s0_4lane_in: rp1_i2s0_4lane_in {
                function = "i2s0";
                pins = "gpio18", "gpio19", "gpio20", "gpio22", "gpio24", "gpio26";
                bias-disable;
            };
        };
    };

    fragment@1 {
        target = <&rp1_i2s0>;
        __overlay__ {
            pinctrl-names = "default";
            pinctrl-0 = <&rp1_i2s0_4lane_in>;
            #sound-dai-cells = <0>;
            status = "okay";
        };
    };

    fragment@2 {
        target-path = "/";
        __overlay__ {
            pimix_dummy_codec: pimix-dummy-codec {
                compatible = "linux,snd-soc-dummy";
                #sound-dai-cells = <0>;
                status = "okay";
            };

            pimix_sound: pimix-sound {
                compatible = "simple-audio-card";
                simple-audio-card,name = "PiMix-RP1-8ch";
                simple-audio-card,format = "i2s";
                simple-audio-card,bitclock-master = <&cpu_dai>;
                simple-audio-card,frame-master = <&cpu_dai>;

                cpu_dai: simple-audio-card,cpu {
                    sound-dai = <&rp1_i2s0>;
                    dai-tdm-slot-num = <2>;
                    dai-tdm-slot-width = <32>;
                };

                simple-audio-card,codec {
                    sound-dai = <&pimix_dummy_codec>;
                };
            };
        };
    };
};
```

*Listing 1: Device Tree overlay configuring RP1 `i2s0` as an 8-channel (4-lane) master receiver.*

### Multi-Clock-Domain Aggregation with PipeWire & WirePlumber

CamillaDSP supports a **single** capture device block and a **single** playback device block per instance. Furthermore, the system spans three distinct hardware clock domains:
1. **Pi 5 RP1 Clock Domain:** 8-channel I2S capture from Picos 1–4 (`PiMix-RP1-8ch`, 48.0 kHz).
2. **USB Audio Interface Clock Domain:** 2-channel Line Input capture and 2-channel Master Output playback (48.0 kHz on the external USB interface's crystal).
3. **Bluetooth Clock Domain:** 2-channel BlueZ A2DP Sink (`bluez_input.*`, asynchronous wireless packets).

To bridge these clock domains into CamillaDSP with zero dropouts:
- **CamillaDSP Native PipeWire Backend (or ALSA Loopback):** CamillaDSP exposes a 12-channel capture node (`camilladsp-capture`, ports `in_0` through `in_11`) and a 2-channel playback node (`camilladsp-playback`, ports `out_0`, `out_1`) in the PipeWire graph at a fixed 48.0 kHz / 256-sample quantum (5.3 ms latency).
- **PipeWire Adaptive Resampling (ASRC):** PipeWire's graph clock locks to the **USB Audio Interface** (which drives both the Line Input and the Master Output without resampling) and applies lightweight, high-precision adaptive resampling to the RP1 8-channel I2S input and the Bluetooth A2DP sink.
- **Deterministic WirePlumber Linking Rules:** A dedicated WirePlumber script (`~/.config/wireplumber/wireplumber.conf.d/50-pimix-routing.conf` + Lua linker) automatically binds:
  - `PiMix-RP1-8ch` capture channels `0..7` → `camilladsp-capture` ports `0..7` (USB Hosts 1–4)
  - USB Audio Interface capture channels `0..1` → `camilladsp-capture` ports `8..9` (Line Input)
  - Any active `bluez_input.*` stream `0..1` → `camilladsp-capture` ports `10..11` (Bluetooth Input)
  - `camilladsp-playback` ports `0..1` → USB Audio Interface playback channels `0..1` (Master Output)

## 5. DSP Engine (CamillaDSP)

CamillaDSP processes all 12 input channels (6 stereo pairs) at 64-bit floating-point internal precision and outputs the stereo master bus.

### Channel Mapping & Processing Pipeline

| Input Group | CamillaDSP Channels | Source Description | Per-Channel Processing Stage |
| --- | --- | --- | --- |
| **USB 1** | Ch 0 (L), Ch 1 (R) | Pico #1 (e.g., MacBook #1) | Input Trim, Mute/Phase, 4-Band Parametric EQ |
| **USB 2** | Ch 2 (L), Ch 3 (R) | Pico #2 (e.g., MacBook #2) | Input Trim, Mute/Phase, 4-Band Parametric EQ |
| **USB 3** | Ch 4 (L), Ch 5 (R) | Pico #3 (e.g., Win/Linux PC) | Input Trim, Mute/Phase, 4-Band Parametric EQ |
| **USB 4** | Ch 6 (L), Ch 7 (R) | Pico #4 (Auxiliary Host) | Input Trim, Mute/Phase, 4-Band Parametric EQ |
| **Line In** | Ch 8 (L), Ch 9 (R) | USB Audio Interface Line In | Input Trim, Mute/Phase, 4-Band Parametric EQ |
| **Bluetooth** | Ch 10 (L), Ch 11 (R) | Pi 5 BlueZ A2DP Sink | Input Trim, Mute/Phase, 4-Band Parametric EQ |

### Signal Flow Topology

```
+-----------------------------------------------------------------------+
|                              CamillaDSP                               |
|                                                                       |
|  [Ch 0-1:  USB Host 1 (Pico 1)] ──> [Gain/Mute] ──> [4-Band PEQ] ──┐  |
|  [Ch 2-3:  USB Host 2 (Pico 2)] ──> [Gain/Mute] ──> [4-Band PEQ] ──┤  |
|  [Ch 4-5:  USB Host 3 (Pico 3)] ──> [Gain/Mute] ──> [4-Band PEQ] ──┤  |
|  [Ch 6-7:  USB Host 4 (Pico 4)] ──> [Gain/Mute] ──> [4-Band PEQ] ──┼─> [12x2 Mixer]
|  [Ch 8-9:  Analog Line Input]   ──> [Gain/Mute] ──> [4-Band PEQ] ──┤        │
|  [Ch 10-11: Bluetooth A2DP]     ──> [Gain/Mute] ──> [4-Band PEQ] ──┘        ▼
|                                                                     [Master 10-Band PEQ]
|                                                                             │
|                                                                             ▼
|                                                                     [Master Peak Limiter]
|                                                                             │
|                                                                             ▼
|                                                                   [Master Out (USB DAC)]
+-----------------------------------------------------------------------+
                                      ▲
                                      │ (WebSocket API :1234)
                            [PiMix Web UI & Daemon]
```

*Figure 2: CamillaDSP 12-input to 2-output signal processing and WebSocket control topology.*

### Filter & Dynamics Capabilities

- **Per-Source 4-Band Parametric EQ:** Each stereo input pair has 4 configurable biquad filter bands (Default: Band 1 `Highpass` / `LowShelf`, Bands 2–3 `Peaking`, Band 4 `HighShelf` with adjustable frequency, gain ±15 dB, and Q factor).
- **12×2 Summing Mixer:** Independent channel faders (`Gain` filters with smooth ramp interpolation to prevent zipper noise), per-channel Mute/Solo logic, and stereo balance controls.
- **Master Bus Parametric / Graphic EQ:** Up to 10 bands of parametric Biquad EQ on the summed stereo bus for room correction, desk monitor tuning, or headphone EQ profiles (e.g., AutoEQ target curves).
- **Master Peak Limiter:** Fast-attack lookahead compressor/limiter at the end of the chain to prevent digital clipping (`0 dBFS`) when multiple computers play audio simultaneously.

## 6. Touchscreen & Network Web User Interface

The user interface is built as a modern **Local Web Application (Svelte / React)** paired with a lightweight local backend daemon on the Raspberry Pi 5.

### Dual-Access Architecture

1. **On-Desk Touchscreen Kiosk:** The Pi 5 runs a lightweight Wayland compositor (`cage` or `labwc`) launching Chromium in fullscreen kiosk mode (`--kiosk --ozone-platform=wayland`) pointing to `http://localhost:8080`.
2. **LAN Browser Control:** Any device on the local network (including the connected MacBooks, Windows/Linux workstation, or phone) can open `http://pimix.local:8080` to control faders, edit parametric EQ curves with a mouse, and switch output presets in real time. Every connected UI client stays synchronized via WebSocket state broadcasts.

### Core UI Views

- **Mixer Console View:**
  - 6 stereo input channel strips (`USB 1`, `USB 2`, `USB 3`, `USB 4`, `Line In`, `Bluetooth`) + 1 `Master` strip.
  - Smooth 60 FPS stereo peak/RMS bar meters with peak-hold and clipping indicators (polled from CamillaDSP via WebSocket).
  - Touch-friendly faders, Mute, Solo, and quick-access EQ bypass buttons.
- **Interactive Parametric EQ & Real-Time Spectrum Analyzer (RTA) View:**
  - Real-time frequency-response curve canvas (20 Hz – 20 kHz logarithmic plot) showing individual band curves and the composite EQ curve.
  - 72-band 1/6-octave Real-Time Spectrum Analyzer (RTA) computed via a 4096-point Hann-windowed FFT from a concurrent ALSA `dsnoop` tap (`pimix_snoop`), displaying Pre-EQ input spectrum, Post-EQ output spectrum, and decaying Peak Hold directly behind the EQ curve.
  - Draggable control nodes for Frequency, Gain, and Q (bandwidth) for any selected input channel or the Master bus.
- **Bluetooth & System Drawer:**
  - Displays connected Bluetooth device name, codec, and transport controls (Play/Pause/Next via BlueZ MediaPlayer1 D-Bus API), plus one-tap pairing mode.
- **Preset & Profile Manager:**
  - Instant switching and saving of full mixer + EQ snapshots (e.g., *Desk Monitors*, *Headphones*, *Focus Mode*, *Gaming*).

## 7. Implementation Roadmap

| Phase | Milestone | Key Deliverables |
| --- | --- | --- |
| **Phase 1** | **Single-Pico UAC2 → I2S Prototype** | RP2040 C/C++ firmware with TinyUSB UAC2 + explicit feedback and PIO I2S slave output; verified on macOS, Windows, and Linux. |
| **Phase 2** | **Pi 5 8-Channel RP1 I2S & Isolation** | `pimix-rp1-i2s-8ch.dtbo` Device Tree overlay, breadboard validation of `ISO7741` isolation with 4 Picos on `GPIO 20, 22, 24, 26`. |
| **Phase 3** | **PipeWire + CamillaDSP 12-Channel Core** | CamillaDSP YAML pipeline (6 stereo inputs → PEQ → Master EQ/Limiter → USB Audio Interface) and WirePlumber auto-routing scripts. |
| **Phase 4** | **Web UI & Kiosk Integration** | Svelte/React mixer & interactive parametric EQ interface, backend daemon for presets/Bluetooth, and Pi 5 Wayland kiosk service. |
| **Phase 5** | **PCB / Enclosure & Desk Deployment** | Carrier PCB for the 4 Picos + 4 `ISO7741` isolators + Pi 5 40-pin header mating, plus 3D-printed desk enclosure with touchscreen. |

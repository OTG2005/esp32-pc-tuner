# ESP32 PC Tuner

An ESP32-based Bluetooth speaker and sound-reactive 8×8 RGB matrix controller. The firmware receives music over Bluetooth A2DP, plays it through a MAX98357A amplifier, keeps an I2S microphone active at the same time, and drives responsive lighting from the audio actually sent to the speaker.

> **Current status:** firmware builds successfully and is ready for hardware testing. PC power switching is intentionally disabled until the planned Wi-Fi/ESP-NOW companion node is implemented.

## Features

- Bluetooth Classic **A2DP sink** for phone/PC music playback.
- Simultaneous Bluetooth Classic and **BLE controls** using ESP32 BTDM mode.
- Simultaneous audio input and output using both ESP32 I2S controllers:
  - I2S0 TX for the MAX98357A speaker amplifier.
  - I2S1 RX for the digital microphone.
- Mono downmix so both stereo channels are heard through one speaker.
- Audio-responsive lighting based on the decoded PCM sent to the amplifier.
- Automatic microphone fallback when Bluetooth playback has been inactive for 300 ms.
- Adaptive gain, microphone noise-floor tracking, beat detection, and efficient bass/mid/treble estimation.
- Four music modes: spectrum, wave, pulse, and sparkle.
- Rainbow, blink, color-blend, fade, static-color, and brightness modes.
- 8×8 WS2812/NeoPixel matrix mapping with configurable serpentine wiring.
- IR remote control and Nordic UART Service BLE control.
- Non-blocking animations and queued BLE commands to avoid audio dropouts.
- Bluetooth auto-reconnect.
- Compatibility layer for legacy IDF 4 I2S and newer IDF 5 I2S through Arduino Audio Tools.
- PC power GPIO pulses and direct PC switching are removed from this release.

## Required hardware

| Qty. | Part | Recommended specification | Purpose / notes |
|---:|---|---|---|
| 1 | ESP32 development board | Original ESP32-WROOM/ESP32 DevKit with Bluetooth Classic | ESP32-S2, S3, C3, and C6 do not provide the Bluetooth Classic A2DP sink required by this project. |
| 1 | Addressable RGB matrix | 8×8 WS2812B/NeoPixel, 5 V, 64 pixels | Firmware defaults to serpentine row wiring. |
| 1 | I2S microphone | INMP441 or compatible 3.3 V, 32-bit I2S microphone | Current configuration reads the left I2S slot. |
| 1 | I2S audio amplifier | MAX98357A mono Class-D breakout | Powered from 5 V; receives standard Philips I2S. |
| 1 | Speaker | 4 Ω, 3 W recommended; 8 Ω, 1–3 W also works | Connect only to the amplifier's differential `SPK+`/`SPK-` terminals. |
| 1 | IR receiver | 3.3 V-compatible demodulating receiver, such as VS1838B/TSOP-style module | Connect its digital output to GPIO 34. |
| 1 | Regulated power supply | **5 V, 6 A recommended** | Sized for the matrix at full white plus speaker and ESP32 peaks. |
| 1 | Logic-level shifter | 74AHCT125, 74HCT14, or equivalent | Recommended for reliable 3.3 V-to-5 V WS2812 data. |
| 1 | Matrix data resistor | 330–470 Ω | Place in series near the first LED data input. |
| 1 | Bulk capacitor | 1000 µF or larger, rated at least 6.3 V | Place across matrix 5 V and GND near the panel. |
| 1 | Power fuse | Approximately 5 A, selected for the wiring and supply | Recommended during prototyping and final installation. |
| — | Hook-up wire/connectors | 18–22 AWG for high-current 5 V/GND; lighter wire for signals | Do not carry matrix or speaker current through breadboard power rails or the ESP32 regulator. |

Optional but useful: a powered USB cable with data-only isolation, screw terminals, an enclosure, an amplifier shutdown switch, and additional 5 V/GND injection at the far side of the LED matrix.

## Pin layout

### Signal pins

| ESP32 pin | Direction | Connected device | Signal | Peripheral |
|---:|:---:|---|---|---|
| GPIO 4 | Output | 8×8 RGB matrix | DIN through 330–470 Ω resistor/level shifter | WS2812/RMT |
| GPIO 22 | Output | MAX98357A | DIN | I2S0 TX |
| GPIO 25 | Output | MAX98357A | LRC / WS | I2S0 TX |
| GPIO 26 | Output | MAX98357A | BCLK | I2S0 TX |
| GPIO 32 | Output | I2S microphone | WS / LRCLK | I2S1 RX clock |
| GPIO 33 | Output | I2S microphone | SCK / BCLK | I2S1 RX clock |
| GPIO 35 | Input only | I2S microphone | SD / DOUT | I2S1 RX data |
| GPIO 34 | Input only | IR receiver | OUT | IR input |
| GPIO 16 | Unused | Reserved | Former PC power pin; no current function | Future feature available |

GPIO 4 is an ESP32 boot-strapping pin. The WS2812 data input normally does not drive it, but the external circuit must not force GPIO 4 low during reset.

### Power connections

| Rail | Connections |
|---|---|
| Regulated 5 V | RGB matrix 5 V, MAX98357A VIN, and ESP32 `5V/VIN` only if the specific board supports 5 V input there |
| ESP32 3.3 V | I2S microphone VDD and a 3.3 V-compatible IR receiver |
| Common GND | ESP32, matrix, amplifier, microphone, IR receiver, and external supply |
| Speaker | MAX98357A `SPK+` and `SPK-` only; neither speaker lead goes to ground |

### Wiring overview

```text
                            +---------------- MAX98357A ---------------- Speaker
ESP32 GPIO26 --------------> BCLK                                  SPK+ / SPK-
ESP32 GPIO25 --------------> LRC/WS
ESP32 GPIO22 --------------> DIN

                            +---------------- I2S microphone
ESP32 GPIO33 --------------> SCK/BCLK
ESP32 GPIO32 --------------> WS/LRCLK
ESP32 GPIO35 <-------------- SD/DOUT
Mic L/R -------------------> GND (left slot)

ESP32 GPIO4 --> 74AHCT125 --> 330–470 Ω --> 8×8 matrix DIN
ESP32 GPIO34 <-------------------------------- IR receiver OUT

5 V / 6 A supply ----------> Matrix 5 V and MAX98357A VIN
                           -> ESP32 5V/VIN only when supported by the board
All grounds ----------------> Common ground
```

Do not connect external 5 V and USB 5 V in parallel unless the development board explicitly supports safe power-path isolation. During programming, use a data-only USB arrangement or disconnect the external feed to the board while keeping signal grounds common.

## Estimated power usage

These are engineering estimates, not measurements. Actual WS2812 revisions, brightness, animation content, speaker impedance, audio waveform, and supply efficiency change the result.

| Load | Typical operating current | Conservative maximum budget | Power at 5 V |
|---|---:|---:|---:|
| ESP32 with A2DP + BLE | 0.10–0.25 A | 0.50 A transient budget | Up to 2.5 W |
| 64-pixel WS2812 matrix | 0.3–1.8 A for normal animated patterns | 3.84 A at 64 × 60 mA full white | Up to 19.2 W |
| MAX98357A + 4 Ω speaker | 0.10–0.45 A for normal music | Approximately 0.75 A near maximum output | Up to 3.75 W input |
| I2S microphone + IR receiver | Below 0.01 A | 0.02 A budget | Below 0.1 W |
| **Estimated system total** | **Approximately 0.5–2.5 A** | **Approximately 5.1 A** | **Approximately 2.5–25.5 W** |

The firmware starts at brightness `192/255` (about 75%). A theoretical all-white matrix at that limit can still approach 2.9 A, and the controls allow brightness to reach 255. Design the wiring for the 5.1 A worst-case estimate rather than the average animation load.

### Recommended power design

- Use a regulated **5 V, 6 A (30 W)** supply for comfortable margin.
- Feed the matrix and amplifier directly from the supply, not through the ESP32 board.
- Use adequately sized power and ground wire; inject matrix power at more than one point if voltage drop is visible.
- Keep audio and microphone wiring away from high-current LED leads.
- Place the bulk capacitor near the matrix and add local amplifier decoupling according to the MAX98357A breakout documentation.
- Start testing with low LED brightness and low phone volume.

## Firmware architecture

Bluetooth A2DP delivers interleaved, volume-processed 16-bit PCM. A short callback calculates amplitude and approximate bass/mid/treble energy without drawing pixels or delaying I2S writes. The Arduino loop reads those shared levels and renders LEDs at a controlled frame rate.

The microphone remains live on I2S1 while the speaker runs on I2S0. Using different controllers avoids the original problem where installing one I2S driver replaced or disabled the other. If A2DP packets stop for 300 ms, sound-reactive modes automatically switch to microphone levels.

## Bluetooth controls

Both Bluetooth interfaces advertise as **`OTL PC Tuner`**. Pair the media device with the Classic Bluetooth audio endpoint. BLE controls use the Nordic UART Service UUIDs.

| BLE command | Action |
|---|---|
| `N` / `F` | Lights on / off; these commands no longer switch the PC |
| `U` / `D` | Brightness up / down |
| `W`, `R`, `G`, `B` | White, red, green, or blue static color |
| `C`, `J`, `K`, `A` | Rainbow, blink, color blend, or seven-color fade |
| `1`, `2`, `3`, `S` | Audio sparkle, pulse, wave, or spectrum |

The existing IR remote color, effect, brightness, and sensitivity buttons remain supported. The former 1H–4H threshold buttons now select adaptive audio sensitivity.

## Software requirements

The following versions were detected and used for the verified build:

| Software | Tested version |
|---|---:|
| Arduino IDE | 2.3.10 |
| Espressif Arduino-ESP32 board core | 2.0.10 |
| Adafruit NeoPixel | 1.15.5 |
| IRremote | 4.7.1 |
| ESP32-A2DP | 1.8.10 |
| Arduino Audio Tools | 1.2.2 |

`AudioTools.h` must be included before `BluetoothA2DPSink.h`, as done in the sketch. The firmware also contains the IDF 5 channel-selection path for a correctly installed newer ESP32 core.

## Build and upload

1. Clone the repository.
2. Open `sketch_may4a/sketch_may4a.ino` in Arduino IDE.
3. Install the libraries listed above through Library Manager or from their official repositories.
4. Select these board options:

   | Option | Value |
   |---|---|
   | Board | ESP32 Dev Module |
   | CPU frequency | 240 MHz |
   | Flash size | 4 MB |
   | Partition scheme | Huge APP |
   | PSRAM | Disabled, unless the actual module includes it |

5. Compile, upload, and open Serial Monitor at 115200 baud.
6. Pair `OTL PC Tuner` as a Bluetooth audio device and select one of the audio lighting modes.

Verified build usage with warnings enabled:

- Flash: **1,380,097 / 3,145,728 bytes (43%)**
- Static RAM: **51,300 / 327,680 bytes (15%)**

Hardware behavior still needs to be validated on the final assembled circuit before enclosure installation or unattended use.

## Configuration

The main hardware options are near the top of `sketch_may4a.ino`:

- `MATRIX_SERPENTINE`: set to `false` for progressive rows.
- `SPEAKER_BCLK_PIN`, `SPEAKER_WS_PIN`, `SPEAKER_DATA_PIN`: MAX98357A wiring.
- `MIC_BCLK_PIN`, `MIC_WS_PIN`, `MIC_DATA_PIN`: microphone wiring.
- `MIC_SAMPLE_RATE`: microphone capture rate; currently 32 kHz.

The microphone is configured for the left I2S slot. For a microphone whose L/R pin is tied high, change the modern configuration to `I2SChannelSelect::Right` and the legacy configuration to `I2S_CHANNEL_FMT_ONLY_RIGHT`.

## Upcoming updates

The following items are planned; they are **not implemented in the current firmware**:

### 1. Wi-Fi/ESP-NOW PC power companion

- A separate Wi-Fi-capable ESP receiver installed near the PC motherboard.
- ESP-NOW commands from the PC Tuner to the receiver.
- An isolated optocoupler or suitable transistor across the motherboard power-switch header.
- Command acknowledgement, retry limits, anti-repeat timing, and fail-safe behavior.
- No direct PC power wiring on the current audio/lighting ESP32.

### 2. Configuration and maintenance

- NVS persistence for brightness, last mode, sensitivity, and matrix orientation.
- Wi-Fi provisioning without hard-coded credentials.
- OTA firmware updates and a recovery path.
- Optional local web interface for setup and diagnostics.

### 3. Audio and lighting improvements

- True FFT-based eight-band spectrum mode.
- Guided microphone noise-floor and gain calibration.
- Configurable matrix rotation, origin, and color correction.
- Current-aware LED brightness limiting based on the configured supply capacity.

### 4. Controls and status

- BLE command acknowledgements and live connection/audio-source status.
- Play/pause, previous/next, and volume commands over BLE and IR.
- Bluetooth metadata display support for a future OLED or companion application.
- Optional mobile control interface.

## Safety notes

- A bare speaker must never be connected directly to an ESP32 GPIO.
- Never power the RGB matrix through the ESP32 3.3 V regulator.
- Confirm polarity before connecting the high-current 5 V supply.
- Use a common ground for all signal-connected modules.
- Do not connect any future PC motherboard header directly to an ESP32; use an electrically appropriate isolated or transistor interface.

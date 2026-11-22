# ESP32 PC Tuner

An ESP32-based Bluetooth speaker and sound-reactive 8×8 RGB matrix controller. The firmware receives music over Bluetooth A2DP, plays it through a MAX98357A amplifier, and drives lighting exclusively from that decoded Bluetooth PCM. Microphone capture and fallback have been removed.

> **Testing branch: `testing/bluetooth-only-lights`.** Bluetooth-only lighting is not yet approved for `main`. The plain page has **no login**, the setup hotspot is **open**, and it closes once the router assigns an IP. See [docs/TESTING.md](docs/TESTING.md) for acceptance checks and the explicit-confirmation release gate. PC power switching remains disabled.

## Start here

- **Sketch:** `sketch_may4a/sketch_may4a.ino` — keep all accompanying `.h`/`.cpp` files. The local working copy is `E:\PC Tuner`.
- **Wi-Fi + Internet tutorial:** [docs/WEB_AND_INTERNET.md](docs/WEB_AND_INTERNET.md).
- **Complete Bluetooth commands:** [docs/BLUETOOTH_COMMANDS.md](docs/BLUETOOTH_COMMANDS.md).
- **Current LAN page:** `http://192.168.100.33/` or `http://pc-tuner.local/`.
- **Provisioning:** `PC-Tuner-Setup-3B89`, no password, `http://192.168.4.1/`.

This PC was found on Ethernet, not Wi-Fi. Use the ESP32's open setup page
to enter the **same home router's 2.4 GHz Wi-Fi** details. The Ethernet PC can
reach the ESP32 on that LAN. For another network, use the documented outbound
HTTPS tunnel through an always-on home host; no router port forwarding is needed.
Because device login was removed, require an external email/Access/VPN gate
before exposing controls over the Internet. Legacy device/hotspot passwords
in `private/access.txt` are unused.

## Features

- Bluetooth Classic **A2DP sink** for phone/PC music playback.
- Simultaneous Bluetooth Classic and **BLE controls** using ESP32 BTDM mode.
- I2S0 TX for the MAX98357A speaker amplifier; no I2S1 RX driver, DMA buffers, or microphone sampling.
- Mono downmix so both stereo channels are heard through one speaker.
- Audio-responsive lighting based on the decoded PCM sent to the amplifier.
- Music effects go dark after 300 ms without fresh Bluetooth PCM; there is no ambient-audio fallback.
- Adaptive Bluetooth PCM gain, beat detection, and efficient bass/mid/treble estimation.
- Four music modes: spectrum, wave, pulse, and sparkle.
- Rainbow, blink, color-blend, fade, static-color, and brightness modes.
- 8×8 WS2812/NeoPixel matrix mapping with configurable serpentine wiring.
- IR remote control and Nordic UART Service BLE control.
- Plain, no-login web page with buttons for every existing lighting/audio function; 5.6 KB HTML, 1.9 KB gzip.
- All 23 color presets (including original IR tints) and all 10 effects available over web and BLE.
- Readable commands such as `lights on`, `color deep-pink`, `effect spectrum`, and `audio pause`.
- Speaker volume, mute, playback, Bluetooth-meter calibration, complete help, and network status.
- Open, device-specific setup hotspot; verified-router NVS persistence, automatic retries, and mDNS.
- No automatic web polling, dynamic JSON catalog, or login/session allocations.
- Optional HTTPS Internet access through a home `cloudflared` connector, with email/Cloudflare Access protection.
- Non-blocking animations and queued BLE commands to avoid audio dropouts.
- Bluetooth auto-reconnect.
- Compatibility layer for legacy IDF 4 I2S and newer IDF 5 I2S through Arduino Audio Tools.
- PC power GPIO pulses and direct PC switching are removed from this release.

## Required hardware

| Qty. | Part | Recommended specification | Purpose / notes |
|---:|---|---|---|
| 1 | ESP32 development board | Original ESP32-WROOM/ESP32 DevKit with Bluetooth Classic | ESP32-S2, S3, C3, and C6 do not provide the Bluetooth Classic A2DP sink required by this project. |
| 1 | Addressable RGB matrix | 8×8 WS2812B/NeoPixel, 5 V, 64 pixels | Firmware defaults to serpentine row wiring. |
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
| GPIO 32, 33, 35 | Unused | Former microphone wiring | No signal generated/captured | Available for future use |
| GPIO 34 | Input only | IR receiver | OUT | IR input |
| GPIO 16 | Unused | Reserved | Former PC power pin; no current function | Future feature available |

GPIO 4 is an ESP32 boot-strapping pin. The WS2812 data input normally does not drive it, but the external circuit must not force GPIO 4 low during reset.

### Power connections

| Rail | Connections |
|---|---|
| Regulated 5 V | RGB matrix 5 V, MAX98357A VIN, and ESP32 `5V/VIN` only if the specific board supports 5 V input there |
| ESP32 3.3 V | A 3.3 V-compatible IR receiver |
| Common GND | ESP32, matrix, amplifier, IR receiver, and external supply |
| Speaker | MAX98357A `SPK+` and `SPK-` only; neither speaker lead goes to ground |

### Wiring overview

```text
                            +---------------- MAX98357A ---------------- Speaker
ESP32 GPIO26 --------------> BCLK                                  SPK+ / SPK-
ESP32 GPIO25 --------------> LRC/WS
ESP32 GPIO22 --------------> DIN

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
| IR receiver | Below 0.01 A | 0.02 A conservative budget | Below 0.1 W |
| **Estimated system total** | **Approximately 0.5–2.5 A** | **Approximately 5.1 A** | **Approximately 2.5–25.5 W** |

The firmware starts at brightness `192/255` (about 75%). A theoretical all-white matrix at that limit can still approach 2.9 A, and the controls allow brightness to reach 255. Design the wiring for the 5.1 A worst-case estimate rather than the average animation load.

### Recommended power design

- Use a regulated **5 V, 6 A (30 W)** supply for comfortable margin.
- Feed the matrix and amplifier directly from the supply, not through the ESP32 board.
- Use adequately sized power and ground wire; inject matrix power at more than one point if voltage drop is visible.
- Keep I2S audio wiring away from high-current LED leads.
- Place the bulk capacitor near the matrix and add local amplifier decoupling according to the MAX98357A breakout documentation.
- Start testing with low LED brightness and low phone volume.

## Firmware architecture

Bluetooth A2DP delivers interleaved, volume-processed 16-bit PCM. A short callback calculates amplitude and approximate bass/mid/treble energy without drawing pixels or delaying I2S writes. The Arduino loop reads those shared levels and renders LEDs at a controlled frame rate.

Only the speaker's I2S0 TX driver is initialized. Bluetooth PCM arrives through
the A2DP callback; the loop never reads an external audio input. At 300 ms
without fresh PCM, levels and spectrum history reset and music effects go
dark. Non-music/static effects keep their normal behavior.

## Bluetooth controls

Classic audio advertises **`OTL PC Tuner`**; BLE UART advertises **`OTL PC Tuner BLE`**. Use Nordic UART in a BLE app, not Classic serial/SPP. Only one BLE controller connects at a time.

| BLE command | Action |
|---|---|
| `lights on` / `lights off` / `lights toggle` | LED power, independent of the speaker |
| `brightness up` / `brightness down` / `brightness high` | Brightness adjustment or named preset; numbers 0–255 also work |
| `color white` / `color rose-red` / `color #00AACC` | All named colors or custom RGB |
| `effect rainbow` / `effect fade-seven` / `effect spectrum` | All static, animated, and music modes |
| `sensitivity balanced` / `source bluetooth` / `calibrate audio` | Bluetooth lighting sensitivity and adaptive meters |
| `volume normal` / `mute on` / `downmix mono` | Speaker controls, independent of LED brightness |
| `audio play` / `audio pause` / `audio stop` / `audio next` / `audio previous` | AVRCP control of the paired music source |
| `wifi reconnect` / `wifi status` | Network connection/status; no password echo |
| `settings reset` / `status` / `help` | Defaults, live state, complete command help |

Append a real **LF, CRLF, or CR** to each BLE command and subscribe to NUS TX
notifications for replies. The old single-character commands are replaced.
See [docs/BLUETOOTH_COMMANDS.md](docs/BLUETOOTH_COMMANDS.md) for every parameter,
UUID, preset, and BLE fragmentation example.

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

ArduinoJson is no longer needed. HTTP, Wi-Fi, BLE, and mDNS use the bundled
ESP32 SDK, with bounded memory profiles.

`AudioTools.h` must be included before `BluetoothA2DPSink.h`, as done in the
sketch. The tested board core is 2.0.10; do not infer validation of newer cores.

## Build and upload

1. Use the local project or check out `testing/bluetooth-only-lights`. Do not use this testing change as an approved `main` release.
2. Run `python tools/configure_local.py` from that folder. It preserves existing private configuration. Then open `sketch_may4a/sketch_may4a.ino` in Arduino IDE.
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
6. Join the open, MAC-suffixed `PC-Tuner-Setup-3B89` hotspot. Forget the previously saved WPA2 entry if needed. Open `http://192.168.4.1` without signing in and enter the router's 2.4 GHz credentials. The hotspot closes at successful router connection; use the station IP or `http://pc-tuner.local` afterward.
7. Pair `OTL PC Tuner` as a Bluetooth audio device and select one of the audio lighting modes. BLE controls and web controls remain available.

Repeat the local build and tests without uploading:

```powershell
python tools/compile_firmware.py
python tools/run_local_tests.py
python tools/local_server.py
```

The loopback simulator opens at `http://localhost:8765` and visibly identifies
itself as **LOCAL SIMULATION**. It does not operate an ESP32 and must never be
tunneled. See [docs/LOCAL_TEST_RESULTS.md](docs/LOCAL_TEST_RESULTS.md) for exact
build usage, test scope, and limitations.

Basic web/router/BLE behavior of the earlier lightweight build was checked
on the device. The Bluetooth-only testing change still requires its own
hardware acceptance checks, especially sustained audio and circuit-connected
reset behavior. See [docs/TESTING.md](docs/TESTING.md).

## Configuration

The main hardware options are near the top of `sketch_may4a.ino`:

- `MATRIX_SERPENTINE`: set to `false` for progressive rows.
- `SPEAKER_BCLK_PIN`, `SPEAKER_WS_PIN`, `SPEAKER_DATA_PIN`: MAX98357A wiring.
- Optional bootstrap router credentials live in ignored `sketch_may4a/LocalConfig.h`; no device/hotspot password is used.
- Router Wi-Fi is entered in the plain setup page and saved after successful connection. Responses and BLE never return router passwords.
- Edit `web/index.html`, then run `python tools/build_web.py` to synchronize the embedded `WebPage.h`.

The former microphone can be disconnected with power off. GPIO32/33/35 are
unused in this firmware, and the microphone is no longer required hardware.

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
- OTA firmware updates and a recovery path.

Wi-Fi provisioning, web setup, and network credential persistence are already
implemented in this local update. Lighting settings are not yet persisted.

### 3. Audio and lighting improvements

- True FFT-based eight-band spectrum mode.
- Further Bluetooth PCM gain/beat calibration.
- Configurable matrix rotation, origin, and color correction.
- Current-aware LED brightness limiting based on the configured supply capacity.

### 4. Controls and status

- Optional encrypted/bonded BLE control and a dedicated IR playback layout.
- Bluetooth metadata display support for a future OLED or companion application.

BLE acknowledgements, live status, playback/volume over BLE and web, and the
responsive web control interface are already implemented locally.

## Safety notes

- A bare speaker must never be connected directly to an ESP32 GPIO.
- Never power the RGB matrix through the ESP32 3.3 V regulator.
- Confirm polarity before connecting the high-current 5 V supply.
- Use a common ground for all signal-connected modules.
- Do not connect any future PC motherboard header directly to an ESP32; use an electrically appropriate isolated or transistor interface.

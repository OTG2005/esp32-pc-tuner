# ESP32 PC Tuner (Dual-ESP32 Wi-Fi Architecture)

A two-board ESP32 Bluetooth speaker and sound-reactive 8×8 RGB matrix system connected over local Wi-Fi:

- **Board 1 — Controller & Speaker (`controller_node/`)**: Original ESP32 DevKit handling Bluetooth Classic A2DP audio (`MAX98357A`), BLE Nordic UART commands (`OTL PC Tuner BLE`), lightweight no-login Web UI (`http://pc-tuner.local`), Bluetooth PCM audio analysis, and all 4 sound-reactive LED modes (`sparkle`, `pulse`, `wave`, `spectrum`) streamed over Wi-Fi UDP to Board 2. No microphone, LEDs, or IR hardware are attached.
- **Board 2 — LED Matrix & IR Node (`led_node/`)**: Original ESP32 DevKit driving the 8×8 WS2812B/NeoPixel matrix (`GPIO 4`) and IR receiver (`GPIO 34`). Stores and runs all non-audio modes (`static`, `rainbow`, `blink`, `blend`, `fade-three`, `fade-seven`) directly on the board, displays streamed RGB frames from the Controller during sound-reactive modes, and syncs IR button presses back to the Controller over Wi-Fi.

Separating the boards keeps the speaker amplifier completely isolated from high-current WS2812 PWM switching noise, gives the Controller ample flash and RAM headroom for Wi-Fi, Web UI, Bluetooth, and future companion nodes (e.g., PC power switch or sensors) on the same UDP protocol.

## Architecture overview

```text
  Phone / PC (Audio + Web + BLE)
        │  A2DP Audio / BLE UART / HTTP
        ▼
  ┌────────────────────────────────────────────┐
  │ Board 1: controller_node (ESP32 DevKit)    │
  │ - MAX98357A I2S0 Speaker (GPIO 26/25/22)   │
  │ - Web UI (pc-tuner.local) & BLE UART       │
  │ - Bluetooth PCM analyzer (no mic)          │
  │ - Sound-reactive renderer (4 audio modes)  │
  └─────────────────────┬──────────────────────┘
                        │ Wi-Fi LAN (UDP port 4210 — NodeProtocol)
                        │ • Auto-discovery & ACKs
                        │ • State commands (power, brightness, color, mode)
                        │ • ~33 FPS 8×8 RGB frames in audio modes
                        │ • IR state sync back from LED Node
                        ▼
  ┌────────────────────────────────────────────┐
  │ Board 2: led_node (ESP32 DevKit)           │
  │ - 8×8 WS2812 Matrix (GPIO 4)               │
  │ - IR Receiver (GPIO 34, all 41 buttons)    │
  │ - Local non-audio modes (static, rainbow,  │
  │   blink, blend, fade-3, fade-7)            │
  │ - Remote RGB frame display (500 ms timeout)│
  └────────────────────────────────────────────┘
```

## Hardware & Wiring

Power the two boards from **separate 5 V supplies** (or well-isolated rails) so high-current LED matrix switching never injects noise into the MAX98357A speaker amplifier. No wires are needed between Board 1 and Board 2.

### Board 1: Controller & Speaker (`controller_node/`)

| ESP32 Pin | Direction | Connected Device | Signal | Peripheral |
|---:|:---:|---|---|---|
| GPIO 26 | Output | MAX98357A | BCLK | I2S0 TX |
| GPIO 25 | Output | MAX98357A | LRC / WS | I2S0 TX |
| GPIO 22 | Output | MAX98357A | DIN | I2S0 TX |

- Power MAX98357A from a clean **5 V, 1–2 A** supply (common GND with Board 1 only).
- Connect a 4 Ω 3 W (or 8 Ω 1–3 W) speaker to `SPK+` / `SPK-` only.

### Board 2: LED Matrix & IR Node (`led_node/`)

| ESP32 Pin | Direction | Connected Device | Signal | Peripheral |
|---:|:---:|---|---|---|
| GPIO 4 | Output | 8×8 WS2812B Matrix | DIN via 74AHCT125 + 330–470 Ω resistor | WS2812/RMT |
| GPIO 34 | Input | 3.3 V IR Receiver (VS1838B/TSOP) | OUT | IR input |

- Power the 8×8 matrix from a dedicated **5 V, 5–6 A** supply with a `>= 1000 µF` bulk capacitor across 5 V and GND near the panel (common GND with Board 2 only).
- Power the IR receiver from Board 2's **3.3 V** pin.
- `MATRIX_SERPENTINE` defaults to `true` in `led_node/led_node.ino`; set to `false` for progressive row wiring.

## Wi-Fi Node Protocol (`shared/NodeProtocol.*`)

Both boards communicate over local Wi-Fi using a compact binary UDP protocol on **port `4210`** (`NodeProtocol`, v1) with no hardcoded IPs:

- **Auto-discovery**: Board 2 broadcasts `NodeAnnounce` (`NodeType::LedMatrix`) every 3 seconds; Board 1 learns its IP automatically and pushes the current lighting state.
- **Reliable state commands**: Web/BLE lighting changes send `LedCommand` with sequence numbers, ACKs, and up to 3 automatic retries (`120 ms` interval).
- **Sound-reactive streaming**: In `sparkle`, `pulse`, `wave`, or `spectrum` modes, Board 1 analyzes Bluetooth A2DP PCM (with a `300 ms` silence timeout to dark) and streams 207-byte `LedFrame` packets (64 × RGB24) at ~33 FPS. Board 2 clears the matrix automatically if no frame arrives for `500 ms`.
- **Bidirectional IR sync**: Pressing any button on the IR remote (`GPIO 34` on Board 2) updates local non-audio modes immediately and sends `LedStateSync` back to Board 1 so the Web UI, BLE `status`, and audio-mode streaming stay in sync.
- **Extensible for future nodes**: `NodeType` already reserves IDs for `PcPower` and `Sensor` nodes so Board 1 can control additional ESP boards over the same Wi-Fi network.

## Software Requirements

| Software | Tested Version |
|---|---:|
| Arduino IDE | 2.3.10 |
| Espressif Arduino-ESP32 core | 2.0.10 |
| ESP32-A2DP *(Board 1 only)* | 1.8.10 |
| Arduino Audio Tools *(Board 1 only)* | 1.2.2 |
| Adafruit NeoPixel *(Board 2 only)* | 1.15.5 |
| IRremote *(Board 2 only)* | 4.7.1 |

## Build & Upload

Both sketches are self-contained and ready to open directly in Arduino IDE.

### 1. Upload Board 1 (`controller_node/controller_node.ino`)
1. Open `controller_node/controller_node.ino` in Arduino IDE.
2. Board settings: **ESP32 Dev Module**, 240 MHz, 4 MB Flash, **Partition Scheme: Huge APP**, PSRAM Disabled.
3. Upload and open Serial Monitor at `115200` baud.
4. Connect to the open hotspot `PC-Tuner-Setup-XXXX` -> open `http://192.168.4.1` -> enter your 2.4 GHz router Wi-Fi credentials. Once connected, the hotspot closes and the web panel is available at `http://pc-tuner.local` (or the printed router IP).
5. Pair `OTL PC Tuner` as your Bluetooth audio speaker (and optionally `OTL PC Tuner BLE` for BLE Nordic UART commands).

### 2. Upload Board 2 (`led_node/led_node.ino`)
1. Open `led_node/led_node.ino` in Arduino IDE.
2. Board settings: **ESP32 Dev Module**, 240 MHz, 4 MB Flash, Default 4MB (or Huge APP), PSRAM Disabled.
3. Upload and open Serial Monitor at `115200` baud.
4. Connect to the open hotspot `PC-Tuner-LED-XXXX` -> open `http://192.168.4.1` -> enter the **same 2.4 GHz router Wi-Fi credentials**. Once connected, the hotspot closes and Board 1 automatically discovers Board 2 over Wi-Fi (visible under `LED node=online` on `/api/status` and BLE `status`).

*(Optional)* You can also pre-populate `LocalConfig.h` (copied from `LocalConfig.example.h`) in either sketch folder to skip hotspot provisioning.

## Development & Native Tests

If you edit files in `shared/` or `web/index.html`, run:

```bash
python3 tools/sync_shared.py   # Sync shared/ files into controller_node/ and led_node/
python3 tools/build_web.py     # Rebuild controller_node/WebPage.h from web/index.html
```

Run the native C++ unit test suite (tests `ControlProtocol`, all 41 `IrControl` bindings, `NodeProtocol` packet codecs, and `AudioEffectRenderer`):

```bash
g++ -std=c++17 -Wall -Wextra -Werror \
  tests/native_tests.cpp \
  shared/ControlProtocol.cpp \
  shared/NodeProtocol.cpp \
  controller_node/AudioEffects.cpp \
  led_node/IrControl.cpp \
  -o /tmp/pc_tuner_tests && /tmp/pc_tuner_tests
```

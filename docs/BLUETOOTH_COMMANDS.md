# Complete word-based Bluetooth and web commands

Classic Bluetooth **A2DP** carries audio to the speaker. The **BLE Nordic UART
Service** receives control text. Classic audio is **`OTL PC Tuner`** and BLE UART
is **`OTL PC Tuner BLE`**. Only one BLE controller connects at a time. Pairing
the audio endpoint alone does not open a BLE command console.

## Connect and send a command

1. Use a BLE UART application such as nRF Connect or a compatible BLE serial app.
2. Connect to the device/service:

   | Item | UUID |
   |---|---|
   | Nordic UART Service | `6E400001-B5A3-F393-E0A9-E50E24DCCA9E` |
   | RX: write/write-without-response | `6E400002-B5A3-F393-E0A9-E50E24DCCA9E` |
   | TX: read/notifications | `6E400003-B5A3-F393-E0A9-E50E24DCCA9E` |

3. Subscribe to **TX notifications**.
4. Write ASCII text to RX, for example `effect spectrum`, followed by a **real**
   LF/newline, CRLF, or CR. Typing the two characters `\` and `n` is not a newline.
   For example, `lights on` with LF is the hex payload
   `6C 69 67 68 74 73 20 6F 6E 0A`.
5. Wait for the reply before sending the next command. Replies are split into
   at most 20-byte notifications for the default BLE MTU; join them until newline.

Commands are case-insensitive and take one meaningful word/number parameter.
Hyphens are part of color/effect names. A complete command is at most **95 ASCII
bytes** before its line ending. If the app cannot write more than 20 bytes,
split a command across writes and put the line ending only at the end.
An oversized or NUL-containing line is discarded, never executed partially.
Disconnecting clears an unfinished command.

The old `N`, `F`, `U`, `D`, `W`, `R`, `G`, `B`, `C`, `J`, `K`, `A`, `1`, `2`,
`3`, and `S` commands were replaced; they are not aliases.

## Every command

| Command | Parameters / behavior |
|---|---|
| `lights on` | Enable LEDs; keep the selected color/effect. |
| `lights off` | Disable LEDs without muting music. |
| `lights toggle` | Toggle LED power. |
| `brightness up` / `brightness down` | Adjust LED brightness by 20; clamp to 0–255. |
| `brightness low` / `normal` / `high` / `maximum` | Set 64 / 128 / 192 / 255. Write the full verb each time, e.g. `brightness high`. |
| `brightness 128` | Set any integer 0–255. Zero makes LEDs dark without changing their power flag. |
| `color deep-pink` | Select a named static color from the table below. |
| `color #00AACC` | Custom 24-bit RGB, exactly `#RRGGBB`. |
| `effect spectrum` | Select a named effect from the table below. |
| `sensitivity low` / `balanced` / `normal` / `high` / `maximum` | Set 0.65 / 0.90 / 1.00 / 1.20 / 1.60. |
| `sensitivity 1.25` | Set a decimal multiplier from 0.1 to 3.0. |
| `source bluetooth` | The sole lighting input is decoded Bluetooth PCM. |
| `calibrate audio` | Reset adaptive Bluetooth gain/envelope estimates. |
| `volume up` / `volume down` | Adjust speaker/AVRCP volume by 5; clamp to 0–127. |
| `volume low` / `normal` / `high` / `maximum` | Set 25 / 55 / 90 / 127. |
| `volume 55` | Set any integer 0–127, independently of LED brightness. |
| `mute on` / `mute off` / `mute toggle` | Mute/unmute while preserving the requested volume. |
| `downmix mono` | Mix both stereo channels for one MAX98357A speaker. |
| `downmix stereo` | Keep stereo I2S slots; a single amplifier is still mono hardware. |
| `audio play` | Queue AVRCP play for the connected music source. |
| `audio pause` | Queue AVRCP pause. |
| `audio stop` | Queue AVRCP stop, if supported by the source application. |
| `audio next` | Queue next track. |
| `audio previous` | Queue previous track. |
| `wifi reconnect` | Restart the current Wi-Fi connection attempt. |
| `wifi status` | Return SSID, station IP, signal, setup-hotspot state/address, and hostname; never passwords. |
| `settings reset` | Restore default controls, retaining Wi-Fi credentials. Device passwords are no longer used. |
| `status` | Return light/audio/control state and network information. |
| `help` | Stream the complete command/parameter help. |

**Playback acknowledgements are not proof that a music app acted on the
command.** A2DP plus AVRCP must be connected, and source apps differ in support.
Playback commands report an error when transport is unavailable; other
controls remain usable.

Brightness/color/effect changes while lights are off are remembered in runtime
state until lights are enabled. Settings currently return to defaults on boot;
only network credentials are persisted.

## All 23 named colors

| Parameter | RGB | Original IR color key |
|---|---|---|
| `white` | `#FFFFFF` | W |
| `red` | `#FF0000` | Pure red, also formerly available via BLE |
| `green` | `#00FF00` | Pure green, also formerly available via BLE |
| `blue` | `#0000FF` | Pure blue, also formerly available via BLE |
| `rose-red` | `#FF0028` | R |
| `spring-green` | `#00FF50` | G |
| `azure-blue` | `#0050FF` | B |
| `pink` | `#FF69B4` | Pink 1 |
| `deep-pink` | `#FF1493` | Pink 2 |
| `cyan` | `#00FFFF` | Cyan 1 |
| `turquoise` | `#40E0D0` | Cyan 2 |
| `indigo` | `#4B00FF` | Blue 2 |
| `purple` | `#8000FF` | Purple 1 |
| `orchid` | `#BA55D3` | Purple 2 |
| `magenta` | `#FF00FF` | Purple 3 |
| `orange` | `#FF5000` | Orange 1 |
| `amber` | `#FF8C00` | Orange 2 |
| `gold` | `#FFC800` | Orange 3 |
| `yellow` | `#FFFF28` | Yellow |
| `lime` | `#78FF00` | Light green |
| `sky-blue` | `#00B4FF` | Light blue 1 |
| `teal` | `#0080B4` | Light blue 2 |
| `mint` | `#00FF8C` | Green blue |

## All 10 effects

| Parameter | Effect |
|---|---|
| `static` | Hold the selected static color. |
| `rainbow` | Animated rainbow across the matrix. |
| `blink` | Cycle RGB colors. |
| `blend` | Continuous color-wheel blend. |
| `fade-three` | Fade among red, green, and blue. |
| `fade-seven` | Fade among seven colors. |
| `sparkle` | Audio-responsive sparkles. |
| `pulse` | Audio-responsive pulse/beat lighting. |
| `wave` | Audio-responsive matrix wave. |
| `spectrum` | Approximate bass/mid/treble spectrum columns; not a full FFT. |

## Original remote behavior

All **41 original IR keys** remain supported and are catalogued in the web
panel. `IrControl.cpp` is the single source of the IR mapping.

- Physical brightness +/- retains the original 10-point step and permits
  held-key repeats. Web/BLE +/- uses 20.
- DIM1–DIM4 selects 64, 128, 192, 255.
- 1H–4H selects sensitivity 1.60, 1.20, 0.90, 0.65; these are **not timers**.
- FLASH = fade-three; FADE3 = rainbow; FADE7 = fade-seven.
- JUMP3 = blink; JUMP7 = blend.
- S1 = spectrum; S2 = sparkle; S3 = pulse; S4 = wave.

Web/BLE buttons cover every existing IR function, including the tinted
R/G/B colors and the original 0.90 sensitivity preset.

## Replies, defaults, and security

Successful actions reply `ok <function>: <message>`. Invalid commands reply
`error ...` without applying changes. `status` uses several newline-terminated
messages; `help` uses several help lines. Send commands at a human pace, waiting
for previous replies, rather than flooding the bounded queues.

Default controls:

```text
lights on
brightness 192
effect rainbow
color #FFFFFF
sensitivity 1.00
source bluetooth
volume 55
mute off
downmix mono
```

The shared `status`, `wifi status`, and `help` queries do not increment the
control revision or modify settings.

The retained BLE UART is a **local, unencrypted control channel**, and IR is
also unauthenticated. Anyone nearby with a compatible controller may operate
non-secret controls. Do not rely on BLE as a remote Internet transport.
Router password entry is limited to the web setup form; there is no BLE
password-provisioning command. The web page now has no device login and the
provisioning AP is open, per user request. Use a trusted setting and require
an external gate for Internet access.

For Wi-Fi and secure Internet access, see
[WEB_AND_INTERNET.md](WEB_AND_INTERNET.md).

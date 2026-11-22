# Bluetooth-only testing candidate verification

**Date:** 2026-11-22

**Project:** `E:\PC Tuner`

**Branch:** `testing/bluetooth-only-lights`

**Current firmware marker:** `testing-bluetooth-only-1`
**Release status:** not approved for `main`; explicit owner confirmation required.

## Outcome

The Bluetooth-only sketch was built and uploaded to **COM11**, with flash
verification. Microphone sampling, its I2S1 RX driver/buffers, source selectors,
and fallback/noise-floor logic are removed. Music effects go dark after
300 ms without fresh Bluetooth PCM.

The earlier `lite-web-6` baseline was confirmed working for router-side web
access, setup AP closure, and BLE replies. Those results are historical
baseline evidence, not complete validation of this candidate.

This candidate is committed/pushed only to the testing branch, not `main`.
No full-flash/NVS erase, router configuration change by the agent, or public
tunnel activation was performed. Router credentials remain private.

### Hardware acceptance is still blocked

The current COM11 capture showed garbled serial output and repeated ROM boot
banners before eventually starting `testing-bluetooth-only-1`. A follow-up
capture also contained repeated/truncated text. These observations are **not
a clean hardware pass**, nor enough to identify the electrical/software cause.
Do not promote the candidate based only on compile/tests or verified flashing.
Use [TESTING.md](TESTING.md) to record circuit/audio acceptance results.

## What caused the earlier failures

The supplied crash ELF hash was matched to the Arduino IDE build:
`3a819c3e879f9296…`.

The decoded trace entered the Bluetooth GATT connection allocation path,
`fixed_queue_new`, then `osi_sem_free`/`vQueueDelete` while cleaning up a failed
allocation. Hardware tests subsequently showed network allocation failures
and minimum heap below 1 KB while loading the former page. The phone's password/
connection errors were therefore not reliable evidence of a wrong key.

Intermediate builds were diagnostic, not claimed successful. The final design
removes admin/session handling and dynamic JSON responses, uses a small page,
bounded network buffers, an open provisioning AP, a compact GATT server, and
ESP32-required Wi-Fi modem sleep for Bluetooth coexistence.

## Actual build and upload

| Result | Value |
|---|---|
| Hardware identified | ESP32-D0WD-V3 revision 3.1, CP210x on COM11 |
| Board core | Arduino-ESP32 2.0.10 |
| Board settings | ESP32 Dev Module, 240 MHz, 4 MB, Huge APP, PSRAM off |
| Program storage | **1,792,305 / 3,145,728 bytes (56% reported)** |
| Static RAM | **69,116 / 327,680 bytes (21% reported)** |
| Plain web page | **5,615 UTF-8 bytes** |
| Gzip transferred page | **1,887 bytes** |
| Upload verification | Passed |

Uploads wrote bootloader, partition table, boot-app image, and application
regions. The NVS region `0x9000..0xDFFF` was not erased.
Build artifacts contain configuration data; keep `.local/` private and ignored.

## Native/API/tool regression tests

Command: `python tools/run_local_tests.py`

- **2,875 native checks**, plus compile-time resource-profile assertions.
- **16 loopback API/tool tests**, all passed.
- **117 button/IR/boundary command cases** through the real compiled C++ logic.
- All 23 named colors, all 10 effects, all 41 original IR keys, BLE framing,
  bounds, Bluetooth-only freshness, reset, volume/mute, and transport errors covered.
- Tests reject removed microphone/automatic source commands and verify there
  is no microphone buffer, input read, or RX driver setup in the active sketch.
- Gzip bytes decompress exactly to the HTML source.
- Old login/logout/catalog routes removed; direct local controls require no
  authentication, per user request.
- Router credentials are not echoed in status/replies.
- Remote helper accepts the new status protocol and refuses an ungated public
  tunnel.
- JavaScript/Python syntax and `git diff --check` passed.

The loopback HTTP server simulates radio/network state. Its parser/reducer is
the same portable C++ used by firmware; hardware verification is listed separately.

## Earlier baseline live-device checks (`lite-web-6`)

- Normal boot and HTTP startup: captured on COM11.
- Open `PC-Tuner-Setup-3B89`, no sign-in page, successful form use and router
  join: confirmed by the user.
- Hotspot off after router join: independently confirmed by the status endpoint.
- Root page and status fetched from the Ethernet PC on the real station IP.
- In-app Chromium opened the real page, confirmed 23 colors, 10 effects,
  70 command buttons, zero graphic elements, and no login form. The Status
  button returned the real router-connected/setup-off status.
- BLE connect and notifications: user-confirmed; serial capture recorded a
  connected client and stable running heap without allocation-failure messages.
- Basic HTTP identity/status preflight of the real device: passed without
  starting an Internet tunnel.
- Runtime free heap varies with clients; observations were approximately
  24 KB after open-AP startup, 26 KB in a router-side status read, and 11 KB
  with the observed BLE client connected. These are snapshots, not a guarantee
  under every traffic/audio load.
- Observed router signal approximately **−79 dBm**; improve placement for
  reliability.

All Bluetooth-only/circuit-connected acceptance checks still need to be
repeated for the current candidate. Audio fidelity, sustained A2DP + web + BLE
stress, every physical IR key/matrix effect, and Internet access need extended
tests. No hardware-release, Internet, or audio-quality approval is implied.

## Local files

- `sketch_may4a/sketch_may4a.ino` and supporting `.cpp`/`.h` files.
- `web/index.html` → generated `sketch_may4a/WebPage.h`.
- `.local/firmware-build.log`, `.local/firmware/`.
- `.local/diagnostics/lite-web-6-*.log` and upload/backtrace logs.
- `.local/diagnostics/testing-bluetooth-only-1-*.log` and the verified upload log.
- Legacy admin code and previous source snapshots retained under
  `.local/backups/`; they are not compiled by the active sketch.

Private configuration files were preserved. Legacy device/hotspot passwords
are unused by the current firmware. No direct Internet exposure is permitted
without an external email/Access/VPN gate.

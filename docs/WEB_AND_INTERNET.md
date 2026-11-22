# Lightweight PC Tuner: Wi-Fi, web, Bluetooth, and Internet

The current firmware has **no device web login**. It uses a plain, compact
control page, an **open provisioning hotspot**, and no automatic status polling.
The Bluetooth-only change is published for testing on
`testing/bluetooth-only-lights`, not as an approved `main` release.

## Current verified device

- USB: **COM11**, CP210x bridge, ESP32-D0WD-V3 revision 3.1.
- Router address found during testing: **http://192.168.100.33/**
- mDNS: **http://pc-tuner.local/**; use the numeric IP if name resolution fails.
- This PC is on Ethernet on the same router LAN.
- The setup hotspot closed after the router connection succeeded.
- nRF Connect connection and `status` replies were confirmed on the earlier
  lightweight build. Re-run the acceptance checks for the Bluetooth-only build.

The IP may change unless you reserve it in the router's DHCP client settings.

## First-time or recovery Wi-Fi setup

1. Compile/upload the complete `sketch_may4a` folder, including its `.cpp`/`.h`
   files. Use the original ESP32 with Bluetooth Classic, 4-MB flash, 240 MHz,
   **Huge APP**, and PSRAM disabled unless your board actually has it.
2. If no saved router connection works, join **`PC-Tuner-Setup-3B89`** on a
   phone. Other boards use their own MAC suffix.
3. **No hotspot password is needed.** Forget the previously saved secured
   hotspot entry first if the phone still asks for a password.
4. Stay connected despite the phone's “No Internet” warning.
5. Open **http://192.168.4.1/**. The control page opens directly—there is no
   admin, username, or password page.
6. Enter the router's **2.4-GHz SSID and router password** in the Wi-Fi form.
   The router password is still needed for a secured router; leave it blank
   only for an intentionally open router network.
7. Click **Connect router**. When the ESP32 obtains an IP, credentials are
   saved in NVS and the open setup hotspot closes immediately.
8. Rejoin the normal router network and open the station IP printed in Serial
   Monitor at **115200 baud**. This Ethernet PC can access that same IP.

If the router is unavailable, the setup hotspot returns after approximately
30 seconds; configured-router retries occur approximately every 45 seconds.
An unsuccessful candidate does not replace the last successful NVS record.

The old device/hotspot passwords in `private/access.txt` are **no longer used
by this firmware**. Existing private files were retained, not erased or rotated.
Protect the board, private files, and compiled binaries.

## Use the controls

The small page includes all 23 colors, all 10 effects, lights/brightness,
custom RGB, Bluetooth sensitivity/calibration, volume, mute,
mono/stereo slots, playback, Wi-Fi status/reconnect, help, and control reset.

- Status is fetched only when requested; there is no continuous polling.
- Turning lights off does not mute music.
- Music lighting uses only decoded Bluetooth PCM and goes dark after 300 ms
  without fresh samples. Microphone capture and fallback are removed.
- `source bluetooth` is the sole input. `source automatic` and `source microphone`
  are no longer valid commands.
- One MAX98357A remains mono hardware; mono downmix combines both channels.
- Playback buttons require an A2DP/AVRCP source; the source app may ignore
  unsupported operations.
- Reset restores controls, not Wi-Fi credentials.
- PC motherboard switching and OTA are still not implemented.

Only basic request-length and hardware-parameter checks remain. The firmware
does not use sessions, login validation, a JSON web API, or a dynamic catalog.

## Bluetooth UART

Classic audio advertises **`OTL PC Tuner`**. BLE UART advertises
**`OTL PC Tuner BLE`**. Serial Bluetooth Terminal must use **BLE/Nordic UART**
mode, not Classic SPP.

Only one BLE controller can connect at a time. Disconnect/close another BLE
app before connecting nRF Connect.

1. Connect to `OTL PC Tuner BLE`.
2. Subscribe to TX: `6E400003-B5A3-F393-E0A9-E50E24DCCA9E`.
3. Write to RX: `6E400002-B5A3-F393-E0A9-E50E24DCCA9E`.
4. Append a real LF, CRLF, or CR. `status` + LF in hex is
   `73 74 61 74 75 73 0A`.
5. Fragment long commands into 20-byte writes; join TX notifications until
   newline. Wait for replies rather than flooding the queues.

Full words/presets: [BLUETOOTH_COMMANDS.md](BLUETOOTH_COMMANDS.md).

## Internet access: protect it outside the ESP32

**Anyone on the reachable local network can now operate the controls.**
The provisioning hotspot is also open. Enter router credentials only in a
trusted setting, and keep the open setup period short.

Do **not** forward port 80 or expose this no-login firmware directly to the
Internet. Use Cloudflare email protection, Cloudflare Access, or a private VPN.

### Temporary protected HTTPS tunnel

Keep an always-on home PC/Raspberry Pi on the ESP32's router LAN.
`cloudflared` was already installed on this PC. If necessary:

```powershell
winget install --id Cloudflare.cloudflared
```

From `E:\PC Tuner`, check the real device without opening a tunnel:

```powershell
python tools/start_remote.py --target http://192.168.100.33 --check-only
```

Then replace the example email with your own:

```powershell
python tools/start_remote.py --target http://192.168.100.33 --allowed-email "you@example.com"
```

The helper verifies the device, pins a private IPv4 address, rejects the
setup hotspot/simulator, and **refuses an ungated public tunnel**.
The installed `cloudflared` must support `--allowed-mail`; it will not
silently omit protection.

Open the printed `https://…trycloudflare.com` URL on mobile data or another
network. Complete the email one-time PIN gate; there is no second device login.
Keep the home host awake and connected. Ctrl+C stops the tunnel.

Quick Tunnel URLs change on restart and are for testing. No public tunnel was
started during the firmware-debugging session.

### Stable address

Use a named Cloudflare Tunnel with a domain in your Cloudflare account:

1. Reserve the ESP32's DHCP address.
2. Create a self-hosted **Cloudflare Access** application for the chosen
   hostname, such as `tuner.example.com`, **before exposing it**.
3. Allow only your trusted email/identity group; do not add Everyone/bypass.
4. Create/run the home connector and route the exact hostname to the ESP32
   station address over local HTTP.
5. Verify an unauthorized browser is denied, then install the connector's
   service using the dashboard instructions if desired.

This project does not create account/domain routes or install a service
automatically. Tokens and account certificates are secrets. Avoid debug
tunnel logging that records headers.

A private VPN/subnet router is another option. If you travel with this PC,
another always-on host must remain at home. Internet tunneling carries
controls/status, **not Bluetooth music**; the music source must remain near
the ESP32.

## Local tests and diagnostics

```powershell
python tools/run_local_tests.py
python tools/compile_firmware.py
python tools/local_server.py
python tools/capture_serial.py --port COM11 --seconds 30
```

The simulator is loopback-only at `http://localhost:8765` and clearly labeled
**LOCAL SIMULATION**. It requires no login and does not operate hardware.
Never publish it.

Close Serial Monitor/Plotter before tools use COM11. `capture_serial.py --reset`
reboots the board without erasing settings. Runtime logs report heap, maximum
allocation block, connection events, and allocation failures—not passwords.

If Wi-Fi remains unreliable, improve antenna/router placement. The observed
router RSSI was about **−79 dBm**, a weak signal.

See [LOCAL_TEST_RESULTS.md](LOCAL_TEST_RESULTS.md) for software/hardware scope.

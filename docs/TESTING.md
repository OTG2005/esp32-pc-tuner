# Bluetooth-only testing branch

**Branch:** `testing/bluetooth-only-lights`

**Firmware marker:** `testing-bluetooth-only-1`

This is a testing candidate, not an approved `main` release.

## Release gate

- Commit and push this work only to the testing branch.
- Do not merge, fast-forward, cherry-pick, or push these changes to `main`
  until the project owner explicitly confirms that everything works and
  authorizes promotion.
- Passing a local compile/test suite alone is not that confirmation.
- Do not force-push either branch.
- Never commit `LocalConfig.h`, `private/`, `.local/`, logs, ELF/map files,
  or firmware images containing credentials.

## Change under test

- Keep the current original ESP32 board.
- Remove microphone pins, I2S1 RX initialization, sampling buffers, input
  reads, ambient noise-floor processing, and microphone fallback.
- Use only A2DP decoded PCM for sound-reactive lighting.
- At 300 ms without fresh Bluetooth PCM, reset meter/spectrum history and
  clear music effects. Static/non-music effects retain their normal behavior.
- Remove microphone/automatic-source selectors. `source bluetooth` is the
  only accepted source command.
- Retain Classic A2DP, BLE UART, IR, the small no-login page, open setup AP,
  NVS router credentials, and external authentication for Internet access.

## Local verification

```powershell
python tools/run_local_tests.py
python tools/compile_firmware.py
git diff --check
```

Use ESP32 Dev Module, 240 MHz, 4 MB, Huge APP, PSRAM off. Keep the entire
`sketch_may4a` folder together. Compiling does not upload.

## Hardware acceptance checklist

Power off before changing circuit wiring. The microphone is no longer required;
GPIO32/33/35 are unused.

- [ ] Cold boot with the full circuit attached, no resets or allocation failures.
- [ ] Bluetooth Classic music plays through MAX98357A.
- [ ] Spectrum, pulse, wave, and sparkle respond to actual Bluetooth playback.
- [ ] Pausing/disconnecting music clears those music effects within 300 ms,
      even with speech, claps, or other ambient sound near the old microphone.
- [ ] Resuming music restarts lighting without stale spectrum/beat peaks.
- [ ] Volume, mute/unmute, AVRCP playback/track commands, and mono output work.
- [ ] BLE UART connects, returns replies, and accepts fragmented commands.
- [ ] Original IR colors/effects/brightness/sensitivity remain supported.
- [ ] Plain web buttons and custom values work; no sign-in page returns.
- [ ] Open setup AP accepts router details and disappears once a router IP is obtained.
- [ ] Saved router credentials survive reboot; recovery AP returns on failure.
- [ ] Extended A2DP + BLE + web use is stable with the full circuit and supply.
- [ ] If needed, Internet access works through an email/Access/VPN-protected
      home relay; no direct public port-80 exposure.

Record the results and explicitly tell the assistant when the candidate is
approved for `main`. Until then, leave `main` unchanged.

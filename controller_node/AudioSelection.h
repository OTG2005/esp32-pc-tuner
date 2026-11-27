#pragma once
#include <stdint.h>

constexpr uint32_t BLUETOOTH_PCM_TIMEOUT_MS = 300;

// Shared by firmware and native tests, including millis() wraparound.
inline bool hasFreshBluetoothAudio(uint32_t now, uint32_t sequence,
                                  uint32_t timestamp) {
  return sequence != 0 &&
      static_cast<uint32_t>(now - timestamp) < BLUETOOTH_PCM_TIMEOUT_MS;
}

#pragma once

// Portable sound-reactive 8x8 RGB frame generator running on the Controller.
// Produces logical (x, y) RGB24 frames (y=0 is bottom row) to stream over
// Wi-Fi UDP to the LED Node; zero NeoPixel hardware dependency.
#include "ControlProtocol.h"
#include "NodeProtocol.h"
#include <stdint.h>

struct DisplayAudioLevels {
  float overall = 0.0f;
  float bass = 0.0f;
  float mid = 0.0f;
  float treble = 0.0f;
  float beat = 0.0f;
  bool active = false;
};

class AudioEffectRenderer {
 public:
  void reset();
  void seed(uint32_t value);

  // Renders the active sound-reactive mode into payload.rgb (192 bytes).
  // Returns true if an active visual frame was rendered, false if cleared/idle.
  bool render(uint8_t mode, uint8_t brightness, const DisplayAudioLevels &audio,
              node_protocol::LedFramePayload &outFrame);

 private:
  void clearBuffer();
  void fillBuffer(uint32_t rgb);
  void fadeBuffer(uint8_t amount);
  void setPixel(uint8_t x, uint8_t y, uint32_t rgb);
  void setIndex(uint16_t index, uint32_t rgb);
  uint32_t nextRandom();

  void renderSparkle(const DisplayAudioLevels &audio);
  void renderPulse(const DisplayAudioLevels &audio);
  void renderWave(const DisplayAudioLevels &audio);
  void renderSpectrum(const DisplayAudioLevels &audio);

  uint8_t frameRgb_[node_protocol::FRAME_RGB_BYTES] = {};
  float spectrumHeights_[node_protocol::MATRIX_WIDTH] = {};
  uint8_t animationPhase_ = 0;
  uint32_t rngState_ = 0x13579BDFU;
};

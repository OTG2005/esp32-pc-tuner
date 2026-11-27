#include "AudioEffects.h"
#include <math.h>
#include <string.h>

namespace {

float clamp01(float value) {
  if (value < 0.0f) return 0.0f;
  if (value > 1.0f) return 1.0f;
  return value;
}

uint32_t makeRgb(uint8_t r, uint8_t g, uint8_t b) {
  return (static_cast<uint32_t>(r) << 16) |
         (static_cast<uint32_t>(g) << 8) |
         static_cast<uint32_t>(b);
}

uint32_t scaleRgb(uint32_t rgb, float scale) {
  scale = clamp01(scale);
  const uint8_t r = static_cast<uint8_t>(((rgb >> 16) & 0xFFU) * scale);
  const uint8_t g = static_cast<uint8_t>(((rgb >> 8) & 0xFFU) * scale);
  const uint8_t b = static_cast<uint8_t>((rgb & 0xFFU) * scale);
  return makeRgb(r, g, b);
}

uint32_t colorWheel(uint8_t position) {
  position = static_cast<uint8_t>(255U - position);
  if (position < 85U) {
    return makeRgb(static_cast<uint8_t>(255U - position * 3U), 0,
                   static_cast<uint8_t>(position * 3U));
  }
  if (position < 170U) {
    position = static_cast<uint8_t>(position - 85U);
    return makeRgb(0, static_cast<uint8_t>(position * 3U),
                   static_cast<uint8_t>(255U - position * 3U));
  }
  position = static_cast<uint8_t>(position - 170U);
  return makeRgb(static_cast<uint8_t>(position * 3U),
                 static_cast<uint8_t>(255U - position * 3U), 0);
}

} // namespace

void AudioEffectRenderer::reset() {
  clearBuffer();
  memset(spectrumHeights_, 0, sizeof(spectrumHeights_));
  animationPhase_ = 0;
}

void AudioEffectRenderer::seed(uint32_t value) {
  rngState_ = value ? value : 0x13579BDFU;
}

void AudioEffectRenderer::clearBuffer() {
  memset(frameRgb_, 0, sizeof(frameRgb_));
}

void AudioEffectRenderer::fillBuffer(uint32_t rgb) {
  const uint8_t r = static_cast<uint8_t>((rgb >> 16) & 0xFFU);
  const uint8_t g = static_cast<uint8_t>((rgb >> 8) & 0xFFU);
  const uint8_t b = static_cast<uint8_t>(rgb & 0xFFU);
  for (uint16_t i = 0; i < node_protocol::PIXEL_COUNT; ++i) {
    frameRgb_[i * 3U + 0U] = r;
    frameRgb_[i * 3U + 1U] = g;
    frameRgb_[i * 3U + 2U] = b;
  }
}

void AudioEffectRenderer::fadeBuffer(uint8_t amount) {
  for (size_t i = 0; i < node_protocol::FRAME_RGB_BYTES; ++i) {
    frameRgb_[i] = static_cast<uint8_t>((static_cast<uint16_t>(frameRgb_[i]) * amount) / 255U);
  }
}

void AudioEffectRenderer::setIndex(uint16_t index, uint32_t rgb) {
  if (index >= node_protocol::PIXEL_COUNT) return;
  frameRgb_[index * 3U + 0U] = static_cast<uint8_t>((rgb >> 16) & 0xFFU);
  frameRgb_[index * 3U + 1U] = static_cast<uint8_t>((rgb >> 8) & 0xFFU);
  frameRgb_[index * 3U + 2U] = static_cast<uint8_t>(rgb & 0xFFU);
}

void AudioEffectRenderer::setPixel(uint8_t x, uint8_t y, uint32_t rgb) {
  if (x >= node_protocol::MATRIX_WIDTH || y >= node_protocol::MATRIX_HEIGHT) return;
  setIndex(static_cast<uint16_t>(y) * node_protocol::MATRIX_WIDTH + x, rgb);
}

uint32_t AudioEffectRenderer::nextRandom() {
  // Xorshift32: fast, portable across ESP32 and host C++ tests.
  uint32_t x = rngState_;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  rngState_ = x ? x : 0x13579BDFU;
  return rngState_;
}

void AudioEffectRenderer::renderSparkle(const DisplayAudioLevels &audio) {
  fadeBuffer(188);
  const uint8_t sparkCount = static_cast<uint8_t>(clamp01(audio.overall) * 10.0f);
  for (uint8_t spark = 0; spark < sparkCount; ++spark) {
    const uint16_t pixel = static_cast<uint16_t>(nextRandom() % node_protocol::PIXEL_COUNT);
    const uint8_t hue = static_cast<uint8_t>(animationPhase_ + spark * 37U);
    const float strength = 0.25f + audio.overall * 0.55f + audio.beat * 0.20f;
    setIndex(pixel, scaleRgb(colorWheel(hue), strength));
  }
  animationPhase_ = static_cast<uint8_t>(animationPhase_ + 5U);
}

void AudioEffectRenderer::renderPulse(const DisplayAudioLevels &audio) {
  animationPhase_ = static_cast<uint8_t>(animationPhase_ + (audio.beat > 0.6f ? 13U : 2U));
  const float intensity = clamp01(audio.overall * 0.88f + audio.beat * 0.22f);
  fillBuffer(scaleRgb(colorWheel(animationPhase_), intensity));
}

void AudioEffectRenderer::renderWave(const DisplayAudioLevels &audio) {
  animationPhase_ = static_cast<uint8_t>(animationPhase_ + 3U);
  clearBuffer();

  const float levelByRow[node_protocol::MATRIX_HEIGHT] = {
      audio.bass,   audio.bass, audio.mid,
      audio.mid,    audio.mid,  audio.treble,
      audio.treble, audio.overall};

  for (uint8_t y = 0; y < node_protocol::MATRIX_HEIGHT; ++y) {
    const float width = clamp01(levelByRow[y]) * node_protocol::MATRIX_WIDTH;
    for (uint8_t x = 0; x < node_protocol::MATRIX_WIDTH; ++x) {
      const float distance = fabsf((static_cast<float>(x) + 0.5f) - 4.0f);
      if (distance < width * 0.5f) {
        float intensity = 0.32f + levelByRow[y] * 0.68f;
        if (audio.beat > 0.65f) intensity = 1.0f;
        setPixel(x, y, scaleRgb(colorWheel(static_cast<uint8_t>(animationPhase_ + y * 23U)),
                                intensity));
      }
    }
  }
}

void AudioEffectRenderer::renderSpectrum(const DisplayAudioLevels &audio) {
  animationPhase_ = static_cast<uint8_t>(animationPhase_ + (audio.beat > 0.55f ? 6U : 1U));

  const float targets[node_protocol::MATRIX_WIDTH] = {
      audio.bass * 0.82f,
      audio.bass,
      audio.bass * 0.55f + audio.mid * 0.45f,
      audio.mid,
      audio.mid * 0.85f,
      audio.mid * 0.35f + audio.treble * 0.65f,
      audio.treble,
      audio.treble * 0.80f};

  clearBuffer();
  for (uint8_t x = 0; x < node_protocol::MATRIX_WIDTH; ++x) {
    if (targets[x] > spectrumHeights_[x]) {
      spectrumHeights_[x] += (targets[x] - spectrumHeights_[x]) * 0.70f;
    } else {
      spectrumHeights_[x] *= 0.82f;
    }

    int rawHeight = static_cast<int>(spectrumHeights_[x] * 9.0f);
    if (rawHeight < 0) rawHeight = 0;
    if (rawHeight > static_cast<int>(node_protocol::MATRIX_HEIGHT)) {
      rawHeight = static_cast<int>(node_protocol::MATRIX_HEIGHT);
    }
    const uint8_t height = static_cast<uint8_t>(rawHeight);

    for (uint8_t y = 0; y < height; ++y) {
      uint32_t color = colorWheel(static_cast<uint8_t>(animationPhase_ + x * 12U + y * 18U));
      float intensity = 0.38f + spectrumHeights_[x] * 0.62f;
      if (audio.beat > 0.72f && y == height - 1U) {
        color = makeRgb(255, 255, 255);
        intensity = 1.0f;
      }
      setPixel(x, y, scaleRgb(color, intensity));
    }
  }
}

bool AudioEffectRenderer::render(uint8_t mode, uint8_t brightness,
                                 const DisplayAudioLevels &audio,
                                 node_protocol::LedFramePayload &outFrame) {
  outFrame.mode = mode;
  outFrame.brightness = brightness;

  if (!audio.active || !node_protocol::isAudioEffectMode(mode)) {
    clearBuffer();
    memset(spectrumHeights_, 0, sizeof(spectrumHeights_));
    outFrame.frameFlags = node_protocol::FRAME_FLAG_CLEAR;
    memset(outFrame.rgb, 0, sizeof(outFrame.rgb));
    return false;
  }

  switch (mode) {
    case MODE_AUDIO_SPARKLE:
      renderSparkle(audio);
      break;
    case MODE_AUDIO_PULSE:
      renderPulse(audio);
      break;
    case MODE_AUDIO_WAVE:
      renderWave(audio);
      break;
    case MODE_AUDIO_SPECTRUM:
      renderSpectrum(audio);
      break;
    default:
      clearBuffer();
      outFrame.frameFlags = node_protocol::FRAME_FLAG_CLEAR;
      memset(outFrame.rgb, 0, sizeof(outFrame.rgb));
      return false;
  }

  outFrame.frameFlags = node_protocol::FRAME_FLAG_ACTIVE;
  memcpy(outFrame.rgb, frameRgb_, sizeof(outFrame.rgb));
  return true;
}

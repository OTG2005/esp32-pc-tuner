/*
  OTL PC Tuner - ESP32 Bluetooth speaker and sound-reactive 8x8 matrix

  Audio hardware uses both ESP32 I2S controllers so input and output can run
  at the same time:
    I2S0 TX -> MAX98357A: BCLK=26, LRC=25, DIN=22
    I2S1 RX <- I2S mic:   BCLK=33, WS=32,  SD=35 (left channel)

  Bluetooth Classic A2DP carries music to the speaker. BLE remains available
  for light controls by starting the radio in BTDM (Classic + BLE) mode.
  Music modes analyze the decoded A2DP PCM that is actually sent to the amp;
  when Bluetooth audio is paused, they automatically fall back to the mic.
*/

#include <Arduino.h>
#include <Adafruit_NeoPixel.h>
#include <IRremote.hpp>

// AudioTools must be included before ESP32-A2DP 1.8.x.
#include "AudioTools.h"
#include "BluetoothA2DPSink.h"

#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

// ------------------------------- Hardware ---------------------------------

constexpr uint8_t IR_PIN = 34;

constexpr uint8_t LED_PIN = 4;
constexpr uint8_t MATRIX_WIDTH = 8;
constexpr uint8_t MATRIX_HEIGHT = 8;
constexpr uint16_t PIXEL_COUNT = MATRIX_WIDTH * MATRIX_HEIGHT;

// Most 8x8 WS2812 panels use serpentine rows. Change to false for progressive
// row wiring. Y=0 is treated as the bottom of the matrix by the effects.
constexpr bool MATRIX_SERPENTINE = true;

constexpr int SPEAKER_I2S_PORT = 0;
constexpr int SPEAKER_BCLK_PIN = 26;
constexpr int SPEAKER_WS_PIN = 25;
constexpr int SPEAKER_DATA_PIN = 22;

constexpr int MIC_I2S_PORT = 1;
constexpr int MIC_BCLK_PIN = 33;
constexpr int MIC_WS_PIN = 32;
constexpr int MIC_DATA_PIN = 35;
constexpr uint32_t MIC_SAMPLE_RATE = 32000;

constexpr char BLUETOOTH_NAME[] = "OTL PC Tuner";

// Nordic UART Service used by the existing phone controller.
constexpr char SERVICE_UUID[] = "6E400001-B5A3-F393-E0A9-E50E24DCCA9E";
constexpr char RX_UUID[] = "6E400002-B5A3-F393-E0A9-E50E24DCCA9E";
constexpr char TX_UUID[] = "6E400003-B5A3-F393-E0A9-E50E24DCCA9E";

// ----------------------------- IR commands --------------------------------

#define BTN_ON          0x40
#define BTN_OFF         0x41
#define BTN_V_DOWN      0x5D
#define BTN_V_UP        0x5C
#define BTN_W           0x44
#define BTN_B           0x45
#define BTN_G           0x59
#define BTN_R           0x58
#define BTN_PINK_1      0x48
#define BTN_PINK_2      0x4C
#define BTN_CYAN_1      0x1F
#define BTN_CYAN_2      0x1B
#define BTN_BLUE_2      0x49
#define BTN_PURPLE_1    0x4D
#define BTN_PURPLE_2    0x1E
#define BTN_PURPLE_3    0x1A
#define BTN_ORANGE_1    0x54
#define BTN_ORANGE_2    0x50
#define BTN_ORANGE_3    0x1C
#define BTN_YELLOW      0x18
#define BTN_LT_GREEN_1  0x55
#define BTN_LT_BLUE_1   0x51
#define BTN_LT_BLUE_2   0x1D
#define BTN_GREEN_BLUE  0x19
#define BTN_FLASH       0x0A
#define BTN_1H          0x14
#define BTN_2H          0x10
#define BTN_3H          0x0C
#define BTN_4H          0x08
#define BTN_DIM_1       0x17
#define BTN_DIM_2       0x13
#define BTN_DIM_3       0x0F
#define BTN_DIM_4       0x0B
#define BTN_JUMP3       0x15
#define BTN_JUMP7       0x11
#define BTN_FADE3       0x0D
#define BTN_FADE7       0x09
#define BTN_S4          0x07
#define BTN_S3          0x06
#define BTN_S2          0x05
#define BTN_S1          0x04

enum LightMode : uint8_t {
  MODE_STATIC = 1,
  MODE_RAINBOW = 5,
  MODE_BLINK = 6,
  MODE_COLOR_BLEND = 7,
  MODE_FADE_3 = 8,
  MODE_FADE_7 = 9,
  MODE_AUDIO_SPARKLE = 10,
  MODE_AUDIO_PULSE = 11,
  MODE_AUDIO_WAVE = 12,
  MODE_AUDIO_SPECTRUM = 13
};

// ------------------------------ Peripherals --------------------------------

Adafruit_NeoPixel strip(PIXEL_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);

I2SStream speakerOutput;
I2SStream microphoneInput;
BluetoothA2DPSink a2dpSink(speakerOutput);

BLEServer *bleServer = nullptr;
BLECharacteristic *bleTxCharacteristic = nullptr;
volatile bool bleConnected = false;
volatile char pendingBleCommand = '\0';

// ----------------------------- Light state ---------------------------------

uint8_t brightness = 192;
uint8_t currentLightMode = MODE_RAINBOW;
uint8_t savedLightMode = MODE_RAINBOW;
bool lightsOff = false;
uint32_t staticColor = 0;

uint32_t animationTimestamp = 0;
uint8_t animationPhase = 0;

// ----------------------------- Audio meters --------------------------------

struct RawAudioLevels {
  uint32_t average;
  uint32_t peak;
  uint32_t bass;
  uint32_t mid;
  uint32_t treble;
  uint32_t sequence;
  uint32_t timestampMs;
};

struct DisplayAudioLevels {
  float overall;
  float bass;
  float mid;
  float treble;
  float beat;
  bool fromBluetooth;
};

portMUX_TYPE audioMux = portMUX_INITIALIZER_UNLOCKED;
RawAudioLevels bluetoothLevels = {};
RawAudioLevels microphoneLevels = {};
DisplayAudioLevels displayAudio = {};

int32_t microphoneBuffer[256];
bool microphoneReady = false;

float audioNoiseFloor = 0.0f;
float audioDynamicRange = 1.0f;
float audioFastEnvelope = 0.0f;
float audioSlowEnvelope = 0.0f;
float audioSensitivity = 1.0f;
uint32_t lastAudioSequence = 0;
uint32_t lastBeatMs = 0;
bool lastAudioSourceWasBluetooth = false;
bool audioSourceInitialized = false;

// ----------------------------- Small helpers -------------------------------

float clamp01(float value) {
  if (value < 0.0f) return 0.0f;
  if (value > 1.0f) return 1.0f;
  return value;
}

uint16_t matrixIndex(uint8_t x, uint8_t y) {
  if (x >= MATRIX_WIDTH || y >= MATRIX_HEIGHT) return 0;
  if (MATRIX_SERPENTINE && (y & 1U)) {
    x = MATRIX_WIDTH - 1U - x;
  }
  return static_cast<uint16_t>(y) * MATRIX_WIDTH + x;
}

void setMatrixPixel(uint8_t x, uint8_t y, uint32_t color) {
  strip.setPixelColor(matrixIndex(x, y), color);
}

uint32_t scaleColor(uint32_t color, float scale) {
  scale = clamp01(scale);
  const uint8_t red = static_cast<uint8_t>(((color >> 16) & 0xFFU) * scale);
  const uint8_t green = static_cast<uint8_t>(((color >> 8) & 0xFFU) * scale);
  const uint8_t blue = static_cast<uint8_t>((color & 0xFFU) * scale);
  return strip.Color(red, green, blue);
}

uint32_t colorWheel(uint8_t position) {
  position = 255U - position;
  if (position < 85U) {
    return strip.Color(255U - position * 3U, 0, position * 3U);
  }
  if (position < 170U) {
    position -= 85U;
    return strip.Color(0, position * 3U, 255U - position * 3U);
  }
  position -= 170U;
  return strip.Color(position * 3U, 255U - position * 3U, 0);
}

void fillStrip(uint32_t color, bool showNow = true) {
  strip.fill(color, 0, PIXEL_COUNT);
  if (showNow) strip.show();
}

void fadeExistingPixels(uint8_t amount) {
  for (uint16_t index = 0; index < PIXEL_COUNT; ++index) {
    const uint32_t color = strip.getPixelColor(index);
    const uint8_t red = ((color >> 16) & 0xFFU) * amount / 255U;
    const uint8_t green = ((color >> 8) & 0xFFU) * amount / 255U;
    const uint8_t blue = (color & 0xFFU) * amount / 255U;
    strip.setPixelColor(index, strip.Color(red, green, blue));
  }
}

void setBrightnessValue(int value) {
  brightness = static_cast<uint8_t>(constrain(value, 15, 255));
  strip.setBrightness(brightness);
  strip.show();
}

void setStaticColor(uint32_t color) {
  staticColor = color;
  currentLightMode = MODE_STATIC;
  if (!lightsOff) fillStrip(staticColor);
}

void setLightsPower(bool enabled) {
  if (enabled) {
    if (!lightsOff) return;
    lightsOff = false;
    currentLightMode = savedLightMode;
    strip.setBrightness(brightness);
    if (currentLightMode == MODE_STATIC) fillStrip(staticColor);
    animationTimestamp = 0;
  } else {
    if (lightsOff) return;
    savedLightMode = currentLightMode;
    lightsOff = true;
    fillStrip(0);
  }
}

// ----------------------------- Audio capture -------------------------------

uint32_t absolute32(int32_t value) {
  if (value >= 0) return static_cast<uint32_t>(value);
  return static_cast<uint32_t>(-(static_cast<int64_t>(value)));
}

RawAudioLevels analyzeMonoSamples(const int32_t *samples, size_t count,
                                  int32_t &lowState, int32_t &midState,
                                  uint32_t sequence, uint32_t timestampMs) {
  RawAudioLevels result = {};
  if (samples == nullptr || count == 0) return result;

  uint64_t averageSum = 0;
  uint64_t bassSum = 0;
  uint64_t midSum = 0;
  uint64_t trebleSum = 0;
  uint32_t peak = 0;

  for (size_t index = 0; index < count; ++index) {
    const int32_t sample = samples[index];
    // Two inexpensive one-pole filters separate approximate bass/mid/treble
    // energy without running an FFT in the Bluetooth callback.
    lowState += (sample - lowState) / 64;
    midState += (sample - midState) / 8;

    const uint32_t magnitude = absolute32(sample);
    const uint32_t bassMagnitude = absolute32(lowState);
    const uint32_t midMagnitude = absolute32(midState - lowState);
    const uint32_t trebleMagnitude = absolute32(sample - midState);

    averageSum += magnitude;
    bassSum += bassMagnitude;
    midSum += midMagnitude;
    trebleSum += trebleMagnitude;
    if (magnitude > peak) peak = magnitude;
  }

  result.average = static_cast<uint32_t>(averageSum / count);
  result.peak = peak;
  result.bass = static_cast<uint32_t>(bassSum / count);
  result.mid = static_cast<uint32_t>(midSum / count);
  result.treble = static_cast<uint32_t>(trebleSum / count);
  result.sequence = sequence;
  result.timestampMs = timestampMs;
  return result;
}

void onBluetoothAudio(const uint8_t *data, uint32_t length) {
  if (data == nullptr || length < 4U) return;

  const int16_t *stereo = reinterpret_cast<const int16_t *>(data);
  const size_t frameCount = length / (sizeof(int16_t) * 2U);
  if (frameCount == 0) return;

  // A2DP delivers interleaved signed 16-bit stereo PCM. Analyze a bounded
  // chunk so this callback never delays the I2S speaker output.
  constexpr size_t MAX_ANALYSIS_FRAMES = 512;
  const size_t step = frameCount > MAX_ANALYSIS_FRAMES
                          ? (frameCount + MAX_ANALYSIS_FRAMES - 1U) /
                                MAX_ANALYSIS_FRAMES
                          : 1U;

  // Static storage avoids placing a 2 KB array on the Bluetooth task stack.
  static int32_t mono[MAX_ANALYSIS_FRAMES];
  size_t monoCount = 0;
  for (size_t frame = 0; frame < frameCount && monoCount < MAX_ANALYSIS_FRAMES;
       frame += step) {
    const int32_t left = stereo[frame * 2U];
    const int32_t right = stereo[frame * 2U + 1U];
    mono[monoCount++] = (left + right) / 2;
  }

  static int32_t lowState = 0;
  static int32_t midState = 0;
  static uint32_t sequence = 0;
  const uint32_t now = millis();
  const RawAudioLevels levels =
      analyzeMonoSamples(mono, monoCount, lowState, midState, ++sequence, now);

  portENTER_CRITICAL(&audioMux);
  bluetoothLevels = levels;
  portEXIT_CRITICAL(&audioMux);
}

void readMicrophone() {
  if (!microphoneReady) return;

  const size_t bytesRead = microphoneInput.readBytes(
      reinterpret_cast<uint8_t *>(microphoneBuffer), sizeof(microphoneBuffer));
  const size_t sampleCount = bytesRead / sizeof(microphoneBuffer[0]);
  if (sampleCount == 0) return;

  // Standard I2S microphones place their 24-bit sample in a 32-bit word.
  // Shift to a manageable signed range while retaining useful resolution.
  for (size_t index = 0; index < sampleCount; ++index) {
    microphoneBuffer[index] >>= 8;
  }

  static int32_t lowState = 0;
  static int32_t midState = 0;
  static uint32_t sequence = 0;
  microphoneLevels = analyzeMonoSamples(microphoneBuffer, sampleCount, lowState,
                                        midState, ++sequence, millis());
}

RawAudioLevels latestAudioLevels(bool &fromBluetooth) {
  RawAudioLevels bluetoothCopy;
  portENTER_CRITICAL(&audioMux);
  bluetoothCopy = bluetoothLevels;
  portEXIT_CRITICAL(&audioMux);

  const uint32_t now = millis();
  fromBluetooth = bluetoothCopy.timestampMs != 0U &&
                  static_cast<uint32_t>(now - bluetoothCopy.timestampMs) < 300U;
  return fromBluetooth ? bluetoothCopy : microphoneLevels;
}

void updateDisplayAudio() {
  bool fromBluetooth = false;
  const RawAudioLevels raw = latestAudioLevels(fromBluetooth);

  if (raw.sequence == 0U ||
      (raw.sequence == lastAudioSequence &&
       fromBluetooth == lastAudioSourceWasBluetooth)) {
    displayAudio.overall *= 0.92f;
    displayAudio.bass *= 0.91f;
    displayAudio.mid *= 0.91f;
    displayAudio.treble *= 0.91f;
    displayAudio.beat *= 0.72f;
    return;
  }

  if (!audioSourceInitialized ||
      fromBluetooth != lastAudioSourceWasBluetooth) {
    audioNoiseFloor = fromBluetooth ? 0.0f : raw.average * 0.55f;
    audioDynamicRange = raw.average > 1U ? raw.average * 0.45f : 1.0f;
    audioFastEnvelope = 0.0f;
    audioSlowEnvelope = 0.0f;
    displayAudio = {};
    audioSourceInitialized = true;
  }

  lastAudioSourceWasBluetooth = fromBluetooth;
  lastAudioSequence = raw.sequence;
  displayAudio.fromBluetooth = fromBluetooth;

  const float average = static_cast<float>(raw.average);
  if (fromBluetooth) {
    audioNoiseFloor = 0.0f;
  } else if (average < audioNoiseFloor) {
    audioNoiseFloor += (average - audioNoiseFloor) * 0.12f;
  } else if (average < audioNoiseFloor * 1.5f + 1.0f) {
    audioNoiseFloor += (average - audioNoiseFloor) * 0.004f;
  } else {
    // Do not let sustained music rapidly become the microphone noise floor.
    audioNoiseFloor += (average - audioNoiseFloor) * 0.0002f;
  }

  float signal = average - audioNoiseFloor * (fromBluetooth ? 1.0f : 1.08f);
  if (signal < 0.0f) signal = 0.0f;

  const float minimumRange =
      fromBluetooth ? 180.0f : (audioNoiseFloor * 0.10f + 100.0f);
  audioDynamicRange *= 0.992f;
  if (audioDynamicRange < minimumRange) audioDynamicRange = minimumRange;
  if (signal > audioDynamicRange) audioDynamicRange = signal;

  const float target = clamp01((signal * audioSensitivity) /
                               (audioDynamicRange * 0.78f + 1.0f));
  const float levelRate = target > displayAudio.overall ? 0.48f : 0.10f;
  displayAudio.overall += (target - displayAudio.overall) * levelRate;

  const float bandTotal =
      static_cast<float>(raw.bass + raw.mid + raw.treble) + 1.0f;
  const float bandRaw[3] = {static_cast<float>(raw.bass),
                            static_cast<float>(raw.mid),
                            static_cast<float>(raw.treble)};
  float *bandOutput[3] = {&displayAudio.bass, &displayAudio.mid,
                          &displayAudio.treble};
  for (uint8_t band = 0; band < 3; ++band) {
    const float relativeEnergy = bandRaw[band] / bandTotal;
    const float bandTarget =
        clamp01(target * (0.38f + relativeEnergy * 2.15f));
    const float rate = bandTarget > *bandOutput[band] ? 0.55f : 0.14f;
    *bandOutput[band] += (bandTarget - *bandOutput[band]) * rate;
  }

  audioFastEnvelope += (signal - audioFastEnvelope) * 0.38f;
  audioSlowEnvelope += (signal - audioSlowEnvelope) * 0.035f;
  displayAudio.beat *= 0.70f;

  const uint32_t now = millis();
  const bool transient = audioFastEnvelope > audioSlowEnvelope * 1.42f + 1.0f;
  if (target > 0.22f && transient &&
      static_cast<uint32_t>(now - lastBeatMs) > 130U) {
    displayAudio.beat = 1.0f;
    lastBeatMs = now;
  }
}

// ---------------------------- Light animations -----------------------------

void renderRainbow() {
  if (static_cast<uint32_t>(millis() - animationTimestamp) < 40U) return;
  animationTimestamp = millis();
  ++animationPhase;

  for (uint8_t y = 0; y < MATRIX_HEIGHT; ++y) {
    for (uint8_t x = 0; x < MATRIX_WIDTH; ++x) {
      setMatrixPixel(x, y,
                     colorWheel(animationPhase + x * 12U + y * 8U));
    }
  }
  strip.show();
}

void renderBlink() {
  if (static_cast<uint32_t>(millis() - animationTimestamp) < 250U) return;
  animationTimestamp = millis();
  static uint8_t colorIndex = 0;
  const uint32_t colors[] = {strip.Color(255, 0, 0),
                             strip.Color(0, 255, 0),
                             strip.Color(0, 0, 255)};
  fillStrip(colors[colorIndex]);
  colorIndex = (colorIndex + 1U) % 3U;
}

void renderColorBlend() {
  if (static_cast<uint32_t>(millis() - animationTimestamp) < 25U) return;
  animationTimestamp = millis();
  animationPhase += 2U;
  fillStrip(colorWheel(animationPhase));
}

void renderFade(bool sevenColors) {
  if (static_cast<uint32_t>(millis() - animationTimestamp) < 18U) return;
  animationTimestamp = millis();

  static uint8_t fadeValue = 0;
  static int8_t fadeDirection = 3;
  static uint8_t colorIndex = 0;
  const uint32_t colors[] = {
      strip.Color(255, 0, 0),   strip.Color(0, 255, 0),
      strip.Color(0, 0, 255),   strip.Color(255, 255, 0),
      strip.Color(255, 0, 255), strip.Color(0, 255, 255),
      strip.Color(255, 255, 255)};
  const uint8_t colorCount = sevenColors ? 7U : 3U;

  int nextValue = static_cast<int>(fadeValue) + fadeDirection;
  if (nextValue >= 255) {
    nextValue = 255;
    fadeDirection = -3;
  } else if (nextValue <= 0) {
    nextValue = 0;
    fadeDirection = 3;
    colorIndex = (colorIndex + 1U) % colorCount;
  }
  fadeValue = static_cast<uint8_t>(nextValue);

  const float gammaCorrected =
      powf(static_cast<float>(fadeValue) / 255.0f, 2.2f);
  fillStrip(scaleColor(colors[colorIndex], gammaCorrected));
}

void renderAudioSparkle() {
  fadeExistingPixels(188);
  const uint8_t sparkCount =
      1U + static_cast<uint8_t>(displayAudio.overall * 9.0f);
  for (uint8_t spark = 0; spark < sparkCount; ++spark) {
    const uint16_t pixel = random(PIXEL_COUNT);
    const uint8_t hue = animationPhase + spark * 37U;
    const float strength =
        0.25f + displayAudio.overall * 0.55f + displayAudio.beat * 0.20f;
    strip.setPixelColor(pixel, scaleColor(colorWheel(hue), strength));
  }
  animationPhase += 5U;
  strip.show();
}

void renderAudioPulse() {
  animationPhase += displayAudio.beat > 0.6f ? 13U : 2U;
  const float intensity = clamp01(0.035f + displayAudio.overall * 0.88f +
                                  displayAudio.beat * 0.22f);
  fillStrip(scaleColor(colorWheel(animationPhase), intensity));
}

void renderAudioWave() {
  animationPhase += 3U;
  strip.clear();

  const float levelByRow[MATRIX_HEIGHT] = {
      displayAudio.bass,   displayAudio.bass, displayAudio.mid,
      displayAudio.mid,    displayAudio.mid,  displayAudio.treble,
      displayAudio.treble, displayAudio.overall};

  for (uint8_t y = 0; y < MATRIX_HEIGHT; ++y) {
    const float width = levelByRow[y] * MATRIX_WIDTH;
    for (uint8_t x = 0; x < MATRIX_WIDTH; ++x) {
      const float distance = fabsf((static_cast<float>(x) + 0.5f) - 4.0f);
      if (distance < width * 0.5f) {
        float intensity = 0.32f + levelByRow[y] * 0.68f;
        if (displayAudio.beat > 0.65f) intensity = 1.0f;
        setMatrixPixel(x, y,
                       scaleColor(colorWheel(animationPhase + y * 23U),
                                  intensity));
      }
    }
  }
  strip.show();
}

void renderAudioSpectrum() {
  static float columnHeight[MATRIX_WIDTH] = {};
  animationPhase += displayAudio.beat > 0.55f ? 6U : 1U;

  const float targets[MATRIX_WIDTH] = {
      displayAudio.bass * 0.82f,
      displayAudio.bass,
      displayAudio.bass * 0.55f + displayAudio.mid * 0.45f,
      displayAudio.mid,
      displayAudio.mid * 0.85f,
      displayAudio.mid * 0.35f + displayAudio.treble * 0.65f,
      displayAudio.treble,
      displayAudio.treble * 0.80f};

  strip.clear();
  for (uint8_t x = 0; x < MATRIX_WIDTH; ++x) {
    if (targets[x] > columnHeight[x]) {
      columnHeight[x] += (targets[x] - columnHeight[x]) * 0.70f;
    } else {
      columnHeight[x] *= 0.82f;
    }

    const uint8_t height = static_cast<uint8_t>(
        constrain(static_cast<int>(columnHeight[x] * 9.0f), 0,
                  static_cast<int>(MATRIX_HEIGHT)));
    for (uint8_t y = 0; y < height; ++y) {
      uint32_t color = colorWheel(animationPhase + x * 12U + y * 18U);
      float intensity = 0.38f + columnHeight[x] * 0.62f;
      if (displayAudio.beat > 0.72f && y == height - 1U) {
        color = strip.Color(255, 255, 255);
        intensity = 1.0f;
      }
      setMatrixPixel(x, y, scaleColor(color, intensity));
    }
  }
  strip.show();
}

void updateLights() {
  if (lightsOff || currentLightMode == MODE_STATIC) return;

  const bool audioMode = currentLightMode >= MODE_AUDIO_SPARKLE;
  const uint32_t interval = audioMode ? 30U : 0U;
  if (interval != 0U &&
      static_cast<uint32_t>(millis() - animationTimestamp) < interval) {
    return;
  }
  if (audioMode) {
    animationTimestamp = millis();
    updateDisplayAudio();
  }

  switch (currentLightMode) {
    case MODE_RAINBOW:
      renderRainbow();
      break;
    case MODE_BLINK:
      renderBlink();
      break;
    case MODE_COLOR_BLEND:
      renderColorBlend();
      break;
    case MODE_FADE_3:
      renderFade(false);
      break;
    case MODE_FADE_7:
      renderFade(true);
      break;
    case MODE_AUDIO_SPARKLE:
      renderAudioSparkle();
      break;
    case MODE_AUDIO_PULSE:
      renderAudioPulse();
      break;
    case MODE_AUDIO_WAVE:
      renderAudioWave();
      break;
    case MODE_AUDIO_SPECTRUM:
      renderAudioSpectrum();
      break;
    default:
      currentLightMode = MODE_RAINBOW;
      break;
  }
}

// ----------------------------- BLE controls --------------------------------

class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer *server) override {
    (void)server;
    bleConnected = true;
  }

  void onDisconnect(BLEServer *server) override {
    bleConnected = false;
    server->startAdvertising();
  }
};

class CommandCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *characteristic) override {
    // `auto` works with std::string in ESP32 core 2.x and Arduino String in
    // core 3.x, whose BLE APIs changed return type.
    const auto value = characteristic->getValue();
    if (value.length() > 0U) pendingBleCommand = value[0];
  }
};

void setupBLE() {
  BLEDevice::init(BLUETOOTH_NAME);
  bleServer = BLEDevice::createServer();
  bleServer->setCallbacks(new ServerCallbacks());

  BLEService *service = bleServer->createService(SERVICE_UUID);
  bleTxCharacteristic = service->createCharacteristic(
      TX_UUID, BLECharacteristic::PROPERTY_NOTIFY);
  bleTxCharacteristic->addDescriptor(new BLE2902());

  BLECharacteristic *rxCharacteristic = service->createCharacteristic(
      RX_UUID, BLECharacteristic::PROPERTY_WRITE);
  rxCharacteristic->setCallbacks(new CommandCallbacks());

  service->start();
  bleServer->getAdvertising()->addServiceUUID(SERVICE_UUID);
  bleServer->startAdvertising();
}

void processBleCommand(char command) {
  switch (command) {
    case 'N':
      setLightsPower(true);
      break;
    case 'F':
      setLightsPower(false);
      break;
    case 'U':
      setBrightnessValue(brightness + 20);
      break;
    case 'D':
      setBrightnessValue(brightness - 20);
      break;
    case 'W':
      setStaticColor(strip.Color(255, 255, 255));
      break;
    case 'R':
      setStaticColor(strip.Color(255, 0, 0));
      break;
    case 'G':
      setStaticColor(strip.Color(0, 255, 0));
      break;
    case 'B':
      setStaticColor(strip.Color(0, 0, 255));
      break;
    case 'C':
      currentLightMode = MODE_RAINBOW;
      break;
    case 'J':
      currentLightMode = MODE_BLINK;
      break;
    case 'K':
      currentLightMode = MODE_COLOR_BLEND;
      break;
    case 'A':
      currentLightMode = MODE_FADE_7;
      break;
    case '1':
      currentLightMode = MODE_AUDIO_SPARKLE;
      break;
    case '2':
      currentLightMode = MODE_AUDIO_PULSE;
      break;
    case '3':
      currentLightMode = MODE_AUDIO_WAVE;
      break;
    case 'S':
      currentLightMode = MODE_AUDIO_SPECTRUM;
      break;
    default:
      break;
  }
}

void checkBleInput() {
  const char command = pendingBleCommand;
  if (command == '\0') return;
  pendingBleCommand = '\0';
  processBleCommand(command);
}

// ------------------------------ IR controls --------------------------------

void selectMode(uint8_t mode) {
  if (lightsOff) return;
  currentLightMode = mode;
  animationTimestamp = 0;
}

void processIrCommand(uint16_t command, bool isRepeat) {
  if (isRepeat && command != BTN_V_UP && command != BTN_V_DOWN) return;

  switch (command) {
    case BTN_ON:
      setLightsPower(true);
      break;
    case BTN_OFF:
      setLightsPower(false);
      break;
    case BTN_V_UP:
      setBrightnessValue(brightness + 10);
      break;
    case BTN_V_DOWN:
      setBrightnessValue(brightness - 10);
      break;
    case BTN_W:
      setStaticColor(strip.Color(255, 255, 255));
      break;
    case BTN_B:
      setStaticColor(strip.Color(0, 80, 255));
      break;
    case BTN_G:
      setStaticColor(strip.Color(0, 255, 80));
      break;
    case BTN_R:
      setStaticColor(strip.Color(255, 0, 40));
      break;
    case BTN_PINK_1:
      setStaticColor(strip.Color(255, 105, 180));
      break;
    case BTN_PINK_2:
      setStaticColor(strip.Color(255, 20, 147));
      break;
    case BTN_CYAN_1:
      setStaticColor(strip.Color(0, 255, 255));
      break;
    case BTN_CYAN_2:
      setStaticColor(strip.Color(64, 224, 208));
      break;
    case BTN_BLUE_2:
      setStaticColor(strip.Color(75, 0, 255));
      break;
    case BTN_PURPLE_1:
      setStaticColor(strip.Color(128, 0, 255));
      break;
    case BTN_PURPLE_2:
      setStaticColor(strip.Color(186, 85, 211));
      break;
    case BTN_PURPLE_3:
      setStaticColor(strip.Color(255, 0, 255));
      break;
    case BTN_ORANGE_1:
      setStaticColor(strip.Color(255, 80, 0));
      break;
    case BTN_ORANGE_2:
      setStaticColor(strip.Color(255, 140, 0));
      break;
    case BTN_ORANGE_3:
      setStaticColor(strip.Color(255, 200, 0));
      break;
    case BTN_YELLOW:
      setStaticColor(strip.Color(255, 255, 40));
      break;
    case BTN_LT_GREEN_1:
      setStaticColor(strip.Color(120, 255, 0));
      break;
    case BTN_LT_BLUE_1:
      setStaticColor(strip.Color(0, 180, 255));
      break;
    case BTN_LT_BLUE_2:
      setStaticColor(strip.Color(0, 128, 180));
      break;
    case BTN_GREEN_BLUE:
      setStaticColor(strip.Color(0, 255, 140));
      break;
    case BTN_S1:
      selectMode(MODE_AUDIO_SPECTRUM);
      break;
    case BTN_S2:
      selectMode(MODE_AUDIO_SPARKLE);
      break;
    case BTN_S3:
      selectMode(MODE_AUDIO_PULSE);
      break;
    case BTN_S4:
      selectMode(MODE_AUDIO_WAVE);
      break;
    case BTN_JUMP3:
      selectMode(MODE_BLINK);
      break;
    case BTN_JUMP7:
      selectMode(MODE_COLOR_BLEND);
      break;
    case BTN_FLASH:
      selectMode(MODE_FADE_3);
      break;
    case BTN_FADE3:
      selectMode(MODE_RAINBOW);
      break;
    case BTN_FADE7:
      selectMode(MODE_FADE_7);
      break;
    case BTN_DIM_1:
      setBrightnessValue(64);
      break;
    case BTN_DIM_2:
      setBrightnessValue(128);
      break;
    case BTN_DIM_3:
      setBrightnessValue(192);
      break;
    case BTN_DIM_4:
      setBrightnessValue(255);
      break;
    // The old 1H-4H fixed thresholds now select adaptive sensitivity.
    case BTN_1H:
      audioSensitivity = 1.60f;
      break;
    case BTN_2H:
      audioSensitivity = 1.20f;
      break;
    case BTN_3H:
      audioSensitivity = 0.90f;
      break;
    case BTN_4H:
      audioSensitivity = 0.65f;
      break;
    default:
      break;
  }
}

void checkIrInput() {
  if (!IrReceiver.decode()) return;

  const bool isRepeat =
      (IrReceiver.decodedIRData.flags & IRDATA_FLAGS_IS_REPEAT) != 0U;
  const uint16_t command = IrReceiver.decodedIRData.command;
  IrReceiver.resume();
  processIrCommand(command, isRepeat);
}

// ------------------------------- Audio setup -------------------------------

void setupAudio() {
  AudioToolsLogger.begin(Serial, AudioToolsLogLevel::Warning);

  auto speakerConfig = speakerOutput.defaultConfig(TX_MODE);
  speakerConfig.sample_rate = 44100;
  speakerConfig.channels = 2;
  speakerConfig.bits_per_sample = 16;
  speakerConfig.port_no = SPEAKER_I2S_PORT;
  speakerConfig.pin_bck = SPEAKER_BCLK_PIN;
  speakerConfig.pin_ws = SPEAKER_WS_PIN;
  speakerConfig.pin_data = SPEAKER_DATA_PIN;
  speakerConfig.i2s_format = I2S_STD_FORMAT;
  speakerConfig.buffer_count = 8;
  speakerConfig.buffer_size = 128;

  if (!speakerOutput.begin(speakerConfig)) {
    Serial.println(F("ERROR: could not start MAX98357A I2S output"));
  }

  auto microphoneConfig = microphoneInput.defaultConfig(RX_MODE);
  microphoneConfig.sample_rate = MIC_SAMPLE_RATE;
  microphoneConfig.channels = 1;
  microphoneConfig.bits_per_sample = 32;
  microphoneConfig.port_no = MIC_I2S_PORT;
  microphoneConfig.pin_bck = MIC_BCLK_PIN;
  microphoneConfig.pin_ws = MIC_WS_PIN;
  microphoneConfig.pin_data = MIC_DATA_PIN;
  microphoneConfig.i2s_format = I2S_STD_FORMAT;
  microphoneConfig.buffer_count = 4;
  microphoneConfig.buffer_size = 256;

#if ESP_IDF_VERSION_MAJOR >= 5
  microphoneConfig.channel_format = I2SChannelSelect::Left;
#else
  microphoneConfig.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
#endif

  microphoneReady = microphoneInput.begin(microphoneConfig);
  if (microphoneReady) {
    // Never let microphone capture stall animation or Bluetooth processing.
    microphoneInput.driver()->setWaitTimeReadMs(0);
  } else {
    Serial.println(F("ERROR: could not start I2S microphone input"));
  }

  a2dpSink.set_default_bt_mode(ESP_BT_MODE_BTDM);
  // One MAX98357A drives one speaker, so mix both A2DP channels to mono.
  a2dpSink.set_mono_downmix(true);
  a2dpSink.set_stream_reader(onBluetoothAudio, true);
  a2dpSink.set_auto_reconnect(true);
  a2dpSink.start(BLUETOOTH_NAME);
}

// -------------------------------- Arduino ----------------------------------

void setup() {
  Serial.begin(115200);
  delay(100);

  strip.begin();
  strip.setBrightness(brightness);
  strip.clear();
  strip.show();

  IrReceiver.begin(IR_PIN, DISABLE_LED_FEEDBACK);

  // Start A2DP in dual-mode Bluetooth before adding the BLE control service.
  setupAudio();
  setupBLE();

  randomSeed(static_cast<uint32_t>(micros()));
  Serial.println(F("OTL PC Tuner ready: A2DP + BLE + mic + MAX98357A"));
}

void loop() {
  checkIrInput();
  checkBleInput();

  // Drain I2S1 continuously even while a non-audio light effect is selected.
  // This keeps the microphone live alongside I2S0 speaker playback.
  readMicrophone();
  updateLights();

  delay(1);
}

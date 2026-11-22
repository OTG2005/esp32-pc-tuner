/*
  OTL PC Tuner - ESP32 Bluetooth speaker and sound-reactive 8x8 matrix

  Bluetooth audio uses one ESP32 I2S controller:
    I2S0 TX -> MAX98357A: BCLK=26, LRC=25, DIN=22

  Bluetooth Classic A2DP carries music to the speaker. BLE remains available
  for readable light/audio commands in BTDM (Classic + BLE) mode.
  Wi-Fi serves lightweight, no-login controls with the same command grammar.
  Music modes analyze the decoded A2DP PCM that is actually sent to the amp;
  without fresh Bluetooth PCM, music effects go dark. No external audio input
  or I2S1 receive driver is initialized.
*/

#include <Arduino.h>
#include <Adafruit_NeoPixel.h>
#include <IRremote.hpp>

// AudioTools must be included before ESP32-A2DP 1.8.x.
#include "AudioTools.h"
#include "BluetoothA2DPSink.h"

#include "BleControl.h"
#include <atomic>
#include <stdarg.h>
#include <esp32/rom/ets_sys.h>
#include "ControlBridge.h"
#include "IrControl.h"
#include "AudioSelection.h"
#include "WebControl.h"
#include "RuntimeResources.h"

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

constexpr char BLUETOOTH_NAME[] = "OTL PC Tuner";
constexpr char BLE_CONTROL_NAME[] = "OTL PC Tuner BLE";

// Nordic UART Service used by the existing phone controller.
constexpr char SERVICE_UUID[] = "6E400001-B5A3-F393-E0A9-E50E24DCCA9E";
constexpr char RX_UUID[] = "6E400002-B5A3-F393-E0A9-E50E24DCCA9E";
constexpr char TX_UUID[] = "6E400003-B5A3-F393-E0A9-E50E24DCCA9E";

// ------------------------------ Peripherals --------------------------------

Adafruit_NeoPixel strip(PIXEL_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);

I2SStream speakerOutput;
BluetoothA2DPSink a2dpSink(speakerOutput);

std::atomic<bool> bluetoothConnected{false};
std::atomic<bool> bluetoothPlaying{false};
std::atomic<bool> bluetoothTransportAvailable{false};
std::atomic<int> pendingRemoteVolume{-1};
QueueHandle_t transportQueue = nullptr;
bool speakerReady = false;
bool transportTaskReady = false;
bool firmwareReady = false;
size_t bleHelpLine = CONTROL_HELP_COUNT;

// ----------------------------- Light state ---------------------------------

ControlState controls;
uint8_t &brightness = controls.brightness;
uint8_t &currentLightMode = controls.mode;
uint32_t controlRevision = 0;

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
  bool active;
};

// Keep this macro after the sketch's types: Arduino inserts generated function
// prototypes at the first function, including the function inside this macro.
SET_LOOP_TASK_STACK_SIZE(runtime_resources::LOOP_STACK_BYTES);

portMUX_TYPE audioMux = portMUX_INITIALIZER_UNLOCKED;
RawAudioLevels bluetoothLevels = {};
DisplayAudioLevels displayAudio = {};
float spectrumHeights[MATRIX_WIDTH] = {};

float audioDynamicRange = 1.0f;
float audioFastEnvelope = 0.0f;
float audioSlowEnvelope = 0.0f;
float &audioSensitivity = controls.sensitivity;
uint32_t lastAudioSequence = 0;
uint32_t lastBeatMs = 0;
bool audioMeterInitialized = false;

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

void applyLightState() {
  strip.setBrightness(brightness);
  if (!controls.lightsOn || brightness == 0) strip.clear();
  else if (currentLightMode == MODE_STATIC) strip.fill(controls.color);
  else strip.clear();
  animationTimestamp = 0;
  strip.show();
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

RawAudioLevels latestAudioLevels() {
  RawAudioLevels bluetoothCopy;
  portENTER_CRITICAL(&audioMux);
  bluetoothCopy = bluetoothLevels;
  portEXIT_CRITICAL(&audioMux);

  if (!hasFreshBluetoothAudio(millis(), bluetoothCopy.sequence, bluetoothCopy.timestampMs))
    return RawAudioLevels{};
  return bluetoothCopy;
}

void updateDisplayAudio() {
  const RawAudioLevels raw = latestAudioLevels();
  if (raw.sequence == 0U) {
    displayAudio = {};
    memset(spectrumHeights, 0, sizeof(spectrumHeights));
    audioMeterInitialized = false;
    lastAudioSequence = 0;
    return;
  }
  displayAudio.active = true;
  if (raw.sequence == lastAudioSequence) {
    displayAudio.overall *= 0.92f;
    displayAudio.bass *= 0.91f;
    displayAudio.mid *= 0.91f;
    displayAudio.treble *= 0.91f;
    displayAudio.beat *= 0.72f;
    return;
  }

  if (!audioMeterInitialized) {
    audioDynamicRange = raw.average > 1U ? raw.average * 0.45f : 1.0f;
    audioFastEnvelope = 0.0f;
    audioSlowEnvelope = 0.0f;
    displayAudio = {};
    audioMeterInitialized = true;
  }

  lastAudioSequence = raw.sequence;
  displayAudio.active = true;

  const float signal = static_cast<float>(raw.average);
  constexpr float minimumRange = 180.0f;
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
  colorIndex %= colorCount;

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
      static_cast<uint8_t>(displayAudio.overall * 10.0f);
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
  const float intensity = clamp01(displayAudio.overall * 0.88f +
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
    if (targets[x] > spectrumHeights[x]) {
      spectrumHeights[x] += (targets[x] - spectrumHeights[x]) * 0.70f;
    } else {
      spectrumHeights[x] *= 0.82f;
    }

    const uint8_t height = static_cast<uint8_t>(
        constrain(static_cast<int>(spectrumHeights[x] * 9.0f), 0,
                  static_cast<int>(MATRIX_HEIGHT)));
    for (uint8_t y = 0; y < height; ++y) {
      uint32_t color = colorWheel(animationPhase + x * 12U + y * 18U);
      float intensity = 0.38f + spectrumHeights[x] * 0.62f;
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
  if (!controls.lightsOn || currentLightMode == MODE_STATIC) return;

  const bool audioMode = currentLightMode >= MODE_AUDIO_SPARKLE;
  const uint32_t interval = audioMode ? 30U : 0U;
  if (interval != 0U &&
      static_cast<uint32_t>(millis() - animationTimestamp) < interval) {
    return;
  }
  if (audioMode) {
    animationTimestamp = millis();
    if (!displayAudio.active) {
      strip.clear();
      strip.show();
      return;
    }
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

void updateStatusSnapshot() {
  DeviceStatus status;
  status.controls = controls;
  status.bluetoothConnected = bluetoothConnected.load();
  status.bluetoothPlaying = bluetoothPlaying.load();
  status.playbackAvailable = bluetoothConnected.load() &&
      bluetoothTransportAvailable.load() && transportTaskReady;
  status.speakerReady = speakerReady;
  status.bluetoothPcmActive = displayAudio.active;
  status.level = displayAudio.overall;
  status.bass = displayAudio.bass;
  status.mid = displayAudio.mid;
  status.treble = displayAudio.treble;
  status.uptime = millis();
  status.freeHeap = ESP.getFreeHeap();
  status.revision = controlRevision;
  publishDeviceStatus(status);
}

void transportTask(void *) {
  TransportAction action;
  for (;;) {
    if (xQueueReceive(transportQueue, &action, portMAX_DELAY) == pdTRUE) {
      if (!bluetoothConnected.load() || !bluetoothTransportAvailable.load()) continue;
      switch (action) {
        case TransportAction::Play: a2dpSink.play(); break;
        case TransportAction::Pause: a2dpSink.pause(); break;
        case TransportAction::Stop: a2dpSink.stop(); break;
        case TransportAction::Next: a2dpSink.next(); break;
        case TransportAction::Previous: a2dpSink.previous(); break;
      }
    }
  }
}

ControlResult executeControl(const ControlCommand &command, bool remoteVolume = false) {
  const bool playbackAvailable = bluetoothConnected.load() &&
      bluetoothTransportAvailable.load() && transportTaskReady;
  ControlResult result = applyControlCommand(controls, command, playbackAvailable);
  if (!result.ok) return result;
  if (remoteVolume) controls.muted = false;
  if (result.actions & ACTION_LIGHTS) applyLightState();
  if ((result.actions & ACTION_VOLUME) && !remoteVolume)
    a2dpSink.set_volume(controls.muted ? 0 : controls.volume);
  if (result.actions & ACTION_DOWNMIX) a2dpSink.set_mono_downmix(controls.mono);
  if (result.actions & ACTION_CALIBRATE) {
    audioMeterInitialized = false;
    lastAudioSequence = 0;
    audioFastEnvelope = audioSlowEnvelope = 0;
    displayAudio = {};
    memset(spectrumHeights, 0, sizeof(spectrumHeights));
  }
  if ((result.actions & ACTION_WIFI_RECONNECT) && !requestWifiReconnect()) {
    result.ok = false;
    result.message = "Wi-Fi service is unavailable; check private configuration and Serial Monitor";
  }
  if (result.actions & ACTION_TRANSPORT) {
    TransportAction action = static_cast<TransportAction>(command.value);
    if (!transportQueue || xQueueSend(transportQueue, &action, 0) != pdTRUE) {
      result.ok = false;
      result.message = "playback queue is full; retry";
    }
  }
  if (result.ok && !isReadOnlyCommand(command.kind)) ++controlRevision;
  updateStatusSnapshot();
  return result;
}

void sendBleText(const char *format, ...) {
  CommandReply reply = {};
  va_list args;
  va_start(args, format);
  vsnprintf(reply.message, sizeof(reply.message), format, args);
  va_end(args);
  if (bleReplyQueue && xQueueSend(bleReplyQueue, &reply, 0) != pdTRUE)
    Serial.println(F("BLE replies busy: wait for the current reply before sending commands"));
}

void sendBleWifiStatus() {
  const WifiStatus wifi = readWifiStatus();
  sendBleText("wifi %s; ssid %s; ip %s; signal %d dBm; setup %s; "
      "setup-ip 192.168.4.1; hostname pc-tuner.local\n",
      !wifi.serviceReady ? "unavailable" : wifi.connected ? "connected" : "disconnected",
      *wifi.ssid ? wifi.ssid : "not-configured", *wifi.ip ? wifi.ip : "none",
      wifi.rssi, wifi.setupAp ? "on" : "off");
}

void sendBleError(const char *message) {
  sendBleText("error %s\n", message);
}

void processControlQueue() {
  // The source can update volume from a Bluetooth task. Keep the latest value
  // even if the control queue is busy, and mutate shared state only here.
  const int remoteVolume = pendingRemoteVolume.exchange(-1);
  if (remoteVolume >= 0) {
    ControlCommand volume;
    volume.kind = CommandKind::Volume;
    volume.value = remoteVolume;
    executeControl(volume, true);
  }
  QueuedControl queued;
  for (int count = 0; count < 8 && controlQueue &&
       xQueueReceive(controlQueue, &queued, 0) == pdTRUE; ++count) {
    const ControlResult result = executeControl(
        queued.command, queued.origin == CommandOrigin::RemoteVolume);
    CommandReply reply = {};
    reply.id = queued.id;
    reply.ok = result.ok;
    reply.kind = queued.command.kind;
    if (queued.origin == CommandOrigin::Web) {
      strlcpy(reply.message, result.message, sizeof(reply.message));
      xQueueSend(webReplyQueue, &reply, 0);
    } else if (queued.origin == CommandOrigin::Bluetooth) {
      if (queued.command.kind == CommandKind::Status) {
        sendBleText("lights %s; brightness %u; effect %s; color #%06lX; "
          "sensitivity %.2f; source %s\n",
          controls.lightsOn ? "on" : "off", controls.brightness, effectName(controls.mode),
          static_cast<unsigned long>(controls.color), controls.sensitivity,
          sourceName(controls.source));
        sendBleText("volume %u; mute %s; downmix %s; bluetooth %s; playback %s; "
          "transport %s; PCM %s; speaker %s; revision %lu\n",
          controls.volume, controls.muted ? "on" : "off",
          controls.mono ? "mono" : "stereo",
          bluetoothConnected.load() ? "connected" : "disconnected",
          bluetoothPlaying.load() ? "playing" : "paused",
          bluetoothTransportAvailable.load() ? "ready" : "unavailable",
          displayAudio.active ? "active" : "idle",
          speakerReady ? "ready" : "unavailable",
          static_cast<unsigned long>(controlRevision));
        sendBleWifiStatus();
      } else if (queued.command.kind == CommandKind::WifiStatus) {
        sendBleWifiStatus();
      } else if (queued.command.kind == CommandKind::Help) {
        bleHelpLine = 0;
      } else {
        sendBleText("%s %s: %s\n",
          result.ok ? "ok" : "error", commandName(queued.command.kind), result.message);
      }
    }
  }
}

void receiveBleCommand(const char *line) {
  if (!line) {
    sendBleError("command too long or contains NUL; send a new complete line");
    return;
  }
  uint32_t id;
  const char *error;
  if (!enqueueControl(line, CommandOrigin::Bluetooth, id, error)) sendBleError(error);
}

void setupBLE() {
  if (!setupBleUart(BLE_CONTROL_NAME, receiveBleCommand))
    Serial.println(F("[bluetooth] BLE UART could not initialize; check resource logs"));
}

void checkBleInput() {
  serviceBleUart();
  static CommandReply active = {};
  static size_t offset = 0;
  static uint32_t lastSend = 0;
  if (!bleUartConnected()) {
    active.message[0] = '\0'; offset = 0;
    bleHelpLine = CONTROL_HELP_COUNT;
    CommandReply discarded;
    while (bleReplyQueue && xQueueReceive(bleReplyQueue, &discarded, 0) == pdTRUE) {}
    return;
  }
  if (!bleUartSubscribed()) return;
  if (static_cast<uint32_t>(millis() - lastSend) < 25) return;
  if (!active.message[0]) {
    if (!bleReplyQueue) return;
    if (xQueueReceive(bleReplyQueue, &active, 0) != pdTRUE) {
      if (bleHelpLine >= CONTROL_HELP_COUNT) return;
      strlcpy(active.message, CONTROL_HELP_LINES[bleHelpLine++], sizeof(active.message));
    }
    offset = 0;
  }
  const size_t remaining = strlen(active.message) - offset;
  const size_t count = remaining < 20 ? remaining : 20; // Default MTU payload.
  if (!sendBleUart(reinterpret_cast<const uint8_t *>(active.message + offset), count)) return;
  offset += count;
  lastSend = millis();
  if (offset >= strlen(active.message)) active.message[0] = '\0';
}

// ------------------------------ IR controls --------------------------------

void processIrCommand(uint16_t command, bool isRepeat) {
  ControlCommand parsed;
  if (parseIrControl(command, isRepeat, parsed)) executeControl(parsed);
}

void checkIrInput() {
  if (!IrReceiver.decode()) return;

  const bool known = IrReceiver.decodedIRData.protocol != UNKNOWN;
  const bool isRepeat =
      (IrReceiver.decodedIRData.flags & IRDATA_FLAGS_IS_REPEAT) != 0U;
  const uint16_t command = IrReceiver.decodedIRData.command;
  IrReceiver.resume();
  if (known) processIrCommand(command, isRepeat);
}

// ------------------------------- Audio setup -------------------------------

void onAudioConnection(esp_a2d_connection_state_t state, void *) {
  bluetoothConnected = state == ESP_A2D_CONNECTION_STATE_CONNECTED;
  if (!bluetoothConnected.load()) {
    bluetoothPlaying = false;
    bluetoothTransportAvailable = false;
  }
  ets_printf("[bluetooth] A2DP state=%u\n", static_cast<unsigned>(state));
}
void onTransportConnection(bool connected) {
  bluetoothTransportAvailable = connected;
  ets_printf("[bluetooth] AVRCP %s\n", connected ? "connected" : "disconnected");
}
void onPlaybackState(esp_a2d_audio_state_t state, void *) {
  bluetoothPlaying = state == ESP_A2D_AUDIO_STATE_STARTED;
}
void onRemoteVolume(int volume) {
  if (volume >= 0 && volume <= 127) pendingRemoteVolume = volume;
}

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

  speakerReady = speakerOutput.begin(speakerConfig);
  if (!speakerReady) {
    Serial.println(F("ERROR: could not start MAX98357A I2S output"));
  }

  logRuntimeMemory("after speaker I2S driver");
  if (!startTunerBluetoothController()) {
    Serial.println(F("ERROR: Bluetooth controller could not start; audio/BLE not advertised"));
    return;
  }
  logRuntimeMemory("after Bluetooth controller");
  a2dpSink.set_default_bt_mode(ESP_BT_MODE_BTDM);
  // One MAX98357A drives one speaker, so mix both A2DP channels to mono.
  a2dpSink.set_mono_downmix(true);
  a2dpSink.set_stream_reader(onBluetoothAudio, true);
  a2dpSink.set_on_connection_state_changed(onAudioConnection);
  a2dpSink.set_on_audio_state_changed_post(onPlaybackState);
  a2dpSink.set_avrc_connection_state_callback(onTransportConnection);
  a2dpSink.set_avrc_rn_volumechange(onRemoteVolume);
  a2dpSink.set_volume(controls.volume);
  a2dpSink.set_auto_reconnect(true);
  a2dpSink.start(BLUETOOTH_NAME);
  logRuntimeMemory("after A2DP stack");
}

// -------------------------------- Arduino ----------------------------------

void setup() {
  Serial.begin(115200);
  delay(100);
  installRuntimeDiagnostics();
  Serial.println(F("OTL PC Tuner startup: testing-bluetooth-only-1"));
  logRuntimeMemory("boot");
  controlQueue = xQueueCreate(runtime_resources::CONTROL_QUEUE_DEPTH, sizeof(QueuedControl));
  webReplyQueue = xQueueCreate(runtime_resources::WEB_REPLY_DEPTH, sizeof(CommandReply));
  bleReplyQueue = xQueueCreate(runtime_resources::BLE_REPLY_DEPTH, sizeof(CommandReply));
  transportQueue = xQueueCreate(4, sizeof(TransportAction));
  if (!controlQueue || !webReplyQueue || !bleReplyQueue || !transportQueue) {
    Serial.println(F("ERROR: not enough memory for control queues"));
    return;
  }
  logRuntimeMemory("after control queues");

  strip.begin();
  strip.setBrightness(brightness);
  strip.clear();
  strip.show();

  IrReceiver.begin(IR_PIN, DISABLE_LED_FEEDBACK);

  // Start A2DP in dual-mode Bluetooth before adding the BLE control service.
  setupAudio();
  if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_ENABLED &&
      esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_ENABLED) setupBLE();
  transportTaskReady = xTaskCreatePinnedToCore(
      transportTask, "pc-playback", runtime_resources::PLAYBACK_STACK_BYTES,
      nullptr, 1, nullptr, 1) == pdPASS;
  if (!transportTaskReady) Serial.println(F("ERROR: Bluetooth playback task could not start"));
  updateStatusSnapshot();
  setupWebControl();

  randomSeed(static_cast<uint32_t>(micros()));
  firmwareReady = true;
  Serial.println(F("Core controls initialized; network task will report HTTP/AP readiness separately."));
}

void loop() {
  if (!firmwareReady) { delay(250); return; }
  checkIrInput();
  processControlQueue();
  checkBleInput();

  static uint32_t lastMeter = 0;
  if (static_cast<uint32_t>(millis() - lastMeter) >= 30) {
    lastMeter = millis();
    updateDisplayAudio();
    updateStatusSnapshot();
  }
  updateLights();
  static uint32_t lastMemoryLog = 0;
  if (static_cast<uint32_t>(millis() - lastMemoryLog) >=
      runtime_resources::MEMORY_LOG_INTERVAL_MS) {
    lastMemoryLog = millis();
    logRuntimeMemory("running");
  }

  delay(1);
}

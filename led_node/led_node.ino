/*
  OTL PC Tuner - Board 2: Dedicated LED Matrix & IR Receiver Node

  Hardware (ESP32 DevKit):
    - GPIO 4  -> 8x8 WS2812B/NeoPixel RGB matrix (through level shifter + 330-470 ohm resistor)
    - GPIO 34 <- 3.3V IR receiver OUT

  Responsibilities:
    - Stores and executes all non-audio modes directly on this board:
        static, rainbow, blink, color-blend, fade-three, fade-seven
    - Connects to the same 2.4 GHz Wi-Fi router as Board 1 (Controller) and
      announces itself over UDP (port 4210) for zero-config auto-discovery
    - Receives real-time 8x8 RGB frames from the Controller in sound-reactive
      modes (sparkle, pulse, wave, spectrum), clearing the matrix if frames stop
      for 500 ms
    - Decodes all 41 IR remote buttons on GPIO 34, executes local changes
      immediately, and syncs all IR changes (including audio mode & sensitivity
      buttons) back to the Controller over Wi-Fi UDP

  No Bluetooth or speaker hardware runs on this board, keeping high-current LED
  switching noise isolated from the audio board and maximizing LED frame timing.
*/

#include <Arduino.h>
#include <Adafruit_NeoPixel.h>
#include <IRremote.hpp>
#include <math.h>
#include <string.h>

#include "ControlProtocol.h"
#include "IrControl.h"
#include "LedWebSetup.h"
#include "NodeProtocol.h"
#include "NodeUdp.h"
#include "RuntimeResources.h"
#include "TunerWiFi.h"

// ------------------------------- Hardware ---------------------------------

constexpr uint8_t IR_PIN = 34;

constexpr uint8_t LED_PIN = 4;
constexpr uint8_t MATRIX_WIDTH = node_protocol::MATRIX_WIDTH;
constexpr uint8_t MATRIX_HEIGHT = node_protocol::MATRIX_HEIGHT;
constexpr uint16_t PIXEL_COUNT = node_protocol::PIXEL_COUNT;

// Most 8x8 WS2812 panels use serpentine rows. Change to false for progressive
// row wiring. Y=0 is treated as the bottom of the matrix.
constexpr bool MATRIX_SERPENTINE = true;

// ------------------------------ Peripherals --------------------------------

Adafruit_NeoPixel strip(PIXEL_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);

SET_LOOP_TASK_STACK_SIZE(runtime_resources::LOOP_STACK_BYTES);

// ------------------------------ Light state --------------------------------

ControlState controls;
uint32_t animationTimestamp = 0;
uint8_t animationPhase = 0;

uint32_t lastRemoteFrameMs = 0;
bool remoteFrameCleared = true;

// --------------------------- Wi-Fi Controller Link -------------------------

uint32_t controllerIpNetOrder = 0;
char controllerIpText[16] = {};
uint16_t udpTxSequence = 0;
uint32_t lastAnnounceMs = 0;

struct PendingStateSync {
  bool active = false;
  uint16_t sequence = 0;
  uint8_t retriesLeft = 0;
  uint32_t lastSentMs = 0;
  node_protocol::LedStateSyncPayload payload = {};
};

PendingStateSync pendingSync;

// ----------------------------- Matrix helpers ------------------------------

float clamp01(float value) {
  if (value < 0.0f) return 0.0f;
  if (value > 1.0f) return 1.0f;
  return value;
}

uint16_t matrixIndex(uint8_t x, uint8_t y) {
  if (x >= MATRIX_WIDTH || y >= MATRIX_HEIGHT) return 0;
  if (MATRIX_SERPENTINE && (y & 1U)) {
    x = static_cast<uint8_t>(MATRIX_WIDTH - 1U - x);
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
  position = static_cast<uint8_t>(255U - position);
  if (position < 85U) {
    return strip.Color(static_cast<uint8_t>(255U - position * 3U), 0,
                       static_cast<uint8_t>(position * 3U));
  }
  if (position < 170U) {
    position = static_cast<uint8_t>(position - 85U);
    return strip.Color(0, static_cast<uint8_t>(position * 3U),
                       static_cast<uint8_t>(255U - position * 3U));
  }
  position = static_cast<uint8_t>(position - 170U);
  return strip.Color(static_cast<uint8_t>(position * 3U),
                     static_cast<uint8_t>(255U - position * 3U), 0);
}

void fillStrip(uint32_t color, bool showNow = true) {
  strip.fill(color, 0, PIXEL_COUNT);
  if (showNow) strip.show();
}

void applyLocalLightState() {
  strip.setBrightness(controls.brightness);
  if (!controls.lightsOn || controls.brightness == 0) {
    strip.clear();
  } else if (controls.mode == MODE_STATIC) {
    strip.fill(controls.color, 0, PIXEL_COUNT);
  } else {
    strip.clear();
  }
  animationTimestamp = 0;
  lastRemoteFrameMs = 0;
  remoteFrameCleared = true;
  strip.show();
}

// ----------------------- Local non-audio animations ------------------------

void renderRainbow() {
  if (static_cast<uint32_t>(millis() - animationTimestamp) < 40U) return;
  animationTimestamp = millis();
  ++animationPhase;

  for (uint8_t y = 0; y < MATRIX_HEIGHT; ++y) {
    for (uint8_t x = 0; x < MATRIX_WIDTH; ++x) {
      setMatrixPixel(x, y,
                     colorWheel(static_cast<uint8_t>(animationPhase + x * 12U + y * 8U)));
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
  colorIndex = static_cast<uint8_t>((colorIndex + 1U) % 3U);
}

void renderColorBlend() {
  if (static_cast<uint32_t>(millis() - animationTimestamp) < 25U) return;
  animationTimestamp = millis();
  animationPhase = static_cast<uint8_t>(animationPhase + 2U);
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
    colorIndex = static_cast<uint8_t>((colorIndex + 1U) % colorCount);
  }
  fadeValue = static_cast<uint8_t>(nextValue);

  const float gammaCorrected = powf(static_cast<float>(fadeValue) / 255.0f, 2.2f);
  fillStrip(scaleColor(colors[colorIndex], gammaCorrected));
}

void displayRemoteAudioFrame(const node_protocol::LedFramePayload &frame) {
  if (!controls.lightsOn) return;

  if (controls.brightness != frame.brightness) {
    controls.brightness = frame.brightness;
    strip.setBrightness(controls.brightness);
  }

  if ((frame.frameFlags & node_protocol::FRAME_FLAG_CLEAR) ||
      !(frame.frameFlags & node_protocol::FRAME_FLAG_ACTIVE)) {
    strip.clear();
    strip.show();
    remoteFrameCleared = true;
    return;
  }

  for (uint8_t y = 0; y < MATRIX_HEIGHT; ++y) {
    for (uint8_t x = 0; x < MATRIX_WIDTH; ++x) {
      const size_t offset = (static_cast<size_t>(y) * MATRIX_WIDTH + x) * 3U;
      setMatrixPixel(x, y, strip.Color(frame.rgb[offset + 0U],
                                       frame.rgb[offset + 1U],
                                       frame.rgb[offset + 2U]));
    }
  }
  strip.show();
  lastRemoteFrameMs = millis();
  remoteFrameCleared = false;
}

void updateLocalLights() {
  if (!controls.lightsOn || controls.mode == MODE_STATIC) return;

  if (node_protocol::isAudioEffectMode(controls.mode)) {
    // Sound-reactive frames are driven by Board 1 (Controller).
    // Clear the matrix if no fresh frame arrives within 500 ms.
    if (!remoteFrameCleared &&
        static_cast<uint32_t>(millis() - lastRemoteFrameMs) >=
            node_protocol::FRAME_TIMEOUT_MS) {
      strip.clear();
      strip.show();
      remoteFrameCleared = true;
    }
    return;
  }

  switch (controls.mode) {
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
    default:
      controls.mode = MODE_RAINBOW;
      break;
  }
}

// --------------------------- Wi-Fi Node Link -------------------------------

void sendAnnounce(uint32_t targetIpNetOrder = 0) {
  if (!node_udp::isOpen()) return;
  node_protocol::NodeAnnouncePayload payload;
  tuner_wifi::stationMac(payload.nodeId);
  payload.lightsOn = controls.lightsOn;
  payload.brightness = controls.brightness;
  payload.mode = controls.mode;
  payload.color = controls.color;
  payload.sensitivityX100 = node_protocol::encodeSensitivity(controls.sensitivity);
  payload.uptimeSec = millis() / 1000U;

  uint8_t packet[node_protocol::MAX_PACKET_BYTES];
  const size_t length = node_protocol::encodeNodeAnnounce(
      packet, sizeof(packet), node_protocol::NodeType::LedMatrix,
      ++udpTxSequence, payload);
  if (!length) return;

  if (targetIpNetOrder != 0U) {
    node_udp::sendUnicast(targetIpNetOrder, node_protocol::UDP_PORT, packet, length);
  } else {
    node_udp::sendBroadcast(node_protocol::UDP_PORT, packet, length);
  }
  lastAnnounceMs = millis();
}

void sendAck(uint32_t targetIpNetOrder, uint16_t ackedSeq,
             node_protocol::PacketType ackedType) {
  if (!node_udp::isOpen() || targetIpNetOrder == 0U) return;
  node_protocol::AckPayload ack;
  ack.ackedSequence = ackedSeq;
  ack.ackedType = ackedType;
  ack.status = 0;

  uint8_t packet[node_protocol::MAX_PACKET_BYTES];
  const size_t length = node_protocol::encodeAck(
      packet, sizeof(packet), node_protocol::NodeType::LedMatrix,
      ++udpTxSequence, ack);
  if (length) {
    node_udp::sendUnicast(targetIpNetOrder, node_protocol::UDP_PORT, packet, length);
  }
}

void queueStateSyncToController(uint8_t syncFlags) {
  pendingSync.payload.lightsOn = controls.lightsOn;
  pendingSync.payload.brightness = controls.brightness;
  pendingSync.payload.mode = controls.mode;
  pendingSync.payload.color = controls.color;
  pendingSync.payload.sensitivityX100 =
      node_protocol::encodeSensitivity(controls.sensitivity);
  pendingSync.payload.syncFlags = syncFlags;
  pendingSync.sequence = ++udpTxSequence;
  pendingSync.retriesLeft = node_protocol::COMMAND_MAX_RETRIES;
  pendingSync.lastSentMs = 0;
  pendingSync.active = true;
}

void sendStateSyncPacket() {
  if (!node_udp::isOpen()) return;
  uint8_t packet[node_protocol::MAX_PACKET_BYTES];
  const size_t length = node_protocol::encodeLedStateSync(
      packet, sizeof(packet), pendingSync.sequence, pendingSync.payload);
  if (!length) return;

  if (controllerIpNetOrder != 0U) {
    node_udp::sendUnicast(controllerIpNetOrder, node_protocol::UDP_PORT, packet, length);
  } else {
    node_udp::sendBroadcast(node_protocol::UDP_PORT, packet, length);
  }
  pendingSync.lastSentMs = millis();
  if (pendingSync.retriesLeft > 0) --pendingSync.retriesLeft;
}

void rememberController(uint32_t ipNetOrder) {
  if (ipNetOrder == 0U || ipNetOrder == controllerIpNetOrder) return;
  controllerIpNetOrder = ipNetOrder;
  node_udp::formatIpv4(ipNetOrder, controllerIpText);
  Serial.printf("[node-link] Controller discovered at %s\n", controllerIpText);
}

void serviceNodeLink() {
  if (!tuner_wifi::connected()) {
    if (node_udp::isOpen()) node_udp::close();
    return;
  }
  if (!node_udp::isOpen()) {
    if (!node_udp::open(node_protocol::UDP_PORT)) return;
    Serial.printf("[node-link] LED Node UDP listening on port %u\n", node_protocol::UDP_PORT);
    sendAnnounce();
  }

  const uint32_t now = millis();
  uint8_t buffer[node_protocol::MAX_PACKET_BYTES];
  uint32_t senderIp = 0;
  uint16_t senderPort = 0;

  for (int i = 0; i < 8; ++i) {
    const size_t len = node_udp::receive(buffer, sizeof(buffer), senderIp, senderPort);
    if (!len) break;
    (void)senderPort;

    node_protocol::PacketHeader header;
    if (!node_protocol::decodeHeader(buffer, len, header)) continue;
    if (header.nodeType != node_protocol::NodeType::Controller) continue;

    rememberController(senderIp);

    if (header.type == node_protocol::PacketType::Discover) {
      sendAnnounce(senderIp);
    } else if (header.type == node_protocol::PacketType::LedCommand) {
      node_protocol::LedCommandPayload cmd;
      if (!node_protocol::decodeLedCommand(buffer, len, cmd)) continue;
      controls.lightsOn = cmd.lightsOn;
      controls.brightness = cmd.brightness;
      controls.mode = cmd.mode;
      controls.color = cmd.color;
      controls.sensitivity = node_protocol::decodeSensitivity(cmd.sensitivityX100);
      applyLocalLightState();
      sendAck(senderIp, header.sequence, header.type);
    } else if (header.type == node_protocol::PacketType::LedFrame) {
      node_protocol::LedFramePayload frame;
      if (!node_protocol::decodeLedFrame(buffer, len, frame)) continue;
      if (node_protocol::isAudioEffectMode(frame.mode)) {
        controls.mode = frame.mode;
        displayRemoteAudioFrame(frame);
      }
    } else if (header.type == node_protocol::PacketType::Ack) {
      node_protocol::AckPayload ack;
      if (node_protocol::decodeAck(buffer, len, ack) &&
          pendingSync.active && ack.ackedSequence == pendingSync.sequence) {
        pendingSync.active = false;
      }
    }
  }

  // Periodically announce presence so Controller always knows our IP & liveness.
  if (static_cast<uint32_t>(now - lastAnnounceMs) >= node_protocol::ANNOUNCE_INTERVAL_MS) {
    sendAnnounce();
  }

  // Send or retry pending IR state sync to Controller.
  if (pendingSync.active) {
    if (pendingSync.lastSentMs == 0 ||
        static_cast<uint32_t>(now - pendingSync.lastSentMs) >=
            node_protocol::COMMAND_RETRY_INTERVAL_MS) {
      if (pendingSync.retriesLeft > 0) {
        sendStateSyncPacket();
      } else {
        pendingSync.active = false;
      }
    }
  }
}

// ------------------------------ IR controls --------------------------------

void processIrCommand(uint16_t command, bool isRepeat) {
  ControlCommand parsed;
  if (!parseIrControl(command, isRepeat, parsed)) return;

  const ControlResult result = applyControlCommand(controls, parsed, false);
  if (!result.ok) return;

  if (result.actions & ACTION_LIGHTS) {
    applyLocalLightState();
  }

  uint8_t syncFlags = node_protocol::SYNC_FLAG_NONE;
  switch (parsed.kind) {
    case CommandKind::Lights:
      syncFlags = node_protocol::SYNC_FLAG_POWER;
      break;
    case CommandKind::Brightness:
      syncFlags = node_protocol::SYNC_FLAG_BRIGHTNESS;
      break;
    case CommandKind::Color:
      syncFlags = node_protocol::SYNC_FLAG_COLOR | node_protocol::SYNC_FLAG_MODE;
      break;
    case CommandKind::Effect:
      syncFlags = node_protocol::SYNC_FLAG_MODE;
      break;
    case CommandKind::Sensitivity:
      syncFlags = node_protocol::SYNC_FLAG_SENSITIVITY;
      break;
    default:
      break;
  }

  if (syncFlags != node_protocol::SYNC_FLAG_NONE) {
    queueStateSyncToController(syncFlags);
  }
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

// -------------------------------- Arduino ----------------------------------

void setup() {
  Serial.begin(115200);
  delay(100);
  installRuntimeDiagnostics();
  Serial.println(F("OTL PC Tuner LED Node startup: dual-esp32-wifi-1"));
  logRuntimeMemory("boot");

  strip.begin();
  strip.setBrightness(controls.brightness);
  strip.clear();
  strip.show();

  IrReceiver.begin(IR_PIN, DISABLE_LED_FEEDBACK);
  setupLedWebSetup();

  Serial.println(F("LED Node ready: 8x8 WS2812 (GPIO 4) + IR (GPIO 34) + Wi-Fi UDP NodeLink."));
}

void loop() {
  checkIrInput();
  serviceNodeLink();
  updateLocalLights();

  static uint32_t lastMemoryLog = 0;
  if (static_cast<uint32_t>(millis() - lastMemoryLog) >=
      runtime_resources::MEMORY_LOG_INTERVAL_MS) {
    lastMemoryLog = millis();
    logRuntimeMemory("running");
  }

  delay(1);
}

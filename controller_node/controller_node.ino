/*
  OTL PC Tuner - Board 1: Controller & Bluetooth Speaker Unit

  Responsibilities:
    - Bluetooth Classic A2DP audio sink -> MAX98357A I2S0 TX (BCLK=26, LRC=25, DIN=22)
    - BLE Nordic UART Service ("OTL PC Tuner BLE") for mobile/PC commands
    - Wi-Fi provisioning + lightweight no-login web UI (http://pc-tuner.local)
    - Bluetooth PCM audio analysis + sound-reactive 8x8 RGB frame rendering
      (sparkle, pulse, wave, spectrum), streamed over Wi-Fi UDP to Board 2 (LED Node)
    - Bidirectional Wi-Fi UDP node link (port 4210) with auto-discovery, command
      ACK/retry, and IR state sync from the LED Node (extensible to future nodes)

  No NeoPixel matrix, IR receiver, or microphone is connected to this board.
  Keeping LED PWM power completely off this board isolates the speaker from
  switching noise and leaves flash/RAM headroom for Wi-Fi, web, and future nodes.
*/

#include <Arduino.h>

// AudioTools must be included before ESP32-A2DP 1.8.x.
#include "AudioTools.h"
#include "BluetoothA2DPSink.h"

#include "AudioEffects.h"
#include "AudioSelection.h"
#include "BleControl.h"
#include "BluetoothResources.h"
#include "ControlBridge.h"
#include "ControlProtocol.h"
#include "NodeProtocol.h"
#include "NodeUdp.h"
#include "RuntimeResources.h"
#include "TunerWiFi.h"
#include "WebControl.h"

#include <atomic>
#include <stdarg.h>
#include <string.h>
#include <esp32/rom/ets_sys.h>

// ------------------------------- Hardware ---------------------------------

constexpr int SPEAKER_I2S_PORT = 0;
constexpr int SPEAKER_BCLK_PIN = 26;
constexpr int SPEAKER_WS_PIN = 25;
constexpr int SPEAKER_DATA_PIN = 22;

constexpr char BLUETOOTH_NAME[] = "OTL PC Tuner";
constexpr char BLE_CONTROL_NAME[] = "OTL PC Tuner BLE";

// ------------------------------ Peripherals --------------------------------

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

// ----------------------------- Control state -------------------------------

ControlState controls;
uint32_t controlRevision = 0;

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

// Keep this macro after the sketch's types: Arduino inserts generated function
// prototypes at the first function, including the function inside this macro.
SET_LOOP_TASK_STACK_SIZE(runtime_resources::LOOP_STACK_BYTES);

portMUX_TYPE audioMux = portMUX_INITIALIZER_UNLOCKED;
RawAudioLevels bluetoothLevels = {};
DisplayAudioLevels displayAudio = {};
AudioEffectRenderer audioEffects;

float audioDynamicRange = 1.0f;
float audioFastEnvelope = 0.0f;
float audioSlowEnvelope = 0.0f;
uint32_t lastAudioSequence = 0;
uint32_t lastBeatMs = 0;
bool audioMeterInitialized = false;
bool clearFrameSent = true;

// --------------------------- Wi-Fi LED Node Link ---------------------------

struct RemoteLedNode {
  uint32_t ipNetOrder = 0;
  char ipText[16] = {};
  uint8_t nodeId[6] = {};
  uint32_t lastSeenMs = 0;
  bool online = false;
};

struct PendingLedCommand {
  bool active = false;
  uint16_t sequence = 0;
  uint8_t retriesLeft = 0;
  uint32_t lastSentMs = 0;
  node_protocol::LedCommandPayload payload = {};
};

RemoteLedNode ledNode;
PendingLedCommand pendingLedCmd;
uint16_t udpTxSequence = 0;
uint32_t lastDiscoverMs = 0;

// ----------------------------- Small helpers -------------------------------

float clamp01(float value) {
  if (value < 0.0f) return 0.0f;
  if (value > 1.0f) return 1.0f;
  return value;
}

uint32_t absolute32(int32_t value) {
  if (value >= 0) return static_cast<uint32_t>(value);
  return static_cast<uint32_t>(-(static_cast<int64_t>(value)));
}

// ----------------------------- Audio capture -------------------------------

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

  constexpr size_t MAX_ANALYSIS_FRAMES = 512;
  const size_t step = frameCount > MAX_ANALYSIS_FRAMES
                          ? (frameCount + MAX_ANALYSIS_FRAMES - 1U) / MAX_ANALYSIS_FRAMES
                          : 1U;

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

  const float target = clamp01((signal * controls.sensitivity) /
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

// --------------------------- Wi-Fi Node Link -------------------------------

void queueLedStatePush() {
  pendingLedCmd.payload.lightsOn = controls.lightsOn;
  pendingLedCmd.payload.brightness = controls.brightness;
  pendingLedCmd.payload.mode = controls.mode;
  pendingLedCmd.payload.color = controls.color;
  pendingLedCmd.payload.sensitivityX100 =
      node_protocol::encodeSensitivity(controls.sensitivity);
  pendingLedCmd.sequence = ++udpTxSequence;
  pendingLedCmd.retriesLeft = node_protocol::COMMAND_MAX_RETRIES;
  pendingLedCmd.lastSentMs = 0;
  pendingLedCmd.active = true;
  clearFrameSent = false;
}

void sendLedCommandPacket() {
  if (!ledNode.online || !node_udp::isOpen()) return;
  uint8_t packet[node_protocol::MAX_PACKET_BYTES];
  const size_t length = node_protocol::encodeLedCommand(
      packet, sizeof(packet), pendingLedCmd.sequence, pendingLedCmd.payload);
  if (length) {
    node_udp::sendUnicast(ledNode.ipNetOrder, node_protocol::UDP_PORT, packet, length);
    pendingLedCmd.lastSentMs = millis();
    if (pendingLedCmd.retriesLeft > 0) --pendingLedCmd.retriesLeft;
  }
}

void sendAckToNode(uint32_t ipNetOrder, uint16_t ackedSeq,
                   node_protocol::PacketType ackedType) {
  if (!node_udp::isOpen()) return;
  node_protocol::AckPayload ack;
  ack.ackedSequence = ackedSeq;
  ack.ackedType = ackedType;
  ack.status = 0;
  uint8_t packet[node_protocol::MAX_PACKET_BYTES];
  const size_t length = node_protocol::encodeAck(
      packet, sizeof(packet), node_protocol::NodeType::Controller,
      ++udpTxSequence, ack);
  if (length) {
    node_udp::sendUnicast(ipNetOrder, node_protocol::UDP_PORT, packet, length);
  }
}

void streamAudioFrameToLedNode() {
  if (!ledNode.online || !node_udp::isOpen()) return;
  if (!controls.lightsOn || !node_protocol::isAudioEffectMode(controls.mode)) return;

  node_protocol::LedFramePayload frame;
  const bool active = audioEffects.render(controls.mode, controls.brightness,
                                          displayAudio, frame);
  if (!active) {
    if (clearFrameSent) return;
    clearFrameSent = true;
  } else {
    clearFrameSent = false;
  }

  uint8_t packet[node_protocol::MAX_PACKET_BYTES];
  const size_t length = node_protocol::encodeLedFrame(
      packet, sizeof(packet), ++udpTxSequence, frame);
  if (length) {
    node_udp::sendUnicast(ledNode.ipNetOrder, node_protocol::UDP_PORT, packet, length);
  }
}

void updateStatusSnapshot();

void serviceNodeLink() {
  if (!tuner_wifi::connected()) {
    if (node_udp::isOpen()) node_udp::close();
    ledNode.online = false;
    return;
  }
  if (!node_udp::isOpen()) {
    if (!node_udp::open(node_protocol::UDP_PORT)) return;
    Serial.printf("[node-link] UDP listening on port %u\n", node_protocol::UDP_PORT);
    lastDiscoverMs = 0;
  }

  const uint32_t now = millis();

  // Receive incoming packets from LED Node (and future companion nodes).
  uint8_t buffer[node_protocol::MAX_PACKET_BYTES];
  uint32_t senderIp = 0;
  uint16_t senderPort = 0;
  for (int i = 0; i < 8; ++i) {
    const size_t len = node_udp::receive(buffer, sizeof(buffer), senderIp, senderPort);
    if (!len) break;
    (void)senderPort;

    node_protocol::PacketHeader header;
    if (!node_protocol::decodeHeader(buffer, len, header)) continue;

    if (header.type == node_protocol::PacketType::NodeAnnounce &&
        header.nodeType == node_protocol::NodeType::LedMatrix) {
      node_protocol::NodeAnnouncePayload announce;
      if (!node_protocol::decodeNodeAnnounce(buffer, len, announce)) continue;
      const bool newlyDiscovered = !ledNode.online || ledNode.ipNetOrder != senderIp;
      ledNode.ipNetOrder = senderIp;
      node_udp::formatIpv4(senderIp, ledNode.ipText);
      memcpy(ledNode.nodeId, announce.nodeId, 6);
      ledNode.lastSeenMs = now;
      ledNode.online = true;
      if (newlyDiscovered) {
        Serial.printf("[node-link] LED Node online at %s\n", ledNode.ipText);
        // Push controller's authoritative state to newly connected LED node.
        queueLedStatePush();
        updateStatusSnapshot();
      }
    } else if (header.type == node_protocol::PacketType::Ack &&
               header.nodeType == node_protocol::NodeType::LedMatrix) {
      node_protocol::AckPayload ack;
      if (node_protocol::decodeAck(buffer, len, ack) &&
          pendingLedCmd.active && ack.ackedSequence == pendingLedCmd.sequence) {
        pendingLedCmd.active = false;
      }
      ledNode.lastSeenMs = now;
    } else if (header.type == node_protocol::PacketType::LedStateSync &&
               header.nodeType == node_protocol::NodeType::LedMatrix) {
      node_protocol::LedStateSyncPayload sync;
      if (!node_protocol::decodeLedStateSync(buffer, len, sync)) continue;
      ledNode.ipNetOrder = senderIp;
      node_udp::formatIpv4(senderIp, ledNode.ipText);
      ledNode.lastSeenMs = now;
      ledNode.online = true;

      // Apply IR-originated state updates from the LED board so Web/BLE stay in sync.
      if (sync.syncFlags & node_protocol::SYNC_FLAG_POWER)
        controls.lightsOn = sync.lightsOn;
      if (sync.syncFlags & node_protocol::SYNC_FLAG_BRIGHTNESS)
        controls.brightness = sync.brightness;
      if (sync.syncFlags & node_protocol::SYNC_FLAG_MODE)
        controls.mode = sync.mode;
      if (sync.syncFlags & node_protocol::SYNC_FLAG_COLOR)
        controls.color = sync.color;
      if (sync.syncFlags & node_protocol::SYNC_FLAG_SENSITIVITY)
        controls.sensitivity = node_protocol::decodeSensitivity(sync.sensitivityX100);

      clearFrameSent = false;
      ++controlRevision;
      sendAckToNode(senderIp, header.sequence, header.type);
      updateStatusSnapshot();
      Serial.printf("[node-link] IR sync from LED Node: lights=%s mode=%s bri=%u sens=%.2f\n",
                    controls.lightsOn ? "on" : "off", effectName(controls.mode),
                    controls.brightness, controls.sensitivity);
    }
  }

  // Mark LED node offline if no packets heard within timeout.
  if (ledNode.online &&
      static_cast<uint32_t>(now - ledNode.lastSeenMs) > node_protocol::NODE_OFFLINE_TIMEOUT_MS) {
    ledNode.online = false;
    Serial.println(F("[node-link] LED Node offline (timeout)"));
    updateStatusSnapshot();
  }

  // Broadcast discovery when LED node is not yet known.
  if (!ledNode.online &&
      (lastDiscoverMs == 0 ||
       static_cast<uint32_t>(now - lastDiscoverMs) >= node_protocol::ANNOUNCE_INTERVAL_MS)) {
    uint8_t packet[node_protocol::MAX_PACKET_BYTES];
    const size_t length = node_protocol::encodeDiscover(
        packet, sizeof(packet), node_protocol::NodeType::Controller, ++udpTxSequence);
    if (length) node_udp::sendBroadcast(node_protocol::UDP_PORT, packet, length);
    lastDiscoverMs = now;
  }

  // Send or retry pending state command to LED node.
  if (pendingLedCmd.active && ledNode.online) {
    if (pendingLedCmd.lastSentMs == 0 ||
        static_cast<uint32_t>(now - pendingLedCmd.lastSentMs) >=
            node_protocol::COMMAND_RETRY_INTERVAL_MS) {
      if (pendingLedCmd.retriesLeft > 0) {
        sendLedCommandPacket();
      } else {
        pendingLedCmd.active = false;
      }
    }
  }
}

// ----------------------------- BLE & Web controls --------------------------

void updateStatusSnapshot() {
  DeviceStatus status;
  status.controls = controls;
  status.bluetoothConnected = bluetoothConnected.load();
  status.bluetoothPlaying = bluetoothPlaying.load();
  status.playbackAvailable = bluetoothConnected.load() &&
      bluetoothTransportAvailable.load() && transportTaskReady;
  status.speakerReady = speakerReady;
  status.bluetoothPcmActive = displayAudio.active;
  status.ledNodeConnected = ledNode.online;
  strlcpy(status.ledNodeIp, ledNode.ipText, sizeof(status.ledNodeIp));
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

  if ((result.actions & ACTION_LIGHTS) || command.kind == CommandKind::Sensitivity) {
    queueLedStatePush();
  }
  if ((result.actions & ACTION_VOLUME) && !remoteVolume)
    a2dpSink.set_volume(controls.muted ? 0 : controls.volume);
  if (result.actions & ACTION_DOWNMIX) a2dpSink.set_mono_downmix(controls.mono);
  if (result.actions & ACTION_CALIBRATE) {
    audioMeterInitialized = false;
    lastAudioSequence = 0;
    audioFastEnvelope = audioSlowEnvelope = 0;
    displayAudio = {};
    audioEffects.reset();
    clearFrameSent = false;
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
  sendBleText("wifi %s; ssid %s; ip %s; signal %d dBm; led-node %s (%s); setup %s\n",
      !wifi.serviceReady ? "unavailable" : wifi.connected ? "connected" : "disconnected",
      *wifi.ssid ? wifi.ssid : "not-configured", *wifi.ip ? wifi.ip : "none",
      wifi.rssi, ledNode.online ? "online" : "offline",
      *ledNode.ipText ? ledNode.ipText : "none",
      wifi.setupAp ? "on" : "off");
}

void sendBleError(const char *message) {
  sendBleText("error %s\n", message);
}

void processControlQueue() {
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
  const size_t count = remaining < 20 ? remaining : 20;
  if (!sendBleUart(reinterpret_cast<const uint8_t *>(active.message + offset), count)) return;
  offset += count;
  lastSend = millis();
  if (offset >= strlen(active.message)) active.message[0] = '\0';
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
  Serial.println(F("OTL PC Tuner Controller startup: dual-esp32-wifi-1"));
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

  audioEffects.seed(static_cast<uint32_t>(micros()));
  setupAudio();
  if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_ENABLED &&
      esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_ENABLED) {
    setupBLE();
  }
  transportTaskReady = xTaskCreatePinnedToCore(
      transportTask, "pc-playback", runtime_resources::PLAYBACK_STACK_BYTES,
      nullptr, 1, nullptr, 1) == pdPASS;
  if (!transportTaskReady) {
    Serial.println(F("ERROR: Bluetooth playback task could not start"));
  }
  updateStatusSnapshot();
  setupWebControl();

  firmwareReady = true;
  Serial.println(F("Controller ready: A2DP + BLE + Web + Wi-Fi UDP NodeLink (no LEDs/IR on this board)."));
}

void loop() {
  if (!firmwareReady) { delay(250); return; }

  processControlQueue();
  checkBleInput();
  serviceNodeLink();

  static uint32_t lastFrameTick = 0;
  if (static_cast<uint32_t>(millis() - lastFrameTick) >= 30U) {
    lastFrameTick = millis();
    updateDisplayAudio();
    streamAudioFrameToLedNode();
    updateStatusSnapshot();
  }

  static uint32_t lastMemoryLog = 0;
  if (static_cast<uint32_t>(millis() - lastMemoryLog) >=
      runtime_resources::MEMORY_LOG_INTERVAL_MS) {
    lastMemoryLog = millis();
    logRuntimeMemory("running");
  }

  delay(1);
}

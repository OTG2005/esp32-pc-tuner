#pragma once

// Portable Wi-Fi UDP protocol shared by the Controller, LED Node, future
// companion nodes (e.g. PC Power / Sensors), and native C++ unit tests.
#include "ControlProtocol.h"
#include <stddef.h>
#include <stdint.h>

namespace node_protocol {

constexpr uint16_t UDP_PORT = 4210;
constexpr uint8_t PROTOCOL_VERSION = 1;
constexpr uint8_t MATRIX_WIDTH = 8;
constexpr uint8_t MATRIX_HEIGHT = 8;
constexpr uint16_t PIXEL_COUNT = MATRIX_WIDTH * MATRIX_HEIGHT;
constexpr size_t FRAME_RGB_BYTES = PIXEL_COUNT * 3U;

constexpr size_t HEADER_BYTES = 12;
constexpr size_t MAX_PACKET_BYTES = 256;

constexpr uint32_t ANNOUNCE_INTERVAL_MS = 3000;
constexpr uint32_t NODE_OFFLINE_TIMEOUT_MS = 10000;
constexpr uint32_t FRAME_TIMEOUT_MS = 500;
constexpr uint32_t COMMAND_RETRY_INTERVAL_MS = 120;
constexpr uint8_t COMMAND_MAX_RETRIES = 3;

enum class NodeType : uint8_t {
  Controller = 0,
  LedMatrix = 1,
  PcPower = 2,
  Sensor = 3
};

enum class PacketType : uint8_t {
  Discover = 1,
  NodeAnnounce = 2,
  LedCommand = 3,
  LedFrame = 4,
  LedStateSync = 5,
  Ack = 6
};

enum FrameFlags : uint8_t {
  FRAME_FLAG_NONE = 0,
  FRAME_FLAG_ACTIVE = 1 << 0,
  FRAME_FLAG_CLEAR = 1 << 1
};

enum SyncFlags : uint8_t {
  SYNC_FLAG_NONE = 0,
  SYNC_FLAG_POWER = 1 << 0,
  SYNC_FLAG_BRIGHTNESS = 1 << 1,
  SYNC_FLAG_MODE = 1 << 2,
  SYNC_FLAG_COLOR = 1 << 3,
  SYNC_FLAG_SENSITIVITY = 1 << 4
};

struct PacketHeader {
  uint8_t version = PROTOCOL_VERSION;
  PacketType type = PacketType::Discover;
  NodeType nodeType = NodeType::Controller;
  uint8_t flags = 0;
  uint16_t sequence = 0;
  uint16_t payloadLength = 0;
};

struct NodeAnnouncePayload {
  uint8_t nodeId[6] = {};
  bool lightsOn = true;
  uint8_t brightness = 192;
  uint8_t mode = MODE_RAINBOW;
  uint32_t color = 0xFFFFFF;
  uint16_t sensitivityX100 = 100;
  uint32_t uptimeSec = 0;
};

struct LedCommandPayload {
  bool lightsOn = true;
  uint8_t brightness = 192;
  uint8_t mode = MODE_RAINBOW;
  uint32_t color = 0xFFFFFF;
  uint16_t sensitivityX100 = 100;
};

struct LedFramePayload {
  uint8_t mode = MODE_AUDIO_SPECTRUM;
  uint8_t brightness = 192;
  uint8_t frameFlags = FRAME_FLAG_ACTIVE;
  uint8_t rgb[FRAME_RGB_BYTES] = {}; // Logical order: (y * 8 + x) * 3 -> R,G,B
};

struct LedStateSyncPayload {
  bool lightsOn = true;
  uint8_t brightness = 192;
  uint8_t mode = MODE_RAINBOW;
  uint32_t color = 0xFFFFFF;
  uint16_t sensitivityX100 = 100;
  uint8_t syncFlags = SYNC_FLAG_NONE;
};

struct AckPayload {
  uint16_t ackedSequence = 0;
  PacketType ackedType = PacketType::LedCommand;
  uint8_t status = 0; // 0 = OK
};

inline bool isAudioEffectMode(uint8_t mode) {
  return mode >= MODE_AUDIO_SPARKLE && mode <= MODE_AUDIO_SPECTRUM;
}

inline uint16_t encodeSensitivity(float sensitivity) {
  if (sensitivity < 0.1f) sensitivity = 0.1f;
  if (sensitivity > 3.0f) sensitivity = 3.0f;
  return static_cast<uint16_t>(sensitivity * 100.0f + 0.5f);
}

inline float decodeSensitivity(uint16_t sensitivityX100) {
  if (sensitivityX100 < 10U) return 0.1f;
  if (sensitivityX100 > 300U) return 3.0f;
  return static_cast<float>(sensitivityX100) / 100.0f;
}

bool decodeHeader(const uint8_t *buffer, size_t length, PacketHeader &header);

size_t encodeDiscover(uint8_t *out, size_t capacity, NodeType sender, uint16_t sequence);

size_t encodeNodeAnnounce(uint8_t *out, size_t capacity, NodeType nodeType,
                          uint16_t sequence, const NodeAnnouncePayload &payload);
bool decodeNodeAnnounce(const uint8_t *buffer, size_t length,
                        NodeAnnouncePayload &payload);

size_t encodeLedCommand(uint8_t *out, size_t capacity, uint16_t sequence,
                        const LedCommandPayload &payload);
bool decodeLedCommand(const uint8_t *buffer, size_t length,
                      LedCommandPayload &payload);

size_t encodeLedFrame(uint8_t *out, size_t capacity, uint16_t sequence,
                      const LedFramePayload &payload);
bool decodeLedFrame(const uint8_t *buffer, size_t length,
                    LedFramePayload &payload);

size_t encodeLedStateSync(uint8_t *out, size_t capacity, uint16_t sequence,
                          const LedStateSyncPayload &payload);
bool decodeLedStateSync(const uint8_t *buffer, size_t length,
                        LedStateSyncPayload &payload);

size_t encodeAck(uint8_t *out, size_t capacity, NodeType sender,
                 uint16_t sequence, const AckPayload &payload);
bool decodeAck(const uint8_t *buffer, size_t length, AckPayload &payload);

} // namespace node_protocol

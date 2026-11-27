#include "NodeProtocol.h"
#include <string.h>

namespace node_protocol {
namespace {

constexpr uint8_t MAGIC[4] = {'P', 'C', 'T', 'N'};

constexpr size_t ANNOUNCE_PAYLOAD_BYTES = 18;
constexpr size_t COMMAND_PAYLOAD_BYTES = 8;
constexpr size_t FRAME_PAYLOAD_BYTES = 3 + FRAME_RGB_BYTES;
constexpr size_t STATE_SYNC_PAYLOAD_BYTES = 9;
constexpr size_t ACK_PAYLOAD_BYTES = 4;

void putU16(uint8_t *dst, uint16_t value) {
  dst[0] = static_cast<uint8_t>(value & 0xFFU);
  dst[1] = static_cast<uint8_t>((value >> 8) & 0xFFU);
}

void putU24(uint8_t *dst, uint32_t value) {
  dst[0] = static_cast<uint8_t>((value >> 16) & 0xFFU);
  dst[1] = static_cast<uint8_t>((value >> 8) & 0xFFU);
  dst[2] = static_cast<uint8_t>(value & 0xFFU);
}

void putU32(uint8_t *dst, uint32_t value) {
  dst[0] = static_cast<uint8_t>(value & 0xFFU);
  dst[1] = static_cast<uint8_t>((value >> 8) & 0xFFU);
  dst[2] = static_cast<uint8_t>((value >> 16) & 0xFFU);
  dst[3] = static_cast<uint8_t>((value >> 24) & 0xFFU);
}

uint16_t getU16(const uint8_t *src) {
  return static_cast<uint16_t>(src[0]) |
         (static_cast<uint16_t>(src[1]) << 8);
}

uint32_t getU24(const uint8_t *src) {
  return (static_cast<uint32_t>(src[0]) << 16) |
         (static_cast<uint32_t>(src[1]) << 8) |
         static_cast<uint32_t>(src[2]);
}

uint32_t getU32(const uint8_t *src) {
  return static_cast<uint32_t>(src[0]) |
         (static_cast<uint32_t>(src[1]) << 8) |
         (static_cast<uint32_t>(src[2]) << 16) |
         (static_cast<uint32_t>(src[3]) << 24);
}

void writeHeader(uint8_t *out, PacketType type, NodeType nodeType,
                 uint8_t flags, uint16_t sequence, uint16_t payloadLength) {
  memcpy(out, MAGIC, sizeof(MAGIC));
  out[4] = PROTOCOL_VERSION;
  out[5] = static_cast<uint8_t>(type);
  out[6] = static_cast<uint8_t>(nodeType);
  out[7] = flags;
  putU16(out + 8, sequence);
  putU16(out + 10, payloadLength);
}

bool validMode(uint8_t mode) {
  switch (mode) {
    case MODE_STATIC:
    case MODE_RAINBOW:
    case MODE_BLINK:
    case MODE_COLOR_BLEND:
    case MODE_FADE_3:
    case MODE_FADE_7:
    case MODE_AUDIO_SPARKLE:
    case MODE_AUDIO_PULSE:
    case MODE_AUDIO_WAVE:
    case MODE_AUDIO_SPECTRUM:
      return true;
    default:
      return false;
  }
}

bool validateEnvelope(const uint8_t *buffer, size_t length,
                      PacketType expectedType, size_t expectedPayload) {
  PacketHeader header;
  if (!decodeHeader(buffer, length, header)) return false;
  return header.type == expectedType && header.payloadLength == expectedPayload;
}

} // namespace

bool decodeHeader(const uint8_t *buffer, size_t length, PacketHeader &header) {
  if (!buffer || length < HEADER_BYTES) return false;
  if (memcmp(buffer, MAGIC, sizeof(MAGIC)) != 0) return false;
  if (buffer[4] != PROTOCOL_VERSION) return false;
  const uint16_t payloadLength = getU16(buffer + 10);
  if (HEADER_BYTES + payloadLength != length) return false;

  header.version = buffer[4];
  header.type = static_cast<PacketType>(buffer[5]);
  header.nodeType = static_cast<NodeType>(buffer[6]);
  header.flags = buffer[7];
  header.sequence = getU16(buffer + 8);
  header.payloadLength = payloadLength;
  return true;
}

size_t encodeDiscover(uint8_t *out, size_t capacity, NodeType sender,
                      uint16_t sequence) {
  if (!out || capacity < HEADER_BYTES) return 0;
  writeHeader(out, PacketType::Discover, sender, 0, sequence, 0);
  return HEADER_BYTES;
}

size_t encodeNodeAnnounce(uint8_t *out, size_t capacity, NodeType nodeType,
                          uint16_t sequence, const NodeAnnouncePayload &payload) {
  const size_t total = HEADER_BYTES + ANNOUNCE_PAYLOAD_BYTES;
  if (!out || capacity < total) return 0;
  writeHeader(out, PacketType::NodeAnnounce, nodeType, 0, sequence,
              ANNOUNCE_PAYLOAD_BYTES);
  uint8_t *p = out + HEADER_BYTES;
  memcpy(p, payload.nodeId, 6);
  p[6] = payload.lightsOn ? 1U : 0U;
  p[7] = payload.brightness;
  p[8] = payload.mode;
  putU24(p + 9, payload.color);
  putU16(p + 12, payload.sensitivityX100);
  putU32(p + 14, payload.uptimeSec);
  return total;
}

bool decodeNodeAnnounce(const uint8_t *buffer, size_t length,
                        NodeAnnouncePayload &payload) {
  if (!validateEnvelope(buffer, length, PacketType::NodeAnnounce,
                        ANNOUNCE_PAYLOAD_BYTES)) {
    return false;
  }
  const uint8_t *p = buffer + HEADER_BYTES;
  if (!validMode(p[8])) return false;
  memcpy(payload.nodeId, p, 6);
  payload.lightsOn = p[6] != 0U;
  payload.brightness = p[7];
  payload.mode = p[8];
  payload.color = getU24(p + 9);
  payload.sensitivityX100 = getU16(p + 12);
  payload.uptimeSec = getU32(p + 14);
  return true;
}

size_t encodeLedCommand(uint8_t *out, size_t capacity, uint16_t sequence,
                        const LedCommandPayload &payload) {
  const size_t total = HEADER_BYTES + COMMAND_PAYLOAD_BYTES;
  if (!out || capacity < total) return 0;
  writeHeader(out, PacketType::LedCommand, NodeType::Controller, 0, sequence,
              COMMAND_PAYLOAD_BYTES);
  uint8_t *p = out + HEADER_BYTES;
  p[0] = payload.lightsOn ? 1U : 0U;
  p[1] = payload.brightness;
  p[2] = payload.mode;
  putU24(p + 3, payload.color);
  putU16(p + 6, payload.sensitivityX100);
  return total;
}

bool decodeLedCommand(const uint8_t *buffer, size_t length,
                      LedCommandPayload &payload) {
  if (!validateEnvelope(buffer, length, PacketType::LedCommand,
                        COMMAND_PAYLOAD_BYTES)) {
    return false;
  }
  const uint8_t *p = buffer + HEADER_BYTES;
  if (!validMode(p[2])) return false;
  payload.lightsOn = p[0] != 0U;
  payload.brightness = p[1];
  payload.mode = p[2];
  payload.color = getU24(p + 3);
  payload.sensitivityX100 = getU16(p + 6);
  return true;
}

size_t encodeLedFrame(uint8_t *out, size_t capacity, uint16_t sequence,
                      const LedFramePayload &payload) {
  const size_t total = HEADER_BYTES + FRAME_PAYLOAD_BYTES;
  if (!out || capacity < total) return 0;
  writeHeader(out, PacketType::LedFrame, NodeType::Controller,
              payload.frameFlags, sequence, FRAME_PAYLOAD_BYTES);
  uint8_t *p = out + HEADER_BYTES;
  p[0] = payload.mode;
  p[1] = payload.brightness;
  p[2] = payload.frameFlags;
  memcpy(p + 3, payload.rgb, FRAME_RGB_BYTES);
  return total;
}

bool decodeLedFrame(const uint8_t *buffer, size_t length,
                    LedFramePayload &payload) {
  if (!validateEnvelope(buffer, length, PacketType::LedFrame,
                        FRAME_PAYLOAD_BYTES)) {
    return false;
  }
  const uint8_t *p = buffer + HEADER_BYTES;
  if (!validMode(p[0])) return false;
  payload.mode = p[0];
  payload.brightness = p[1];
  payload.frameFlags = p[2];
  memcpy(payload.rgb, p + 3, FRAME_RGB_BYTES);
  return true;
}

size_t encodeLedStateSync(uint8_t *out, size_t capacity, uint16_t sequence,
                          const LedStateSyncPayload &payload) {
  const size_t total = HEADER_BYTES + STATE_SYNC_PAYLOAD_BYTES;
  if (!out || capacity < total) return 0;
  writeHeader(out, PacketType::LedStateSync, NodeType::LedMatrix,
              payload.syncFlags, sequence, STATE_SYNC_PAYLOAD_BYTES);
  uint8_t *p = out + HEADER_BYTES;
  p[0] = payload.lightsOn ? 1U : 0U;
  p[1] = payload.brightness;
  p[2] = payload.mode;
  putU24(p + 3, payload.color);
  putU16(p + 6, payload.sensitivityX100);
  p[8] = payload.syncFlags;
  return total;
}

bool decodeLedStateSync(const uint8_t *buffer, size_t length,
                        LedStateSyncPayload &payload) {
  if (!validateEnvelope(buffer, length, PacketType::LedStateSync,
                        STATE_SYNC_PAYLOAD_BYTES)) {
    return false;
  }
  const uint8_t *p = buffer + HEADER_BYTES;
  if (!validMode(p[2])) return false;
  payload.lightsOn = p[0] != 0U;
  payload.brightness = p[1];
  payload.mode = p[2];
  payload.color = getU24(p + 3);
  payload.sensitivityX100 = getU16(p + 6);
  payload.syncFlags = p[8];
  return true;
}

size_t encodeAck(uint8_t *out, size_t capacity, NodeType sender,
                 uint16_t sequence, const AckPayload &payload) {
  const size_t total = HEADER_BYTES + ACK_PAYLOAD_BYTES;
  if (!out || capacity < total) return 0;
  writeHeader(out, PacketType::Ack, sender, 0, sequence, ACK_PAYLOAD_BYTES);
  uint8_t *p = out + HEADER_BYTES;
  putU16(p, payload.ackedSequence);
  p[2] = static_cast<uint8_t>(payload.ackedType);
  p[3] = payload.status;
  return total;
}

bool decodeAck(const uint8_t *buffer, size_t length, AckPayload &payload) {
  if (!validateEnvelope(buffer, length, PacketType::Ack, ACK_PAYLOAD_BYTES)) {
    return false;
  }
  const uint8_t *p = buffer + HEADER_BYTES;
  payload.ackedSequence = getU16(p);
  payload.ackedType = static_cast<PacketType>(p[2]);
  payload.status = p[3];
  return true;
}

} // namespace node_protocol

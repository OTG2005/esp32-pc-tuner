#include "../shared/ControlProtocol.h"
#include "../shared/NetworkValidation.h"
#include "../shared/NodeProtocol.h"
#include "../shared/RuntimeResources.h"
#include "../controller_node/AudioEffects.h"
#include "../controller_node/AudioSelection.h"
#include "../led_node/IrControl.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static int checks = 0;
#define CHECK(test) do { ++checks; if (!(test)) { \
  std::fprintf(stderr, "FAILED line %d: %s\n", __LINE__, #test); std::exit(1); } } while (0)

static ControlCommand parse(const char *text) {
  ControlCommand command;
  const char *error = nullptr;
  CHECK(parseControlCommand(text, command, error));
  return command;
}
static void apply(ControlState &state, const char *text) {
  CHECK(applyControlCommand(state, parse(text), true).ok);
}
static void invalid(const char *text) {
  ControlCommand command;
  const char *error = nullptr;
  CHECK(!parseControlCommand(text, command, error));
  CHECK(error && *error);
}

static void testControlProtocol() {
  static_assert(runtime_resources::BLE_CONNECTIONS == 1, "One BLE controller");
  static_assert(runtime_resources::CLASSIC_CONNECTIONS == 1, "One music source");
  static_assert(runtime_resources::BLE_REPLY_DEPTH >= 3, "A status reply has three lines");
  static_assert(runtime_resources::WEB_REPLY_DEPTH == 1, "HTTP command handler is serialized");
  static_assert(runtime_resources::HTTP_CLIENT_SOCKETS >= 2, "HTTP socket capacity");
  static_assert(runtime_resources::HTTP_STACK_BYTES >= 4096, "HTTP stack capacity");
  static_assert(runtime_resources::NETWORK_STACK_BYTES >= 4096, "Wi-Fi stack capacity");
  static_assert(runtime_resources::LOOP_STACK_BYTES >= 6144, "Loop stack capacity");

  ControlState state;
  CHECK(state.lightsOn && state.brightness == 192 && state.volume == 55 &&
        state.source == LightingSource::Bluetooth);
  apply(state, "lights off");
  apply(state, "color deep-pink");
  CHECK(!state.lightsOn && state.mode == MODE_STATIC && state.color == 0xFF1493);
  apply(state, "effect spectrum");
  CHECK(!state.lightsOn && state.mode == MODE_AUDIO_SPECTRUM);
  apply(state, "lights on");
  CHECK(state.lightsOn && state.mode == MODE_AUDIO_SPECTRUM);
  apply(state, "  LiGhTs\tToGgLe \r\n");
  CHECK(!state.lightsOn);

  for (int value = 0; value <= 255; ++value) {
    apply(state, ("brightness " + std::to_string(value)).c_str());
    CHECK(state.brightness == value);
  }
  apply(state, "brightness up"); CHECK(state.brightness == 255);
  apply(state, "brightness 0"); apply(state, "brightness down"); CHECK(state.brightness == 0);

  for (size_t i = 0; i < NAMED_COLOR_COUNT; ++i) {
    apply(state, (std::string("color ") + NAMED_COLORS[i].name).c_str());
    CHECK(state.color == NAMED_COLORS[i].rgb && state.mode == MODE_STATIC);
  }
  apply(state, "color #00aAcC"); CHECK(state.color == 0x00AACC);

  for (size_t i = 0; i < NAMED_EFFECT_COUNT; ++i) {
    apply(state, (std::string("effect ") + NAMED_EFFECTS[i].name).c_str());
    CHECK(state.mode == NAMED_EFFECTS[i].mode);
    CHECK(std::string(effectName(state.mode)) == NAMED_EFFECTS[i].name);
  }

  for (int value = 0; value <= 127; ++value) {
    apply(state, ("volume " + std::to_string(value)).c_str());
    CHECK(state.volume == value);
  }
  apply(state, "volume up"); CHECK(state.volume == 127);
  apply(state, "volume 55"); apply(state, "mute on");
  CHECK(state.muted && state.volume == 55);
  apply(state, "volume up"); CHECK(state.muted && state.volume == 60);
  apply(state, "mute off"); CHECK(!state.muted && state.volume == 60);
  apply(state, "mute toggle"); CHECK(state.muted);
  apply(state, "downmix stereo"); CHECK(!state.mono);
  apply(state, "downmix mono"); CHECK(state.mono);
  apply(state, "sensitivity 1.25"); CHECK(std::fabs(state.sensitivity - 1.25f) < 0.0001f);
  apply(state, "source bluetooth"); CHECK(state.source == LightingSource::Bluetooth);
  apply(state, "settings reset");
  CHECK(state.lightsOn && state.brightness == 192 && state.mode == MODE_RAINBOW &&
        state.volume == 55 && !state.muted && state.mono);

  for (const char *bad : {"", "N", "lights", "lights yes", "brightness -1",
       "brightness 256", "volume 128", "sensitivity nan", "sensitivity 0",
       "sensitivity 3.01", "color #FFFFFG", "effect nonexistent",
       "source microphone", "pc power", "lights on\nsettings reset"}) invalid(bad);
  invalid(nullptr);

  // BLE line framing
  CommandLineBuffer line;
  std::vector<std::string> lines;
  int overflow = 0;
  auto receive = [&](const char *value) {
    if (value) lines.emplace_back(value); else ++overflow;
  };
  line.feed("bright", 6, receive); CHECK(lines.empty());
  line.feed("ness 192\n", 9, receive);
  CHECK(lines.size() == 1 && lines[0] == "brightness 192");
  std::string tooLong(120, 'x'); tooLong += "\nstatus\n";
  line.feed(tooLong.data(), tooLong.size(), receive);
  CHECK(overflow == 1 && lines.back() == "status");

  // IR bindings (now running on LED Node)
  CHECK(IR_BINDING_COUNT == 41);
  for (size_t i = 0; i < IR_BINDING_COUNT; ++i) {
    ControlCommand ir;
    CHECK(parseIrControl(IR_BINDINGS[i].code, false, ir));
    CHECK(applyControlCommand(state, ir, false).ok);
    CHECK(parseIrControl(IR_BINDINGS[i].code, true, ir) ==
          (IR_BINDINGS[i].code == 0x5C || IR_BINDINGS[i].code == 0x5D));
  }

  // Bluetooth PCM freshness
  CHECK(hasFreshBluetoothAudio(1000, 1, 701));
  CHECK(!hasFreshBluetoothAudio(1000, 1, 700));
  CHECK(!hasFreshBluetoothAudio(1000, 0, 990));
  CHECK(hasFreshBluetoothAudio(0x10U, 1, 0xFFFFFFF0U));

  // Wi-Fi credential validation
  const char *error = nullptr;
  CHECK(validWifiCredentials("home", "12345678", false, error));
  CHECK(!validWifiCredentials("", "12345678", false, error));
  CHECK(validWifiCredentials("home", "", true, error));
  CHECK(!validWifiCredentials("home", "short", false, error));
}

static void testNodeProtocol() {
  using namespace node_protocol;

  CHECK(isAudioEffectMode(MODE_AUDIO_SPARKLE));
  CHECK(isAudioEffectMode(MODE_AUDIO_PULSE));
  CHECK(isAudioEffectMode(MODE_AUDIO_WAVE));
  CHECK(isAudioEffectMode(MODE_AUDIO_SPECTRUM));
  CHECK(!isAudioEffectMode(MODE_STATIC));
  CHECK(!isAudioEffectMode(MODE_RAINBOW));
  CHECK(!isAudioEffectMode(MODE_FADE_7));

  CHECK(encodeSensitivity(1.25f) == 125U);
  CHECK(std::fabs(decodeSensitivity(125U) - 1.25f) < 0.001f);
  CHECK(encodeSensitivity(0.0f) == 10U);
  CHECK(encodeSensitivity(9.0f) == 300U);

  uint8_t buf[MAX_PACKET_BYTES];

  // Discover round-trip
  size_t len = encodeDiscover(buf, sizeof(buf), NodeType::Controller, 42);
  CHECK(len == HEADER_BYTES);
  PacketHeader hdr;
  CHECK(decodeHeader(buf, len, hdr));
  CHECK(hdr.type == PacketType::Discover && hdr.nodeType == NodeType::Controller &&
        hdr.sequence == 42 && hdr.payloadLength == 0);

  // NodeAnnounce round-trip
  NodeAnnouncePayload annIn;
  const uint8_t mac[6] = {0xAA, 0xBB, 0xCC, 0x11, 0x22, 0x33};
  std::memcpy(annIn.nodeId, mac, 6);
  annIn.lightsOn = true;
  annIn.brightness = 180;
  annIn.mode = MODE_RAINBOW;
  annIn.color = 0x123456;
  annIn.sensitivityX100 = 120;
  annIn.uptimeSec = 98765;
  len = encodeNodeAnnounce(buf, sizeof(buf), NodeType::LedMatrix, 7, annIn);
  CHECK(len > HEADER_BYTES);
  NodeAnnouncePayload annOut;
  CHECK(decodeNodeAnnounce(buf, len, annOut));
  CHECK(std::memcmp(annOut.nodeId, mac, 6) == 0);
  CHECK(annOut.lightsOn && annOut.brightness == 180 && annOut.mode == MODE_RAINBOW &&
        annOut.color == 0x123456 && annOut.sensitivityX100 == 120 && annOut.uptimeSec == 98765);

  // LedCommand round-trip
  LedCommandPayload cmdIn;
  cmdIn.lightsOn = false;
  cmdIn.brightness = 90;
  cmdIn.mode = MODE_COLOR_BLEND;
  cmdIn.color = 0xFF1493;
  cmdIn.sensitivityX100 = 160;
  len = encodeLedCommand(buf, sizeof(buf), 101, cmdIn);
  CHECK(len == HEADER_BYTES + 8);
  LedCommandPayload cmdOut;
  CHECK(decodeLedCommand(buf, len, cmdOut));
  CHECK(!cmdOut.lightsOn && cmdOut.brightness == 90 && cmdOut.mode == MODE_COLOR_BLEND &&
        cmdOut.color == 0xFF1493 && cmdOut.sensitivityX100 == 160);

  // LedFrame round-trip (64 RGB pixels = 192 bytes)
  LedFramePayload frmIn;
  frmIn.mode = MODE_AUDIO_SPECTRUM;
  frmIn.brightness = 210;
  frmIn.frameFlags = FRAME_FLAG_ACTIVE;
  for (size_t i = 0; i < FRAME_RGB_BYTES; ++i) frmIn.rgb[i] = static_cast<uint8_t>((i * 7U) & 0xFFU);
  len = encodeLedFrame(buf, sizeof(buf), 500, frmIn);
  CHECK(len == HEADER_BYTES + 3 + FRAME_RGB_BYTES);
  CHECK(len <= MAX_PACKET_BYTES);
  LedFramePayload frmOut;
  CHECK(decodeLedFrame(buf, len, frmOut));
  CHECK(frmOut.mode == MODE_AUDIO_SPECTRUM && frmOut.brightness == 210 &&
        frmOut.frameFlags == FRAME_FLAG_ACTIVE);
  CHECK(std::memcmp(frmOut.rgb, frmIn.rgb, FRAME_RGB_BYTES) == 0);

  // LedStateSync round-trip (IR remote -> Controller)
  LedStateSyncPayload syncIn;
  syncIn.lightsOn = true;
  syncIn.brightness = 64;
  syncIn.mode = MODE_AUDIO_WAVE;
  syncIn.color = 0x00FF50;
  syncIn.sensitivityX100 = 90;
  syncIn.syncFlags = SYNC_FLAG_MODE | SYNC_FLAG_SENSITIVITY;
  len = encodeLedStateSync(buf, sizeof(buf), 99, syncIn);
  LedStateSyncPayload syncOut;
  CHECK(decodeLedStateSync(buf, len, syncOut));
  CHECK(syncOut.lightsOn && syncOut.brightness == 64 && syncOut.mode == MODE_AUDIO_WAVE &&
        syncOut.color == 0x00FF50 && syncOut.sensitivityX100 == 90 &&
        syncOut.syncFlags == (SYNC_FLAG_MODE | SYNC_FLAG_SENSITIVITY));

  // Ack round-trip
  AckPayload ackIn;
  ackIn.ackedSequence = 101;
  ackIn.ackedType = PacketType::LedCommand;
  ackIn.status = 0;
  len = encodeAck(buf, sizeof(buf), NodeType::LedMatrix, 12, ackIn);
  AckPayload ackOut;
  CHECK(decodeAck(buf, len, ackOut));
  CHECK(ackOut.ackedSequence == 101 && ackOut.ackedType == PacketType::LedCommand &&
        ackOut.status == 0);

  // Reject corrupt magic, version, truncated length, and invalid mode
  buf[0] = 'X';
  CHECK(!decodeAck(buf, len, ackOut));
  len = encodeLedCommand(buf, sizeof(buf), 102, cmdIn);
  buf[4] = 99; // bad version
  CHECK(!decodeLedCommand(buf, len, cmdOut));
  len = encodeLedCommand(buf, sizeof(buf), 102, cmdIn);
  CHECK(!decodeLedCommand(buf, len - 1, cmdOut)); // truncated
  buf[HEADER_BYTES + 2] = 99; // invalid LightMode
  CHECK(!decodeLedCommand(buf, len, cmdOut));
}

static bool hasAnyLitPixel(const node_protocol::LedFramePayload &frame) {
  for (size_t i = 0; i < node_protocol::FRAME_RGB_BYTES; ++i) {
    if (frame.rgb[i] != 0) return true;
  }
  return false;
}

static void testAudioEffectRenderer() {
  AudioEffectRenderer renderer;
  renderer.seed(12345U);

  DisplayAudioLevels activeAudio;
  activeAudio.overall = 0.85f;
  activeAudio.bass = 0.90f;
  activeAudio.mid = 0.70f;
  activeAudio.treble = 0.60f;
  activeAudio.beat = 0.85f;
  activeAudio.active = true;

  node_protocol::LedFramePayload frame;
  const uint8_t audioModes[] = {
      MODE_AUDIO_SPARKLE, MODE_AUDIO_PULSE, MODE_AUDIO_WAVE, MODE_AUDIO_SPECTRUM};
  for (uint8_t mode : audioModes) {
    CHECK(renderer.render(mode, 192, activeAudio, frame));
    CHECK(frame.mode == mode);
    CHECK(frame.brightness == 192);
    CHECK(frame.frameFlags == node_protocol::FRAME_FLAG_ACTIVE);
    CHECK(hasAnyLitPixel(frame));
  }

  // Silence / inactive Bluetooth PCM clears the frame cleanly
  DisplayAudioLevels silentAudio = {};
  silentAudio.active = false;
  CHECK(!renderer.render(MODE_AUDIO_SPECTRUM, 192, silentAudio, frame));
  CHECK(frame.frameFlags == node_protocol::FRAME_FLAG_CLEAR);
  CHECK(!hasAnyLitPixel(frame));

  // Non-audio mode is not rendered by AudioEffectRenderer (handled on LED Node)
  CHECK(!renderer.render(MODE_RAINBOW, 192, activeAudio, frame));
  CHECK(frame.frameFlags == node_protocol::FRAME_FLAG_CLEAR);
  CHECK(!hasAnyLitPixel(frame));
}

int main() {
  testControlProtocol();
  testNodeProtocol();
  testAudioEffectRenderer();
  std::printf("PASS: %d native checks (control protocol, IR bindings, Wi-Fi NodeProtocol codecs, "
              "controller-side sound-reactive frame renderer)\n", checks);
  return 0;
}

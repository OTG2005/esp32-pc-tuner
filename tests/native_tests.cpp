#include "../sketch_may4a/ControlProtocol.h"
#include "../sketch_may4a/NetworkValidation.h"
#include "../sketch_may4a/IrControl.h"
#include "../sketch_may4a/AudioSelection.h"
#include "../sketch_may4a/RuntimeResources.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
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

int main() {
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
    std::string text = "brightness " + std::to_string(value);
    apply(state, text.c_str());
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
  CHECK(std::string(sourceName(state.source)) == "bluetooth");
  apply(state, "sensitivity balanced"); CHECK(std::fabs(state.sensitivity - 0.9f) < 0.0001f);
  const char *presets[] = {"low", "normal", "high", "maximum"};
  for (const char *preset : presets) {
    apply(state, (std::string("brightness ") + preset).c_str());
    apply(state, (std::string("volume ") + preset).c_str());
    apply(state, (std::string("sensitivity ") + preset).c_str());
  }
  for (const char *verb : {"play", "pause", "stop", "next", "previous"}) {
    const ControlCommand c = parse((std::string("audio ") + verb).c_str());
    CHECK(applyControlCommand(state, c, true).actions & ACTION_TRANSPORT);
    CHECK(!applyControlCommand(state, c, false).ok);
  }
  CHECK(applyControlCommand(state, parse("wifi reconnect"), true).actions & ACTION_WIFI_RECONNECT);
  CHECK(parse("wifi status").kind == CommandKind::WifiStatus);
  CHECK(parse("status").kind == CommandKind::Status);
  CHECK(parse("help").kind == CommandKind::Help);
  for (CommandKind query : {CommandKind::Status, CommandKind::WifiStatus, CommandKind::Help})
    CHECK(isReadOnlyCommand(query));
  CHECK(!isReadOnlyCommand(CommandKind::Lights));
  for (size_t i = 0; i < CONTROL_HELP_COUNT; ++i)
    CHECK(std::string(CONTROL_HELP_LINES[i]).size() < 256);
  CHECK(applyControlCommand(state, parse("calibrate audio"), true).actions & ACTION_CALIBRATE);
  apply(state, "settings reset");
  CHECK(state.lightsOn && state.brightness == 192 && state.mode == MODE_RAINBOW &&
        state.volume == 55 && !state.muted && state.mono);
  for (const char *bad : {"", "N", "lights", "lights yes", "lights on extra",
       "brightness -1", "brightness 256", "brightness 1.2", "brightness 9999999999999",
       "volume 128", "sensitivity nan", "sensitivity inf", "sensitivity 0",
       "sensitivity 3.01", "sensitivity 1e2", "sensitivity 1.2.3", "color #FFFFFG",
        "color #123", "effect nonexistent", "audio reboot", "source internet",
        "source automatic", "source microphone",
       "downmix unknown", "pc power", "wifi password", "settings wipe",
       "status extra", "help extra", "lights on\nsettings reset", "lights\non"}) invalid(bad);
  invalid(nullptr);
  invalid(std::string(96, 'a').c_str());
  invalid("color \xFF");

  CommandLineBuffer line;
  std::vector<std::string> lines;
  int overflow = 0;
  auto receive = [&](const char *value) {
    if (value) lines.emplace_back(value); else ++overflow;
  };
  line.feed("bright", 6, receive); CHECK(lines.empty());
  line.feed("ness 192\n", 9, receive);
  CHECK(lines.size() == 1 && lines[0] == "brightness 192");
  line.feed("lights off\r\ncolor red\n", 22, receive);
  CHECK(lines.size() == 3 && lines[1] == "lights off" && lines[2] == "color red");
  std::string tooLong(120, 'x'); tooLong += "\nstatus\n";
  line.feed(tooLong.data(), tooLong.size(), receive);
  CHECK(overflow == 1 && lines.back() == "status");
  line.feed("effect ", 7, receive); line.reset();
  line.feed("status\n", 7, receive); CHECK(lines.back() == "status");
  const char nul[] = {'l','i','g','h','t','s',' ','o','n','\0','\n'};
  line.feed(nul, sizeof(nul), receive); CHECK(overflow == 2);
  line.feed("lights off\rstatus\r\n", 19, receive);
  CHECK(lines[lines.size() - 2] == "lights off" && lines.back() == "status");
  for (const auto &binding : std::vector<IrBinding>(IR_BINDINGS, IR_BINDINGS + IR_BINDING_COUNT)) {
    const std::string framed = std::string(binding.command) + "\n";
    for (size_t split = 0; split <= framed.size(); ++split) {
      CommandLineBuffer fragments;
      std::vector<std::string> received;
      auto collect = [&](const char *text) { CHECK(text); received.emplace_back(text); };
      fragments.feed(framed.data(), split, collect);
      fragments.feed(framed.data() + split, framed.size() - split, collect);
      CHECK(received.size() == 1 && received[0] == binding.command);
    }
    ControlCommand ir;
    CHECK(parseIrControl(binding.code, false, ir));
    CHECK(applyControlCommand(state, ir, true).ok);
    CHECK(parseIrControl(binding.code, true, ir) ==
          (binding.code == 0x5C || binding.code == 0x5D));
  }
  CHECK(IR_BINDING_COUNT == 41);
  ControlCommand ir;
  CHECK(parseIrControl(0x5C, true, ir) && ir.relative && ir.value == 10);
  CHECK(parseIrControl(0x5D, true, ir) && ir.relative && ir.value == -10);
  CHECK(parseIrControl(0x58, false, ir) && ir.color == 0xFF0028);
  CHECK(parseIrControl(0x59, false, ir) && ir.color == 0x00FF50);
  CHECK(parseIrControl(0x45, false, ir) && ir.color == 0x0050FF);
  CHECK(!parseIrControl(0xFFFF, false, ir));

  CHECK(hasFreshBluetoothAudio(1000, 1, 701)); // 299 ms old.
  CHECK(!hasFreshBluetoothAudio(1000, 1, 700)); // Exactly 300 ms => no input.
  CHECK(!hasFreshBluetoothAudio(1000, 0, 990)); // No decoded PCM yet.
  CHECK(hasFreshBluetoothAudio(0x10U, 1, 0xFFFFFFF0U)); // millis() wrap.
  CHECK(!hasFreshBluetoothAudio(0x200U, 1, 0xFFFFFFF0U));
  CHECK(hasFreshBluetoothAudio(299, 1, 0)); // A valid sample at boot timestamp 0.
  CHECK(!hasFreshBluetoothAudio(0, 0, 0));

  const char *error;
  CHECK(validWifiCredentials("home", "12345678", false, error));
  CHECK(!validWifiCredentials("", "12345678", false, error));
  CHECK(!validWifiCredentials("home", "", false, error));
  CHECK(validWifiCredentials("home", "", true, error));
  CHECK(!validWifiCredentials("home", "short", false, error));
  CHECK(!validWifiCredentials(std::string(33, 's').c_str(), "12345678", false, error));
  CHECK(validWifiCredentials("home", std::string(64, 'a').c_str(), false, error));
  CHECK(!validWifiCredentials("home", std::string(64, 'z').c_str(), false, error));
  CHECK(!validWifiCredentials("home\nbad", "12345678", false, error));
  CHECK(!validWifiCredentials("home", "12345\n789", false, error));
  CHECK(!validWifiCredentials("home", "12345678", true, error));
  CHECK(!validWifiCredentials("home", "passw\xC3\xB6rd", false, error));
  CHECK(validWifiCredentials(std::string(32, 's').c_str(), "12345678", false, error));
  std::printf("PASS: %d native checks (parser, reducer, BLE framing, all IR buttons, "
               "Bluetooth-only freshness, bounded Wi-Fi input)\n", checks);
}

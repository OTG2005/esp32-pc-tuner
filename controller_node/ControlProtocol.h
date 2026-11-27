#pragma once

// Portable control logic: compiled unchanged by the ESP32 and native tests.
#include <stddef.h>
#include <stdint.h>

constexpr size_t COMMAND_CAPACITY = 96;

enum LightMode : uint8_t {
  MODE_STATIC = 1, MODE_RAINBOW = 5, MODE_BLINK = 6, MODE_COLOR_BLEND = 7,
  MODE_FADE_3 = 8, MODE_FADE_7 = 9, MODE_AUDIO_SPARKLE = 10,
  MODE_AUDIO_PULSE = 11, MODE_AUDIO_WAVE = 12, MODE_AUDIO_SPECTRUM = 13
};

enum class LightingSource : uint8_t { Bluetooth };
enum class CommandKind : uint8_t {
  Lights, Brightness, Color, Effect, Sensitivity, Source, Volume, Mute,
  Downmix, Audio, WifiReconnect, Status, Help, Reset, Calibrate, WifiStatus
};
enum class TransportAction : uint8_t { Play, Pause, Stop, Next, Previous };

struct ControlCommand {
  CommandKind kind = CommandKind::Status;
  int value = 0;
  float decimal = 0;
  uint32_t color = 0;
  bool relative = false;
};

struct ControlState {
  bool lightsOn = true;
  uint8_t brightness = 192;
  uint8_t mode = MODE_RAINBOW;
  uint32_t color = 0xFFFFFF;
  float sensitivity = 1.0f;
  LightingSource source = LightingSource::Bluetooth;
  uint8_t volume = 55; // AVRCP scale: 0..127, independent of LED brightness.
  bool muted = false;
  bool mono = true;
};

enum ControlAction : uint16_t {
  ACTION_NONE = 0, ACTION_LIGHTS = 1, ACTION_VOLUME = 2, ACTION_DOWNMIX = 4,
  ACTION_TRANSPORT = 8, ACTION_WIFI_RECONNECT = 16, ACTION_CALIBRATE = 32,
  ACTION_HELP = 64
};

struct ControlResult {
  bool ok = false;
  uint16_t actions = ACTION_NONE;
  const char *message = "unknown command";
};

struct NamedColor { const char *name; const char *label; uint32_t rgb; };
struct NamedEffect { const char *name; const char *label; uint8_t mode; };

extern const NamedColor NAMED_COLORS[];
extern const size_t NAMED_COLOR_COUNT;
extern const NamedEffect NAMED_EFFECTS[];
extern const size_t NAMED_EFFECT_COUNT;
extern const char *const CONTROL_HELP_LINES[];
extern const size_t CONTROL_HELP_COUNT;

bool parseControlCommand(const char *text, ControlCommand &command,
                         const char *&error);
ControlResult applyControlCommand(ControlState &state, const ControlCommand &command,
                                  bool playbackAvailable);
bool isReadOnlyCommand(CommandKind kind);
const char *effectName(uint8_t mode);
const char *sourceName(LightingSource source);
const char *commandName(CommandKind kind);

// Length-bounded UART/BLE framing. LF, CRLF or CR commits a complete command.
// Overlong lines are discarded in full, never truncated into valid commands.
class CommandLineBuffer {
 public:
  template <typename Callback>
  void feed(const char *bytes, size_t length, Callback callback) {
    for (size_t i = 0; i < length; ++i) {
      const char ch = bytes[i];
      if (ch == '\n' || ch == '\r') {
        if (overflow_) callback(nullptr);
        else if (used_) { data_[used_] = '\0'; callback(data_); }
        reset();
      } else {
        if (ch == '\0' || used_ >= COMMAND_CAPACITY - 1) overflow_ = true;
        if (!overflow_) data_[used_++] = ch;
      }
    }
  }
  void reset() { used_ = 0; overflow_ = false; }
 private:
  char data_[COMMAND_CAPACITY] = {};
  size_t used_ = 0;
  bool overflow_ = false;
};

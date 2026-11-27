#include "ControlProtocol.h"
#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

const NamedColor NAMED_COLORS[] = {
  {"white", "White", 0xFFFFFF}, {"red", "Red", 0xFF0000},
  {"green", "Green", 0x00FF00}, {"blue", "Blue", 0x0000FF},
  {"pink", "Pink", 0xFF69B4}, {"deep-pink", "Deep pink", 0xFF1493},
  {"cyan", "Cyan", 0x00FFFF}, {"turquoise", "Turquoise", 0x40E0D0},
  {"indigo", "Indigo", 0x4B00FF}, {"purple", "Purple", 0x8000FF},
  {"orchid", "Orchid", 0xBA55D3}, {"magenta", "Magenta", 0xFF00FF},
  {"orange", "Orange", 0xFF5000}, {"amber", "Amber", 0xFF8C00},
  {"gold", "Gold", 0xFFC800}, {"yellow", "Yellow", 0xFFFF28},
  {"lime", "Lime", 0x78FF00}, {"sky-blue", "Sky blue", 0x00B4FF},
  {"teal", "Teal", 0x0080B4}, {"mint", "Mint", 0x00FF8C},
  // Keep the original remote's tinted RGB buttons as well as pure RGB.
  {"rose-red", "Rose red (IR)", 0xFF0028},
  {"spring-green", "Spring green (IR)", 0x00FF50},
  {"azure-blue", "Azure blue (IR)", 0x0050FF}
};
const size_t NAMED_COLOR_COUNT = sizeof(NAMED_COLORS) / sizeof(NAMED_COLORS[0]);
const NamedEffect NAMED_EFFECTS[] = {
  {"static", "Static color", MODE_STATIC}, {"rainbow", "Rainbow", MODE_RAINBOW},
  {"blink", "RGB blink", MODE_BLINK}, {"blend", "Color blend", MODE_COLOR_BLEND},
  {"fade-three", "Three-color fade", MODE_FADE_3},
  {"fade-seven", "Seven-color fade", MODE_FADE_7},
  {"sparkle", "Music sparkle", MODE_AUDIO_SPARKLE},
  {"pulse", "Music pulse", MODE_AUDIO_PULSE},
  {"wave", "Music wave", MODE_AUDIO_WAVE},
  {"spectrum", "Music spectrum", MODE_AUDIO_SPECTRUM}
};
const size_t NAMED_EFFECT_COUNT = sizeof(NAMED_EFFECTS) / sizeof(NAMED_EFFECTS[0]);
// Each line fits a CommandReply. BLE streams these without filling its queue.
const char *const CONTROL_HELP_LINES[] = {
  "help: commands are ASCII words, case-insensitive; end each with LF/CRLF/CR. "
  "lights on|off|toggle; brightness up|down|low|normal|high|maximum|0..255.\n",
  "color white|red|green|blue|pink|deep-pink|cyan|turquoise|indigo|purple|orchid|"
  "magenta|orange|amber|gold|yellow|lime|sky-blue|teal|mint|rose-red|spring-green|"
  "azure-blue, or color #RRGGBB.\n",
  "effect static|rainbow|blink|blend|fade-three|fade-seven|sparkle|pulse|wave|spectrum.\n",
  "sensitivity low|balanced|normal|high|maximum|0.1..3.0; "
  "source bluetooth (the only lighting input); calibrate audio.\n",
  "volume up|down|low|normal|high|maximum|0..127; mute on|off|toggle; "
  "downmix mono|stereo; audio play|pause|stop|next|previous.\n",
  "wifi reconnect; wifi status; settings reset; status; help. "
  "Wi-Fi details: web setup only. No web login; protect Internet access externally. "
  "PC power is not implemented.\n"
};
const size_t CONTROL_HELP_COUNT = sizeof(CONTROL_HELP_LINES) / sizeof(CONTROL_HELP_LINES[0]);

namespace {
bool equal(const char *a, const char *b) { return strcmp(a, b) == 0; }
bool integer(const char *s, int maximum, int &out) {
  if (!*s) return false;
  int value = 0;
  for (; *s; ++s) {
    if (*s < '0' || *s > '9') return false;
    value = value * 10 + (*s - '0');
    if (value > maximum) return false;
  }
  out = value;
  return true;
}
bool toggleValue(const char *s, int &out) {
  if (equal(s, "on")) out = 1;
  else if (equal(s, "off")) out = 0;
  else if (equal(s, "toggle")) out = 2;
  else return false;
  return true;
}
bool level(const char *s, int maximum, int step, int low, int normal, int high,
           ControlCommand &command) {
  if (equal(s, "up") || equal(s, "down")) {
    command.relative = true;
    command.value = equal(s, "up") ? step : -step;
  } else if (equal(s, "low")) command.value = low;
  else if (equal(s, "normal")) command.value = normal;
  else if (equal(s, "high")) command.value = high;
  else if (equal(s, "maximum")) command.value = maximum;
  else if (!integer(s, maximum, command.value)) return false;
  return true;
}
int bound(int value, int maximum) {
  return value < 0 ? 0 : (value > maximum ? maximum : value);
}
}

bool parseControlCommand(const char *text, ControlCommand &out, const char *&error) {
  out = ControlCommand();
  error = "invalid command";
  if (!text) { error = "missing command"; return false; }
  char line[COMMAND_CAPACITY];
  size_t n = 0;
  while (text[n]) {
    if (n >= sizeof(line) - 1) { error = "command too long"; return false; }
    const unsigned char ch = static_cast<unsigned char>(text[n]);
    if (ch >= 127 || (ch < 32 && ch != '\t' && ch != '\r' && ch != '\n')) {
      error = "command must contain printable ASCII words"; return false;
    }
    line[n++] = static_cast<char>(tolower(ch));
  }
  line[n] = '\0';
  char *verb = line;
  while (isspace(static_cast<unsigned char>(*verb))) ++verb;
  char *end = verb + strlen(verb);
  while (end > verb && isspace(static_cast<unsigned char>(end[-1]))) *--end = '\0';
  if (!*verb) { error = "empty command"; return false; }
  if (strchr(verb, '\r') || strchr(verb, '\n')) {
    error = "send one command per line"; return false;
  }
  char *arg = verb;
  while (*arg && !isspace(static_cast<unsigned char>(*arg))) ++arg;
  if (*arg) *arg++ = '\0';
  while (isspace(static_cast<unsigned char>(*arg))) ++arg;
  for (const char *p = arg; *p; ++p) {
    if (isspace(static_cast<unsigned char>(*p))) {
      error = "expected one parameter"; return false;
    }
  }

  if (equal(verb, "status") || equal(verb, "help")) {
    if (*arg) { error = "this command takes no parameter"; return false; }
    out.kind = equal(verb, "status") ? CommandKind::Status : CommandKind::Help;
    return true;
  }
  if (!*arg) { error = "missing parameter"; return false; }
  if (equal(verb, "lights") || equal(verb, "mute")) {
    out.kind = equal(verb, "lights") ? CommandKind::Lights : CommandKind::Mute;
    if (!toggleValue(arg, out.value)) { error = "use on, off or toggle"; return false; }
  } else if (equal(verb, "brightness") || equal(verb, "volume")) {
    const bool led = equal(verb, "brightness");
    out.kind = led ? CommandKind::Brightness : CommandKind::Volume;
    if (!level(arg, led ? 255 : 127, led ? 20 : 5,
               led ? 64 : 25, led ? 128 : 55, led ? 192 : 90, out)) {
      error = led ? "brightness must be 0..255 or a level word"
                  : "volume must be 0..127 or a level word";
      return false;
    }
  } else if (equal(verb, "color")) {
    out.kind = CommandKind::Color;
    bool found = false;
    for (size_t i = 0; i < NAMED_COLOR_COUNT; ++i) {
      if (equal(arg, NAMED_COLORS[i].name)) { out.color = NAMED_COLORS[i].rgb; found = true; }
    }
    if (!found && strlen(arg) == 7 && arg[0] == '#') {
      found = true;
      uint32_t rgb = 0;
      for (int i = 1; i < 7; ++i) {
        if (!isxdigit(static_cast<unsigned char>(arg[i]))) { found = false; break; }
        rgb = (rgb << 4) | (arg[i] <= '9' ? arg[i] - '0' : arg[i] - 'a' + 10);
      }
      out.color = rgb;
    }
    if (!found) { error = "unknown color; use a color name or #RRGGBB"; return false; }
  } else if (equal(verb, "effect")) {
    out.kind = CommandKind::Effect;
    bool found = false;
    for (size_t i = 0; i < NAMED_EFFECT_COUNT; ++i) {
      if (equal(arg, NAMED_EFFECTS[i].name)) { out.value = NAMED_EFFECTS[i].mode; found = true; }
    }
    if (!found) { error = "unknown effect"; return false; }
  } else if (equal(verb, "sensitivity")) {
    out.kind = CommandKind::Sensitivity;
    if (equal(arg, "low")) out.decimal = 0.65f;
    else if (equal(arg, "balanced")) out.decimal = 0.9f;
    else if (equal(arg, "normal")) out.decimal = 1.0f;
    else if (equal(arg, "high")) out.decimal = 1.2f;
    else if (equal(arg, "maximum")) out.decimal = 1.6f;
    else {
      for (const char *p = arg; *p; ++p) {
        if ((*p < '0' || *p > '9') && *p != '.') {
          error = "sensitivity must be 0.1..3.0 or a preset"; return false;
        }
      }
      char *tail = nullptr;
      out.decimal = strtof(arg, &tail);
      if (!tail || *tail || !isfinite(out.decimal) ||
          out.decimal < 0.1f || out.decimal > 3.0f) {
        error = "sensitivity must be 0.1..3.0"; return false;
      }
    }
  } else if (equal(verb, "source")) {
    out.kind = CommandKind::Source;
    if (equal(arg, "bluetooth")) out.value = static_cast<int>(LightingSource::Bluetooth);
    else { error = "Bluetooth is the only lighting source"; return false; }
  } else if (equal(verb, "downmix")) {
    out.kind = CommandKind::Downmix;
    if (equal(arg, "mono")) out.value = 1;
    else if (equal(arg, "stereo")) out.value = 0;
    else { error = "use mono or stereo"; return false; }
  } else if (equal(verb, "audio")) {
    out.kind = CommandKind::Audio;
    const char *names[] = {"play", "pause", "stop", "next", "previous"};
    bool found = false;
    for (int i = 0; i < 5; ++i) {
      if (equal(arg, names[i])) { out.value = i; found = true; }
    }
    if (!found) { error = "use play, pause, stop, next or previous"; return false; }
  } else if (equal(verb, "wifi") && equal(arg, "reconnect")) {
    out.kind = CommandKind::WifiReconnect;
  } else if (equal(verb, "wifi") && equal(arg, "status")) {
    out.kind = CommandKind::WifiStatus;
  } else if (equal(verb, "settings") && equal(arg, "reset")) {
    out.kind = CommandKind::Reset;
  } else if (equal(verb, "calibrate") && equal(arg, "audio")) {
    out.kind = CommandKind::Calibrate;
  } else {
    error = "unknown command; send help"; return false;
  }
  return true;
}

ControlResult applyControlCommand(ControlState &state, const ControlCommand &cmd,
                                  bool playbackAvailable) {
  ControlResult result;
  result.ok = true;
  result.message = "applied";
  switch (cmd.kind) {
    case CommandKind::Lights:
      state.lightsOn = cmd.value == 2 ? !state.lightsOn : cmd.value == 1;
      result.actions = ACTION_LIGHTS; break;
    case CommandKind::Brightness:
      state.brightness = static_cast<uint8_t>(bound(
          cmd.relative ? state.brightness + cmd.value : cmd.value, 255));
      result.actions = ACTION_LIGHTS; break;
    case CommandKind::Color:
      state.color = cmd.color;
      state.mode = MODE_STATIC;
      result.actions = ACTION_LIGHTS; break;
    case CommandKind::Effect:
      state.mode = static_cast<uint8_t>(cmd.value);
      result.actions = ACTION_LIGHTS; break;
    case CommandKind::Sensitivity: state.sensitivity = cmd.decimal; break;
    case CommandKind::Source:
      state.source = static_cast<LightingSource>(cmd.value);
      result.actions = ACTION_CALIBRATE; break;
    case CommandKind::Volume:
      state.volume = static_cast<uint8_t>(bound(
          cmd.relative ? state.volume + cmd.value : cmd.value, 127));
      result.actions = ACTION_VOLUME; break;
    case CommandKind::Mute:
      state.muted = cmd.value == 2 ? !state.muted : cmd.value == 1;
      result.actions = ACTION_VOLUME; break;
    case CommandKind::Downmix:
      state.mono = cmd.value != 0; result.actions = ACTION_DOWNMIX; break;
    case CommandKind::Audio:
      if (!playbackAvailable) {
        result.ok = false; result.message = "Bluetooth playback control is not connected";
      } else {
        result.actions = ACTION_TRANSPORT; result.message = "queued for the paired Bluetooth source";
      }
      break;
    case CommandKind::WifiReconnect:
      result.actions = ACTION_WIFI_RECONNECT;
      result.message = "Wi-Fi reconnect requested"; break;
    case CommandKind::Reset:
      state = ControlState();
      result.actions = ACTION_LIGHTS | ACTION_VOLUME | ACTION_DOWNMIX | ACTION_CALIBRATE;
      result.message = "controls reset; Wi-Fi credentials retained"; break;
    case CommandKind::Calibrate:
      result.actions = ACTION_CALIBRATE;
      result.message = "audio meter recalibration requested"; break;
    case CommandKind::Help: result.actions = ACTION_HELP; break;
    case CommandKind::Status: result.message = "status"; break;
    case CommandKind::WifiStatus: result.message = "network status"; break;
  }
  return result;
}

bool isReadOnlyCommand(CommandKind kind) {
  return kind == CommandKind::Status || kind == CommandKind::Help ||
         kind == CommandKind::WifiStatus;
}

const char *effectName(uint8_t mode) {
  for (size_t i = 0; i < NAMED_EFFECT_COUNT; ++i)
    if (NAMED_EFFECTS[i].mode == mode) return NAMED_EFFECTS[i].name;
  return "unknown";
}
const char *sourceName(LightingSource source) {
  switch (source) {
    case LightingSource::Bluetooth: return "bluetooth";
  }
  return "unknown";
}
const char *commandName(CommandKind kind) {
  const char *names[] = {"lights", "brightness", "color", "effect", "sensitivity",
    "source", "volume", "mute", "downmix", "audio", "wifi", "status", "help",
    "settings", "calibrate", "wifi"};
  const size_t index = static_cast<size_t>(kind);
  return index < sizeof(names) / sizeof(names[0]) ? names[index] : "unknown";
}

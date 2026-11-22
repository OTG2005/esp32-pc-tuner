#include "../sketch_may4a/ControlProtocol.h"
#include "../sketch_may4a/NetworkValidation.h"
#include "../sketch_may4a/IrControl.h"
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#endif

static std::string quote(const std::string &value) {
  std::string out = "\"";
  for (unsigned char ch : value) {
    if (ch == '"' || ch == '\\') { out += '\\'; out += static_cast<char>(ch); }
    else if (ch == '\n') out += "\\n";
    else if (ch == '\r') out += "\\r";
    else if (ch == '\t') out += "\\t";
    else if (ch >= 32) out += static_cast<char>(ch);
  }
  return out + '"';
}
static std::string stateJson(const ControlState &state) {
  std::ostringstream rgb;
  rgb << '#' << std::uppercase << std::hex << std::setw(6) << std::setfill('0') << state.color;
  std::ostringstream out;
  out << std::boolalpha << "{\"lights\":" << state.lightsOn
      << ",\"brightness\":" << static_cast<int>(state.brightness)
      << ",\"effect\":" << quote(effectName(state.mode))
      << ",\"color\":" << quote(rgb.str())
      << ",\"sensitivity\":" << state.sensitivity
      << ",\"source\":" << quote(sourceName(state.source))
      << ",\"volume\":" << static_cast<int>(state.volume)
      << ",\"muted\":" << state.muted
      << ",\"downmix\":" << quote(state.mono ? "mono" : "stereo") << '}';
  return out.str();
}

int main() {
#ifdef _WIN32
  _setmode(_fileno(stdin), _O_BINARY);
  _setmode(_fileno(stdout), _O_BINARY);
#endif
  ControlState state;
  bool connected = true, playing = false, transport = true;
  uint32_t revision = 0;
  CommandLineBuffer bleLines;
  std::string lengthLine;
  while (std::getline(std::cin, lengthLine)) {
    const size_t length = static_cast<size_t>(std::stoul(lengthLine));
    if (length > 8192) return 2;
    std::string request(length, '\0');
    std::cin.read(&request[0], static_cast<std::streamsize>(length));
    if (!std::cin) return 3;
    std::string response;
    if (request.rfind("validate-wifi:", 0) == 0) {
      const bool open = request[14] == '1';
      const size_t separator = request.find('\n', 16);
      if (separator == std::string::npos) return 5;
      const std::string ssid = request.substr(16, separator - 16), password = request.substr(separator + 1);
      const char *error = nullptr;
      const bool ok = validWifiCredentials(ssid.c_str(), password.c_str(), open, error);
      response = ok ? "{\"ok\":true}" : "{\"ok\":false,\"error\":" + quote(error) + '}';
    } else if (request.rfind("test:", 0) == 0) {
      connected = request == "test:connected" || request == "test:connected-no-transport";
      transport = request == "test:connected";
      if (!connected) playing = false;
      response = "{\"ok\":true}";
    } else if (request.rfind("ble:", 0) == 0) {
      int accepted = 0, rejected = 0;
      const std::string bytes = request.substr(4);
      bleLines.feed(bytes.data(), bytes.size(), [&](const char *line) {
        ControlCommand command;
        const char *error = nullptr;
        if (!line || !parseControlCommand(line, command, error)) { ++rejected; return; }
        const ControlResult result = applyControlCommand(state, command, connected && transport);
        if (!result.ok) { ++rejected; return; }
        ++accepted;
        if (!isReadOnlyCommand(command.kind)) ++revision;
        if (command.kind == CommandKind::Audio) {
          if (command.value == 0) playing = true;
          if (command.value == 1 || command.value == 2) playing = false;
        }
      });
      response = "{\"accepted\":" + std::to_string(accepted) + ",\"rejected\":" +
          std::to_string(rejected) + ",\"controls\":" + stateJson(state) + '}';
    } else if (request == "catalog") {
      std::ostringstream out;
      out << "{\"colors\":[";
      for (size_t i = 0; i < NAMED_COLOR_COUNT; ++i) {
        if (i) out << ',';
        std::ostringstream color;
        color << '#' << std::uppercase << std::hex << std::setw(6) << std::setfill('0') << NAMED_COLORS[i].rgb;
        out << "{\"name\":" << quote(NAMED_COLORS[i].name) << ",\"label\":"
            << quote(NAMED_COLORS[i].label) << ",\"hex\":" << quote(color.str()) << '}';
      }
      out << "],\"effects\":[";
      for (size_t i = 0; i < NAMED_EFFECT_COUNT; ++i) {
        if (i) out << ',';
        out << "{\"name\":" << quote(NAMED_EFFECTS[i].name) << ",\"label\":" << quote(NAMED_EFFECTS[i].label) << '}';
      }
      out << "],\"ir\":[";
      for (size_t i = 0; i < IR_BINDING_COUNT; ++i) {
        if (i) out << ',';
        out << "{\"button\":" << quote(IR_BINDINGS[i].button) << ",\"code\":"
            << IR_BINDINGS[i].code << ",\"command\":" << quote(IR_BINDINGS[i].command) << '}';
      }
      out << "],\"help\":[";
      for (size_t i = 0; i < CONTROL_HELP_COUNT; ++i) {
        if (i) out << ',';
        out << quote(CONTROL_HELP_LINES[i]);
      }
      out << "]}"; response = out.str();
    } else {
      ControlCommand command;
      const char *error = nullptr;
      const std::string text = request.rfind("command:", 0) == 0 ? request.substr(8) : "status";
      if (!parseControlCommand(text.c_str(), command, error)) {
        response = "{\"ok\":false,\"invalid\":true,\"error\":" + quote(error) + '}';
      } else {
        const ControlResult result = applyControlCommand(state, command, connected && transport);
        if (result.ok && !isReadOnlyCommand(command.kind)) ++revision;
        if (result.ok && command.kind == CommandKind::Audio) {
          if (command.value == 0) playing = true;
          if (command.value == 1 || command.value == 2) playing = false;
        }
        std::ostringstream out;
        out << std::boolalpha << "{\"ok\":" << result.ok << ",\"message\":" << quote(result.message)
            << ",\"actions\":" << result.actions << ",\"controls\":" << stateJson(state)
             << ",\"connected\":" << connected << ",\"playing\":" << playing
             << ",\"transport\":" << (connected && transport)
             << ",\"revision\":" << revision;
        if (command.kind == CommandKind::Help) {
          out << ",\"help\":[";
          for (size_t i = 0; i < CONTROL_HELP_COUNT; ++i) {
            if (i) out << ',';
            out << quote(CONTROL_HELP_LINES[i]);
          }
          out << ']';
        }
        out << '}';
        response = out.str();
      }
    }
    std::cout << response << '\n' << std::flush;
  }
}

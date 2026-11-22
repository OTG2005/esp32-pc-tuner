#include "WebControl.h"
#include "ControlBridge.h"
#include "NetworkValidation.h"
#include "RuntimeResources.h"
#include "TunerWiFi.h"
#include "WebPage.h"
#if __has_include("LocalConfig.h")
#include "LocalConfig.h"
#else
#include "LocalConfig.example.h"
#endif
#include <Arduino.h>
#include <Preferences.h>
#include <esp_http_server.h>
#include <mdns.h>
#include <atomic>

namespace {
httpd_handle_t server = nullptr;
std::atomic<bool> reconnectRequested{false}, serviceReady{false}, wifiUpdatePending{false};
QueueHandle_t wifiChanges = nullptr;
struct WifiChange { uint8_t version = 1; char ssid[33]; char password[65]; bool open; };
portMUX_TYPE networkMux = portMUX_INITIALIZER_UNLOCKED;
WifiStatus networkStatus;

void headers(httpd_req_t *req) {
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_set_hdr(req, "X-PC-Tuner-Device", "esp32-pc-tuner");
}
esp_err_t text(httpd_req_t *req, const char *body, const char *status = "200 OK") {
  headers(req);
  httpd_resp_set_status(req, status);
  httpd_resp_set_type(req, "text/plain; charset=utf-8");
  return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}
bool body(httpd_req_t *req, char *output, size_t capacity) {
  // Length bounds prevent buffer corruption; no session, JSON, or deep schema checks.
  if (!req->content_len || req->content_len >= capacity) {
    text(req, "Input too long or empty.", "413 Payload Too Large"); return false;
  }
  size_t received = 0;
  while (received < req->content_len) {
    const int count = httpd_req_recv(req, output + received, req->content_len - received);
    if (count <= 0) { text(req, "Input timed out.", "408 Request Timeout"); return false; }
    received += count;
  }
  if (memchr(output, 0, received)) {
    text(req, "Invalid input.", "400 Bad Request"); return false;
  }
  output[received] = 0;
  return true;
}
int hex(char ch) {
  if (ch >= '0' && ch <= '9') return ch - '0';
  if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
  if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
  return -1;
}
bool field(const char *input, const char *name, char *output, size_t capacity) {
  output[0] = 0;
  const size_t nameLength = strlen(name);
  for (const char *part = input; part && *part;) {
    const char *end = strchr(part, '&');
    if (!end) end = part + strlen(part);
    if (static_cast<size_t>(end - part) > nameLength &&
        strncmp(part, name, nameLength) == 0 && part[nameLength] == '=') {
      size_t used = 0;
      for (const char *value = part + nameLength + 1; value < end; ++value) {
        char ch = *value;
        if (ch == '+') ch = ' ';
        else if (ch == '%') {
          if (value + 2 >= end || hex(value[1]) < 0 || hex(value[2]) < 0) return false;
          ch = static_cast<char>((hex(value[1]) << 4) | hex(value[2])); value += 2;
        }
        if (!ch || used + 1 >= capacity) return false;
        output[used++] = ch;
      }
      output[used] = 0; return true;
    }
    part = *end ? end + 1 : nullptr;
  }
  return false;
}
esp_err_t root(httpd_req_t *req) {
  headers(req);
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
  for (size_t offset = 0; offset < sizeof(WEB_PAGE_GZIP); offset += 512) {
    const size_t remaining = sizeof(WEB_PAGE_GZIP) - offset;
    const esp_err_t result = httpd_resp_send_chunk(req,
        reinterpret_cast<const char *>(WEB_PAGE_GZIP + offset), remaining < 512 ? remaining : 512);
    if (result != ESP_OK) return result;
    vTaskDelay(pdMS_TO_TICKS(5)); // Bound queued Wi-Fi/TCP buffers, not the audio task.
  }
  return httpd_resp_send_chunk(req, nullptr, 0);
}
esp_err_t status(httpd_req_t *req) {
  const DeviceStatus state = readDeviceStatus();
  const WifiStatus wifi = readWifiStatus();
  char output[640];
  snprintf(output, sizeof(output),
      "ESP32 PC Tuner\nlights=%s brightness=%u color=#%06lX effect=%s\n"
      "sensitivity=%.2f source=%s volume=%u mute=%s downmix=%s\n"
      "BT audio=%s playback=%s PCM=%s speaker=%s\n"
      "Wi-Fi=%s SSID=%s IP=%s RSSI=%d setup=%s\nheap=%lu uptime=%lus revision=%lu\n",
      state.controls.lightsOn ? "on" : "off", state.controls.brightness,
      static_cast<unsigned long>(state.controls.color), effectName(state.controls.mode),
      state.controls.sensitivity, sourceName(state.controls.source), state.controls.volume,
      state.controls.muted ? "on" : "off", state.controls.mono ? "mono" : "stereo",
      state.bluetoothConnected ? "connected" : "off", state.bluetoothPlaying ? "playing" : "paused",
      state.bluetoothPcmActive ? "active" : "idle", state.speakerReady ? "ready" : "off",
      wifi.connected ? "connected" : "off", wifi.ssid, wifi.ip, wifi.rssi,
      wifi.setupAp ? "open" : "off", static_cast<unsigned long>(state.freeHeap),
      static_cast<unsigned long>(state.uptime / 1000), static_cast<unsigned long>(state.revision));
  return text(req, output);
}
esp_err_t command(httpd_req_t *req) {
  char input[COMMAND_CAPACITY];
  if (!body(req, input, sizeof(input))) return ESP_FAIL;
  CommandReply reply;
  while (xQueueReceive(webReplyQueue, &reply, 0) == pdTRUE) {}
  uint32_t id; const char *error;
  if (!enqueueControl(input, CommandOrigin::Web, id, error))
    return text(req, error, "400 Bad Request");
  const uint32_t started = millis();
  while (static_cast<uint32_t>(millis() - started) < 1500) {
    if (xQueueReceive(webReplyQueue, &reply, pdMS_TO_TICKS(50)) == pdTRUE && reply.id == id) {
      if (reply.kind == CommandKind::Status || reply.kind == CommandKind::WifiStatus)
        return status(req);
      if (reply.kind == CommandKind::Help) {
        headers(req); httpd_resp_set_type(req, "text/plain; charset=utf-8");
        for (size_t i = 0; i < CONTROL_HELP_COUNT; ++i)
          if (httpd_resp_send_chunk(req, CONTROL_HELP_LINES[i], strlen(CONTROL_HELP_LINES[i])) != ESP_OK)
            return ESP_FAIL;
        return httpd_resp_send_chunk(req, nullptr, 0);
      }
      return text(req, reply.message, reply.ok ? "200 OK" : "409 Conflict");
    }
  }
  return text(req, "No acknowledgement; refresh status.", "504 Gateway Timeout");
}
esp_err_t wifi(httpd_req_t *req) {
  char input[384]; WifiChange change = {};
  if (!body(req, input, sizeof(input))) return ESP_FAIL;
  if (!field(input, "ssid", change.ssid, sizeof(change.ssid)) ||
      !field(input, "password", change.password, sizeof(change.password)))
    return text(req, "SSID/password too long or invalid.", "400 Bad Request");
  change.open = !change.password[0];
  const char *error = nullptr;
  if (!validWifiCredentials(change.ssid, change.password, change.open, error))
    return text(req, error, "400 Bad Request");
  if (wifiUpdatePending.exchange(true))
    return text(req, "Wi-Fi update already pending.", "503 Service Unavailable");
  if (xQueueSend(wifiChanges, &change, 0) != pdTRUE) {
    wifiUpdatePending = false;
    return text(req, "Wi-Fi queue busy.", "503 Service Unavailable");
  }
  memset(input, 0, sizeof(input)); memset(&change, 0, sizeof(change));
  return text(req, "Connecting to router. The open setup hotspot closes once an IP is obtained. "
      "Use the router IP printed in Serial Monitor.", "202 Accepted");
}
esp_err_t notFound(httpd_req_t *req, httpd_err_code_t) {
  return text(req, "Not found.", "404 Not Found");
}
bool startServer() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.stack_size = runtime_resources::HTTP_STACK_BYTES;
  config.max_open_sockets = runtime_resources::HTTP_CLIENT_SOCKETS;
  config.max_uri_handlers = 5;
  config.max_resp_headers = 4;
  config.lru_purge_enable = true;
  config.recv_wait_timeout = config.send_wait_timeout = 3;
  const esp_err_t result = httpd_start(&server, &config);
  if (result != ESP_OK) {
    Serial.printf("[web] start failed: %s\n", esp_err_to_name(result)); return false;
  }
  const struct { const char *uri; httpd_method_t method; esp_err_t (*handler)(httpd_req_t *); } routes[] = {
    {"/", HTTP_GET, root}, {"/api/status", HTTP_GET, status},
    {"/api/command", HTTP_POST, command}, {"/api/wifi", HTTP_POST, wifi}
  };
  for (const auto &route : routes) {
    httpd_uri_t uri = {};
    uri.uri = route.uri; uri.method = route.method; uri.handler = route.handler;
    if (httpd_register_uri_handler(server, &uri) != ESP_OK) {
      httpd_stop(server); server = nullptr; return false;
    }
  }
  httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, notFound);
  Serial.println(F("[web] lightweight controls ready; no device login"));
  logRuntimeMemory("after lightweight HTTP");
  return true;
}

void networkTask(void *) {
  Preferences prefs;
  const bool saved = prefs.begin("pc-network", false);
  WifiChange current = {};
  bool loaded = saved && prefs.getBytesLength("wifi") == sizeof(current) &&
      prefs.getBytes("wifi", &current, sizeof(current)) == sizeof(current) &&
      current.version == 1 && current.ssid[32] == 0 && current.password[64] == 0;
  if (!loaded) {
    current = WifiChange();
    String ssid = saved ? prefs.getString("ssid", local_config::WIFI_SSID) : String(local_config::WIFI_SSID);
    String password = saved ? prefs.getString("password", local_config::WIFI_PASSWORD) : String(local_config::WIFI_PASSWORD);
    strlcpy(current.ssid, ssid.c_str(), sizeof(current.ssid));
    strlcpy(current.password, password.c_str(), sizeof(current.password));
    current.open = current.password[0] == 0;
  }
  const char *error = nullptr;
  if (*current.ssid && !validWifiCredentials(current.ssid, current.password, current.open, error))
    current = WifiChange();
  bool setup = false, wasConnected = false, pendingSave = false, mdns = false;
  uint32_t attempt = millis();
  if (!tuner_wifi::initialize()) { Serial.println(F("[wifi] driver failed")); vTaskDelete(nullptr); return; }
  auto openSetup = [&]() {
    if (!setup) {
      setup = tuner_wifi::startSetup(local_config::SETUP_SSID, *current.ssid != 0);
      if (setup) {
        Serial.print(F("[wifi] OPEN setup hotspot: ")); Serial.println(tuner_wifi::setupName());
        Serial.println(F("[wifi] open http://192.168.4.1; no hotspot or web password"));
      }
    }
  };
  auto connect = [&]() {
    attempt = millis();
    if (*current.ssid) tuner_wifi::connect(current.ssid, current.password, current.open, setup);
    else openSetup();
  };
  connect(); serviceReady = startServer();
  for (;;) {
    WifiChange change;
    if (xQueueReceive(wifiChanges, &change, 0) == pdTRUE) {
      vTaskDelay(pdMS_TO_TICKS(1000)); // Let the acceptance reply leave first.
      if (mdns) { mdns_free(); mdns = false; }
      tuner_wifi::disconnect(); current = change; memset(&change, 0, sizeof(change));
      pendingSave = true; wasConnected = false; wifiUpdatePending = false; connect();
    } else if (reconnectRequested.exchange(false)) {
      if (mdns) { mdns_free(); mdns = false; }
      tuner_wifi::disconnect(); wasConnected = false; connect();
    }
    const bool connected = tuner_wifi::connected();
    if (connected && !wasConnected) {
      if (pendingSave && saved) {
        if (prefs.putBytes("wifi", &current, sizeof(current)) != sizeof(current))
          Serial.println(F("[wifi] connected, but saving failed"));
        pendingSave = false;
      }
      // User-requested lifecycle: no 60-second wait once the router has an IP.
      if (setup && tuner_wifi::stopSetup()) {
        setup = false; Serial.println(F("[wifi] router connected; open setup hotspot CLOSED"));
      }
      char ip[16]; tuner_wifi::stationAddress(ip);
      Serial.print(F("[web] router address: http://")); Serial.println(ip);
      mdns = mdns_init() == ESP_OK;
      if (mdns) { mdns_hostname_set("pc-tuner"); mdns_service_add(nullptr, "_http", "_tcp", 80, nullptr, 0); }
    }
    if (!connected && wasConnected) {
      attempt = millis();
      if (mdns) { mdns_free(); mdns = false; }
    }
    if (!connected && static_cast<uint32_t>(millis() - attempt) > 30000) openSetup();
    if (!connected && *current.ssid && static_cast<uint32_t>(millis() - attempt) > 45000) connect();
    WifiStatus snapshot;
    snapshot.connected = connected; snapshot.setupAp = setup; snapshot.serviceReady = serviceReady.load();
    snapshot.credentialsPending = pendingSave; snapshot.rssi = tuner_wifi::signalStrength();
    strlcpy(snapshot.ssid, current.ssid, sizeof(snapshot.ssid));
    if (connected) tuner_wifi::stationAddress(snapshot.ip);
    portENTER_CRITICAL(&networkMux); networkStatus = snapshot; portEXIT_CRITICAL(&networkMux);
    wasConnected = connected;
    vTaskDelay(pdMS_TO_TICKS(200));
  }
}
}

void setupWebControl() {
  wifiChanges = xQueueCreate(1, sizeof(WifiChange));
  if (!wifiChanges || xTaskCreatePinnedToCore(networkTask, "pc-network",
      runtime_resources::NETWORK_STACK_BYTES, nullptr, 1, nullptr, 0) != pdPASS)
    Serial.println(F("[wifi] network task could not start"));
}
bool requestWifiReconnect() {
  if (!serviceReady.load()) return false;
  reconnectRequested = true; return true;
}
WifiStatus readWifiStatus() {
  portENTER_CRITICAL(&networkMux); WifiStatus copy = networkStatus; portEXIT_CRITICAL(&networkMux);
  return copy;
}

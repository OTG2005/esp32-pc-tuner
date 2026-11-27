#include "LedWebSetup.h"
#include "NetworkValidation.h"
#include "RuntimeResources.h"
#include "TunerWiFi.h"
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
#include <string.h>

namespace {

constexpr char LED_SETUP_SSID_PREFIX[] = "PC-Tuner-LED";
constexpr char LED_HOSTNAME[] = "pc-tuner-led";

const char LED_SETUP_HTML[] PROGMEM =
    "<!doctype html><html lang='en'><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>PC Tuner LED Node</title>"
    "<style>body{font:16px sans-serif;max-width:520px;margin:16px}"
    "input,button{font:inherit;margin:4px 0;padding:6px;width:100%;box-sizing:border-box}</style>"
    "<h1>PC Tuner LED Node</h1>"
    "<p>Connect this board to the same 2.4 GHz router as the PC Tuner Controller. "
    "The open setup hotspot closes automatically once connected.</p>"
    "<form action='/api/wifi' method='post'>"
    "<input name='ssid' placeholder='2.4 GHz router SSID' maxlength='32' required>"
    "<input name='password' placeholder='Router password (blank if open)' type='password' maxlength='64'>"
    "<button>Connect router</button></form>"
    "<p><a href='/api/status'>View LED Node status</a></p></html>";

httpd_handle_t server = nullptr;
std::atomic<bool> wifiUpdatePending{false};
QueueHandle_t wifiChanges = nullptr;
struct WifiChange { uint8_t version = 1; char ssid[33]; char password[65]; bool open; };
portMUX_TYPE networkMux = portMUX_INITIALIZER_UNLOCKED;
LedWifiStatus networkStatus;

void headers(httpd_req_t *req) {
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_set_hdr(req, "X-PC-Tuner-Device", "esp32-pc-tuner-led-node");
}

esp_err_t text(httpd_req_t *req, const char *body, const char *status = "200 OK") {
  headers(req);
  httpd_resp_set_status(req, status);
  httpd_resp_set_type(req, "text/plain; charset=utf-8");
  return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

bool readBody(httpd_req_t *req, char *output, size_t capacity) {
  if (!req->content_len || req->content_len >= capacity) {
    text(req, "Input too long or empty.", "413 Payload Too Large");
    return false;
  }
  size_t received = 0;
  while (received < req->content_len) {
    const int count = httpd_req_recv(req, output + received, req->content_len - received);
    if (count <= 0) { text(req, "Input timed out.", "408 Request Timeout"); return false; }
    received += count;
  }
  if (memchr(output, 0, received)) {
    text(req, "Invalid input.", "400 Bad Request");
    return false;
  }
  output[received] = 0;
  return true;
}

int hexVal(char ch) {
  if (ch >= '0' && ch <= '9') return ch - '0';
  if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
  if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
  return -1;
}

bool readField(const char *input, const char *name, char *output, size_t capacity) {
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
          if (value + 2 >= end || hexVal(value[1]) < 0 || hexVal(value[2]) < 0) return false;
          ch = static_cast<char>((hexVal(value[1]) << 4) | hexVal(value[2]));
          value += 2;
        }
        if (!ch || used + 1 >= capacity) return false;
        output[used++] = ch;
      }
      output[used] = 0;
      return true;
    }
    part = *end ? end + 1 : nullptr;
  }
  return false;
}

esp_err_t handleRoot(httpd_req_t *req) {
  headers(req);
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  return httpd_resp_send(req, LED_SETUP_HTML, HTTPD_RESP_USE_STRLEN);
}

esp_err_t handleStatus(httpd_req_t *req) {
  const LedWifiStatus wifi = readLedWifiStatus();
  char output[256];
  snprintf(output, sizeof(output),
      "ESP32 PC Tuner LED Node\nWi-Fi=%s SSID=%s IP=%s RSSI=%d setup=%s\n"
      "hostname=%s.local heap=%lu uptime=%lus\n",
      wifi.connected ? "connected" : "off",
      *wifi.ssid ? wifi.ssid : "not-configured",
      *wifi.ip ? wifi.ip : "none",
      wifi.rssi,
      wifi.setupAp ? "open" : "off",
      LED_HOSTNAME,
      static_cast<unsigned long>(ESP.getFreeHeap()),
      static_cast<unsigned long>(millis() / 1000));
  return text(req, output);
}

esp_err_t handleWifi(httpd_req_t *req) {
  char input[384];
  WifiChange change = {};
  if (!readBody(req, input, sizeof(input))) return ESP_FAIL;
  if (!readField(input, "ssid", change.ssid, sizeof(change.ssid)) ||
      !readField(input, "password", change.password, sizeof(change.password))) {
    return text(req, "SSID/password too long or invalid.", "400 Bad Request");
  }
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
  memset(input, 0, sizeof(input));
  memset(&change, 0, sizeof(change));
  return text(req, "Connecting LED Node to router. Setup hotspot closes once an IP is obtained.",
              "202 Accepted");
}

bool startServer() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.stack_size = runtime_resources::HTTP_STACK_BYTES;
  config.max_open_sockets = runtime_resources::HTTP_CLIENT_SOCKETS;
  config.max_uri_handlers = 4;
  config.lru_purge_enable = true;
  config.recv_wait_timeout = config.send_wait_timeout = 3;
  if (httpd_start(&server, &config) != ESP_OK) return false;
  const struct { const char *uri; httpd_method_t method; esp_err_t (*handler)(httpd_req_t *); } routes[] = {
    {"/", HTTP_GET, handleRoot},
    {"/api/status", HTTP_GET, handleStatus},
    {"/api/wifi", HTTP_POST, handleWifi}
  };
  for (const auto &route : routes) {
    httpd_uri_t uri = {};
    uri.uri = route.uri;
    uri.method = route.method;
    uri.handler = route.handler;
    if (httpd_register_uri_handler(server, &uri) != ESP_OK) {
      httpd_stop(server);
      server = nullptr;
      return false;
    }
  }
  return true;
}

void ledNetworkTask(void *) {
  Preferences prefs;
  const bool saved = prefs.begin("pc-led-net", false);
  WifiChange current = {};
  bool loaded = saved && prefs.getBytesLength("wifi") == sizeof(current) &&
      prefs.getBytes("wifi", &current, sizeof(current)) == sizeof(current) &&
      current.version == 1 && current.ssid[32] == 0 && current.password[64] == 0;
  if (!loaded) {
    current = WifiChange();
    strlcpy(current.ssid, local_config::WIFI_SSID, sizeof(current.ssid));
    strlcpy(current.password, local_config::WIFI_PASSWORD, sizeof(current.password));
    current.open = current.password[0] == 0;
  }
  const char *error = nullptr;
  if (*current.ssid && !validWifiCredentials(current.ssid, current.password, current.open, error))
    current = WifiChange();

  bool setup = false, wasConnected = false, pendingSave = false, mdns = false;
  uint32_t attempt = millis();
  // No Bluetooth on LED Node: disable modem sleep (bluetoothCoex=false) for low-latency UDP frames.
  if (!tuner_wifi::initialize(LED_HOSTNAME, false)) {
    Serial.println(F("[led-wifi] driver failed"));
    vTaskDelete(nullptr);
    return;
  }

  auto openSetup = [&]() {
    if (!setup) {
      setup = tuner_wifi::startSetup(LED_SETUP_SSID_PREFIX, *current.ssid != 0);
      if (setup) {
        Serial.print(F("[led-wifi] OPEN setup hotspot: "));
        Serial.println(tuner_wifi::setupName());
        Serial.println(F("[led-wifi] open http://192.168.4.1 to enter router Wi-Fi"));
      }
    }
  };
  auto connect = [&]() {
    attempt = millis();
    if (*current.ssid) tuner_wifi::connect(current.ssid, current.password, current.open, setup);
    else openSetup();
  };

  connect();
  startServer();

  for (;;) {
    WifiChange change;
    if (xQueueReceive(wifiChanges, &change, 0) == pdTRUE) {
      vTaskDelay(pdMS_TO_TICKS(1000));
      if (mdns) { mdns_free(); mdns = false; }
      tuner_wifi::disconnect();
      current = change;
      memset(&change, 0, sizeof(change));
      pendingSave = true;
      wasConnected = false;
      wifiUpdatePending = false;
      connect();
    }

    const bool connected = tuner_wifi::connected();
    if (connected && !wasConnected) {
      if (pendingSave && saved) {
        prefs.putBytes("wifi", &current, sizeof(current));
        pendingSave = false;
      }
      if (setup && tuner_wifi::stopSetup()) {
        setup = false;
        Serial.println(F("[led-wifi] router connected; open setup hotspot CLOSED"));
      }
      char ip[16];
      tuner_wifi::stationAddress(ip);
      Serial.print(F("[led-wifi] router address: http://"));
      Serial.println(ip);
      mdns = mdns_init() == ESP_OK;
      if (mdns) {
        mdns_hostname_set(LED_HOSTNAME);
        mdns_service_add(nullptr, "_http", "_tcp", 80, nullptr, 0);
      }
    }
    if (!connected && wasConnected) {
      attempt = millis();
      if (mdns) { mdns_free(); mdns = false; }
    }
    if (!connected && static_cast<uint32_t>(millis() - attempt) > 30000) openSetup();
    if (!connected && *current.ssid && static_cast<uint32_t>(millis() - attempt) > 45000) connect();

    LedWifiStatus snapshot;
    snapshot.connected = connected;
    snapshot.setupAp = setup;
    snapshot.rssi = tuner_wifi::signalStrength();
    strlcpy(snapshot.ssid, current.ssid, sizeof(snapshot.ssid));
    if (connected) tuner_wifi::stationAddress(snapshot.ip);
    portENTER_CRITICAL(&networkMux);
    networkStatus = snapshot;
    portEXIT_CRITICAL(&networkMux);

    wasConnected = connected;
    vTaskDelay(pdMS_TO_TICKS(200));
  }
}

} // namespace

void setupLedWebSetup() {
  wifiChanges = xQueueCreate(1, sizeof(WifiChange));
  if (!wifiChanges || xTaskCreatePinnedToCore(ledNetworkTask, "led-network",
      runtime_resources::NETWORK_STACK_BYTES, nullptr, 1, nullptr, 0) != pdPASS) {
    Serial.println(F("[led-wifi] network task could not start"));
  }
}

LedWifiStatus readLedWifiStatus() {
  portENTER_CRITICAL(&networkMux);
  LedWifiStatus copy = networkStatus;
  portEXIT_CRITICAL(&networkMux);
  return copy;
}

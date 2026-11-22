#include "TunerWiFi.h"
#include "RuntimeResources.h"
#include <Arduino.h>
#include <atomic>
#include <esp_event.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <esp_wifi_default.h>
#include <esp_log.h>
#include <esp32/rom/ets_sys.h>
#include <string.h>

namespace tuner_wifi {
namespace {
esp_netif_t *station = nullptr;
esp_netif_t *accessPoint = nullptr;
bool initialized = false;
bool started = false;
char accessPointName[33] = {};
std::atomic<bool> stationConnected{false};
std::atomic<uint8_t> apClients{0};
std::atomic<int> disconnectReason{0};

bool check(const char *operation, esp_err_t result) {
  if (result == ESP_OK) return true;
  ets_printf("[wifi] %s failed: %s (0x%x)\n", operation,
      esp_err_to_name(result), static_cast<unsigned>(result));
  return false;
}

void events(void *, esp_event_base_t base, int32_t event, void *data) {
  if (base == IP_EVENT && event == IP_EVENT_STA_GOT_IP) {
    stationConnected = true;
    disconnectReason = 0;
    ets_printf("[wifi] station obtained an IP address\n");
  } else if (base == WIFI_EVENT) {
    if (event == WIFI_EVENT_STA_DISCONNECTED) {
      stationConnected = false;
      const auto *details = static_cast<wifi_event_sta_disconnected_t *>(data);
      disconnectReason = details->reason;
      ets_printf("[wifi] station disconnected: reason=%u\n",
          static_cast<unsigned>(details->reason));
    } else if (event == WIFI_EVENT_STA_STOP) {
      stationConnected = false;
    } else if (event == WIFI_EVENT_AP_STACONNECTED) {
      const uint8_t count = apClients.load();
      apClients = count + 1;
      ets_printf("[wifi] setup client connected; clients=%u\n",
          static_cast<unsigned>(apClients.load()));
      logRuntimeMemory("setup client connected");
    } else if (event == WIFI_EVENT_AP_STADISCONNECTED) {
      const uint8_t count = apClients.load();
      if (count) apClients = count - 1;
      ets_printf("[wifi] setup client disconnected; clients=%u\n",
          static_cast<unsigned>(apClients.load()));
    } else if (event == WIFI_EVENT_AP_STOP) {
      apClients = 0;
    }
  }
}

bool ensureStation() {
  if (!station) {
    station = esp_netif_create_default_wifi_sta();
    if (!station) {
      ets_printf("[wifi] station interface allocation failed\n");
      return false;
    }
    esp_netif_set_hostname(station, "pc-tuner");
  }
  return true;
}

bool ensureAccessPoint() {
  if (!accessPoint) {
    accessPoint = esp_netif_create_default_wifi_ap();
    if (!accessPoint) {
      ets_printf("[wifi] setup interface allocation failed\n");
      return false;
    }
    esp_netif_set_hostname(accessPoint, "pc-tuner");
  }
  return true;
}

bool startDriver() {
  if (started) return true;
  // ESP32's Wi-Fi binary aborts on WIFI_PS_NONE when Bluetooth is enabled.
  // Modem sleep is also how the coexistence scheduler gives BT radio time.
  if (!check("coexistence power-save setting", esp_wifi_set_ps(WIFI_PS_MIN_MODEM)))
    return false;
  if (!check("driver start", esp_wifi_start())) return false;
  started = true;
  return true;
}

void address(esp_netif_t *interface, char output[16]) {
  output[0] = '\0';
  if (!interface) return;
  esp_netif_ip_info_t info = {};
  if (esp_netif_get_ip_info(interface, &info) == ESP_OK && info.ip.addr)
    snprintf(output, 16, IPSTR, IP2STR(&info.ip));
}
}

bool initialize() {
  if (initialized) return true;
  if (!check("network initialization", esp_netif_init())) return false;
  const esp_err_t loop = esp_event_loop_create_default();
  if (loop != ESP_OK && loop != ESP_ERR_INVALID_STATE)
    return check("event loop", loop);

  wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
  config.static_rx_buf_num = runtime_resources::WIFI_STATIC_RX_BUFFERS;
  config.dynamic_rx_buf_num = runtime_resources::WIFI_DYNAMIC_RX_BUFFERS;
  config.tx_buf_type = 1; // Dynamic TX; do not reserve a large static TX pool.
  config.static_tx_buf_num = 0;
  config.dynamic_tx_buf_num = runtime_resources::WIFI_DYNAMIC_TX_BUFFERS;
  config.cache_tx_buf_num = 4;
  config.mgmt_sbuf_num = runtime_resources::WIFI_MANAGEMENT_BUFFERS;
  config.ampdu_rx_enable = 0;
  config.ampdu_tx_enable = 0;
  config.amsdu_tx_enable = 0;
  config.rx_ba_win = 4;
  config.nvs_enable = 0; // The application stores one verified credentials blob.
  if (!check("driver initialization", esp_wifi_init(&config))) return false;
  // Authentication/association reasons, not passwords or payload dumps.
  esp_log_level_set("wifi", ESP_LOG_INFO);
  if (!check("RAM-only driver settings", esp_wifi_set_storage(WIFI_STORAGE_RAM))) return false;
  if (!check("coexistence initialization", esp_wifi_set_ps(WIFI_PS_MIN_MODEM))) return false;
  if (!check("Wi-Fi event handler",
      esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, events, nullptr))) return false;
  if (!check("IP event handler",
      esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, events, nullptr))) return false;
  initialized = true;
  logRuntimeMemory("after bounded Wi-Fi driver");
  return true;
}

bool connect(const char *ssid, const char *password, bool open, bool keepSetupAp) {
  if (!initialized || !ssid || !*ssid || !ensureStation()) return false;
  stationConnected = false;
  if (!check("station mode", esp_wifi_set_mode(keepSetupAp ? WIFI_MODE_APSTA : WIFI_MODE_STA)))
    return false;
  wifi_config_t config = {};
  memcpy(config.sta.ssid, ssid, strnlen(ssid, sizeof(config.sta.ssid)));
  if (!open && password)
    memcpy(config.sta.password, password, strnlen(password, sizeof(config.sta.password)));
  config.sta.threshold.authmode = open ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
  config.sta.threshold.rssi = -127;
  config.sta.pmf_cfg.capable = true;
  config.sta.pmf_cfg.required = false;
  const esp_err_t configured = esp_wifi_set_config(WIFI_IF_STA, &config);
  memset(&config, 0, sizeof(config));
  if (!check("station configuration", configured) || !startDriver()) return false;
  return check("station connection", esp_wifi_connect());
}

void disconnect() {
  stationConnected = false;
  if (initialized && started) {
    const esp_err_t result = esp_wifi_disconnect();
    if (result != ESP_OK && result != ESP_ERR_WIFI_NOT_CONNECT)
      check("station disconnection", result);
  }
}

bool startSetup(const char *ssid, bool stationConfigured) {
  if (!initialized || !ensureAccessPoint()) return false;
  if (!check("setup mode",
      esp_wifi_set_mode(stationConfigured ? WIFI_MODE_APSTA : WIFI_MODE_AP))) return false;
  wifi_config_t config = {};
  uint8_t mac[6] = {};
  esp_wifi_get_mac(WIFI_IF_AP, mac);
  // A fresh, device-specific SSID avoids a phone reusing credentials cached
  // for a previous build or connecting to another identically named ESP32.
  snprintf(accessPointName, sizeof(accessPointName), "%.27s-%02X%02X",
      ssid, mac[4], mac[5]);
  const size_t length = strlen(accessPointName);
  memcpy(config.ap.ssid, accessPointName, length);
  config.ap.ssid_len = length;
  config.ap.authmode = WIFI_AUTH_OPEN;
  config.ap.channel = 6;
  config.ap.max_connection = 2;
  config.ap.beacon_interval = 100;
  const esp_err_t configured = esp_wifi_set_config(WIFI_IF_AP, &config);
  ets_printf("[wifi] open setup: SSID=%s (no password)\n", accessPointName);
  memset(&config, 0, sizeof(config));
  return check("open setup configuration", configured) && startDriver();
}

bool stopSetup() {
  if (!initialized) return false;
  return check("close setup interface", esp_wifi_set_mode(WIFI_MODE_STA));
}

bool connected() { return stationConnected.load(); }
uint8_t setupClients() { return apClients.load(); }
int lastDisconnectReason() { return disconnectReason.load(); }

int signalStrength() {
  wifi_ap_record_t info = {};
  return stationConnected.load() && esp_wifi_sta_get_ap_info(&info) == ESP_OK ? info.rssi : 0;
}
void stationAddress(char output[16]) { address(station, output); }
void setupAddress(char output[16]) { address(accessPoint, output); }
const char *setupName() { return accessPointName; }
}

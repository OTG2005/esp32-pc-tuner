#pragma once
#include <stdint.h>

// Small ESP-IDF Wi-Fi adapter. Unlike Arduino WiFi's fixed 32-buffer pool,
// this initializes the driver with a bounded control-traffic buffer profile.
namespace tuner_wifi {
bool initialize(const char *hostname = "pc-tuner", bool bluetoothCoex = true);
bool connect(const char *ssid, const char *password, bool open, bool keepSetupAp);
void disconnect();
bool startSetup(const char *ssid, bool stationConfigured);
bool stopSetup();
bool connected();
uint8_t setupClients();
int lastDisconnectReason();
int signalStrength();
void stationAddress(char output[16]);
void setupAddress(char output[16]);
void stationMac(uint8_t mac[6]);
const char *setupName();
}

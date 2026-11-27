#pragma once
#include <stdint.h>

struct LedWifiStatus {
  bool connected = false;
  bool setupAp = false;
  int rssi = 0;
  char ssid[33] = {};
  char ip[16] = {};
};

void setupLedWebSetup();
LedWifiStatus readLedWifiStatus();

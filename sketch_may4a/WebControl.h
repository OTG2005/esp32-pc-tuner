#pragma once

struct WifiStatus {
  bool connected = false;
  bool setupAp = false;
  bool serviceReady = false;
  bool credentialsPending = false;
  int rssi = 0;
  char ssid[33] = {};
  char ip[16] = {};
};

void setupWebControl();
bool requestWifiReconnect();
WifiStatus readWifiStatus();

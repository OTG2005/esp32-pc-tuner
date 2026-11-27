#pragma once
#include <stddef.h>
#include <string.h>

inline bool validWifiCredentials(const char *ssid, const char *password,
                                 bool allowOpen, const char *&error) {
  error = nullptr;
  if (!ssid || !password || strlen(ssid) < 1 || strlen(ssid) > 32) {
    error = "SSID must be 1..32 UTF-8 bytes"; return false;
  }
  for (const char *p = ssid; *p; ++p)
    if (static_cast<unsigned char>(*p) < 32 || *p == 127) {
      error = "SSID cannot contain control characters"; return false;
    }
  for (const char *p = password; *p; ++p)
    if (static_cast<unsigned char>(*p) < 32 ||
        static_cast<unsigned char>(*p) > 126) {
      error = "Wi-Fi passphrases must use printable ASCII characters"; return false;
    }
  const size_t n = strlen(password);
  if (allowOpen) {
    if (!n) return true;
    error = "an open network must have an empty password"; return false;
  }
  if (n >= 8 && n <= 63) return true;
  if (n == 64) {
    for (size_t i = 0; i < n; ++i) {
      const char c = password[i];
      if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
            (c >= 'A' && c <= 'F'))) {
        error = "64-character Wi-Fi keys must be hexadecimal"; return false;
      }
    }
    return true;
  }
  error = "Wi-Fi password must be 8..63 characters (or a 64-digit hex key)";
  return false;
}

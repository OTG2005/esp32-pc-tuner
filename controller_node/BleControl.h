#pragma once
#include <stddef.h>
#include <stdint.h>

using BleCommandReceiver = void (*)(const char *line);
bool setupBleUart(const char *name, BleCommandReceiver receiver);
void serviceBleUart();
bool bleUartConnected();
bool bleUartSubscribed();
bool sendBleUart(const uint8_t *bytes, size_t length);

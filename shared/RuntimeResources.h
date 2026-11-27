#pragma once
#include <stddef.h>
#include <stdint.h>

// Bounded resource profile shared across ESP32 PC Tuner boards.
namespace runtime_resources {
constexpr uint8_t BLE_CONNECTIONS = 1;
constexpr uint8_t CLASSIC_CONNECTIONS = 1;
constexpr size_t CONTROL_QUEUE_DEPTH = 8;
constexpr size_t WEB_REPLY_DEPTH = 1;
constexpr size_t BLE_REPLY_DEPTH = 4;
constexpr uint32_t LOOP_STACK_BYTES = 6144;
constexpr uint32_t PLAYBACK_STACK_BYTES = 3072;
constexpr uint32_t NETWORK_STACK_BYTES = 4096;
constexpr uint32_t HTTP_STACK_BYTES = 4096;
constexpr uint8_t HTTP_CLIENT_SOCKETS = 2;
constexpr uint8_t WIFI_STATIC_RX_BUFFERS = 4;
constexpr uint8_t WIFI_DYNAMIC_RX_BUFFERS = 4;
constexpr uint8_t WIFI_DYNAMIC_TX_BUFFERS = 4;
constexpr uint8_t WIFI_MANAGEMENT_BUFFERS = 6;
constexpr uint32_t MEMORY_LOG_INTERVAL_MS = 10000;
}

void installRuntimeDiagnostics();
void logRuntimeMemory(const char *stage);

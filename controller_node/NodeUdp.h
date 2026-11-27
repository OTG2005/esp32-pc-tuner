#pragma once

#include <stddef.h>
#include <stdint.h>

// Lightweight non-blocking lwIP UDP transport for inter-board Wi-Fi packets.
// Uses ESP-IDF/lwIP sockets directly so Arduino WiFi.h is never pulled in.
namespace node_udp {

bool open(uint16_t port);
void close();
bool isOpen();

bool sendUnicast(uint32_t ipv4NetOrder, uint16_t port,
                 const uint8_t *data, size_t length);
bool sendBroadcast(uint16_t port, const uint8_t *data, size_t length);

// Returns received byte count (>0) if a packet was read, or 0 if no packet.
size_t receive(uint8_t *buffer, size_t capacity,
               uint32_t &senderIpv4NetOrder, uint16_t &senderPort);

void formatIpv4(uint32_t ipv4NetOrder, char out[16]);

} // namespace node_udp

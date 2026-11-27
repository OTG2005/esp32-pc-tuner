#include "NodeUdp.h"
#include <errno.h>
#include <fcntl.h>
#include <lwip/inet.h>
#include <lwip/sockets.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

namespace node_udp {
namespace {
int udpSocket = -1;
}

bool open(uint16_t port) {
  if (udpSocket >= 0) return true;
  const int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
  if (fd < 0) return false;

  int enable = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));
  setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &enable, sizeof(enable));

  const int flags = fcntl(fd, F_GETFL, 0);
  if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);

  struct sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(port);
  if (bind(fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) < 0) {
    ::close(fd);
    return false;
  }
  udpSocket = fd;
  return true;
}

void close() {
  if (udpSocket >= 0) {
    ::close(udpSocket);
    udpSocket = -1;
  }
}

bool isOpen() { return udpSocket >= 0; }

bool sendUnicast(uint32_t ipv4NetOrder, uint16_t port,
                 const uint8_t *data, size_t length) {
  if (udpSocket < 0 || !data || !length || ipv4NetOrder == 0U) return false;
  struct sockaddr_in dest = {};
  dest.sin_family = AF_INET;
  dest.sin_addr.s_addr = ipv4NetOrder;
  dest.sin_port = htons(port);
  const ssize_t sent = sendto(udpSocket, data, length, 0,
                              reinterpret_cast<struct sockaddr *>(&dest),
                              sizeof(dest));
  return sent == static_cast<ssize_t>(length);
}

bool sendBroadcast(uint16_t port, const uint8_t *data, size_t length) {
  return sendUnicast(htonl(INADDR_BROADCAST), port, data, length);
}

size_t receive(uint8_t *buffer, size_t capacity,
               uint32_t &senderIpv4NetOrder, uint16_t &senderPort) {
  if (udpSocket < 0 || !buffer || !capacity) return 0;
  struct sockaddr_in src = {};
  socklen_t srcLen = sizeof(src);
  const ssize_t received = recvfrom(udpSocket, buffer, capacity, 0,
                                    reinterpret_cast<struct sockaddr *>(&src),
                                    &srcLen);
  if (received <= 0) return 0;
  senderIpv4NetOrder = src.sin_addr.s_addr;
  senderPort = ntohs(src.sin_port);
  return static_cast<size_t>(received);
}

void formatIpv4(uint32_t ipv4NetOrder, char out[16]) {
  if (!ipv4NetOrder) {
    out[0] = '\0';
    return;
  }
  struct in_addr addr = {};
  addr.s_addr = ipv4NetOrder;
  const char *text = inet_ntoa(addr);
  if (!text) {
    out[0] = '\0';
    return;
  }
  snprintf(out, 16, "%s", text);
}

} // namespace node_udp

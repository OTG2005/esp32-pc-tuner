#pragma once
#include "ControlProtocol.h"
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

enum class CommandOrigin : uint8_t { Web, Bluetooth, RemoteVolume };
struct QueuedControl {
  ControlCommand command;
  CommandOrigin origin;
  uint32_t id;
};
struct CommandReply {
  uint32_t id;
  bool ok;
  CommandKind kind;
  char message[256];
};
struct DeviceStatus {
  ControlState controls;
  bool bluetoothConnected = false;
  bool bluetoothPlaying = false;
  bool playbackAvailable = false;
  bool speakerReady = false;
  bool bluetoothPcmActive = false;
  bool ledNodeConnected = false;
  char ledNodeIp[16] = {};
  float level = 0;
  float bass = 0;
  float mid = 0;
  float treble = 0;
  uint32_t uptime = 0;
  uint32_t freeHeap = 0;
  uint32_t revision = 0;
};

extern QueueHandle_t controlQueue;
extern QueueHandle_t webReplyQueue;
extern QueueHandle_t bleReplyQueue;

bool enqueueControl(const char *text, CommandOrigin origin, uint32_t &id,
                    const char *&error);
void publishDeviceStatus(const DeviceStatus &status);
DeviceStatus readDeviceStatus();

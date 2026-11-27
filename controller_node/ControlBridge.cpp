#include "ControlBridge.h"
#include <Arduino.h>
#include <atomic>

QueueHandle_t controlQueue = nullptr;
QueueHandle_t webReplyQueue = nullptr;
QueueHandle_t bleReplyQueue = nullptr;

namespace {
std::atomic<uint32_t> nextId{1};
portMUX_TYPE statusMutex = portMUX_INITIALIZER_UNLOCKED;
DeviceStatus snapshot;
}

bool enqueueControl(const char *text, CommandOrigin origin, uint32_t &id,
                    const char *&error) {
  QueuedControl queued;
  if (!parseControlCommand(text, queued.command, error)) return false;
  queued.origin = origin;
  queued.id = nextId.fetch_add(1);
  if (!controlQueue || xQueueSend(controlQueue, &queued, 0) != pdTRUE) {
    error = "control queue is full; retry";
    return false;
  }
  id = queued.id;
  return true;
}

void publishDeviceStatus(const DeviceStatus &status) {
  portENTER_CRITICAL(&statusMutex);
  snapshot = status;
  portEXIT_CRITICAL(&statusMutex);
}
DeviceStatus readDeviceStatus() {
  portENTER_CRITICAL(&statusMutex);
  DeviceStatus copy = snapshot;
  portEXIT_CRITICAL(&statusMutex);
  return copy;
}

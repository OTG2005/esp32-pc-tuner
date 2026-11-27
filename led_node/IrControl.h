#pragma once
#include "ControlProtocol.h"

struct IrBinding {
  uint16_t code;
  const char *button;
  const char *command;
};

extern const IrBinding IR_BINDINGS[];
extern const size_t IR_BINDING_COUNT;

// All original remote functions use the same parser as web and BLE.
// Only the physical brightness keys repeat; their original step is 10.
bool parseIrControl(uint16_t code, bool repeat, ControlCommand &command);

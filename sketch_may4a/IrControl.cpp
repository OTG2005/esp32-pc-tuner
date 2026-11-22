#include "IrControl.h"

const IrBinding IR_BINDINGS[] = {
  {0x40, "ON", "lights on"}, {0x41, "OFF", "lights off"},
  {0x5C, "Brightness +", "brightness up"},
  {0x5D, "Brightness -", "brightness down"},
  {0x44, "W", "color white"}, {0x45, "B", "color azure-blue"},
  {0x59, "G", "color spring-green"}, {0x58, "R", "color rose-red"},
  {0x48, "Pink 1", "color pink"}, {0x4C, "Pink 2", "color deep-pink"},
  {0x1F, "Cyan 1", "color cyan"}, {0x1B, "Cyan 2", "color turquoise"},
  {0x49, "Blue 2", "color indigo"}, {0x4D, "Purple 1", "color purple"},
  {0x1E, "Purple 2", "color orchid"}, {0x1A, "Purple 3", "color magenta"},
  {0x54, "Orange 1", "color orange"}, {0x50, "Orange 2", "color amber"},
  {0x1C, "Orange 3", "color gold"}, {0x18, "Yellow", "color yellow"},
  {0x55, "Light green", "color lime"}, {0x51, "Light blue 1", "color sky-blue"},
  {0x1D, "Light blue 2", "color teal"}, {0x19, "Green blue", "color mint"},
  {0x04, "S1", "effect spectrum"}, {0x05, "S2", "effect sparkle"},
  {0x06, "S3", "effect pulse"}, {0x07, "S4", "effect wave"},
  {0x15, "JUMP3", "effect blink"}, {0x11, "JUMP7", "effect blend"},
  {0x0A, "FLASH", "effect fade-three"}, {0x0D, "FADE3", "effect rainbow"},
  {0x09, "FADE7", "effect fade-seven"},
  {0x17, "DIM1", "brightness low"}, {0x13, "DIM2", "brightness normal"},
  {0x0F, "DIM3", "brightness high"}, {0x0B, "DIM4", "brightness maximum"},
  {0x14, "1H", "sensitivity maximum"}, {0x10, "2H", "sensitivity high"},
  {0x0C, "3H", "sensitivity balanced"}, {0x08, "4H", "sensitivity low"}
};
const size_t IR_BINDING_COUNT = sizeof(IR_BINDINGS) / sizeof(IR_BINDINGS[0]);

bool parseIrControl(uint16_t code, bool repeat, ControlCommand &command) {
  if (repeat && code != 0x5C && code != 0x5D) return false;
  for (size_t i = 0; i < IR_BINDING_COUNT; ++i) {
    if (IR_BINDINGS[i].code != code) continue;
    const char *error = nullptr;
    if (!parseControlCommand(IR_BINDINGS[i].command, command, error)) return false;
    if (code == 0x5C || code == 0x5D) command.value = code == 0x5C ? 10 : -10;
    return true;
  }
  return false;
}

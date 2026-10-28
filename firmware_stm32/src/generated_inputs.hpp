#pragma once
#include <cstdint>
// Generated from the project's Input Map (project.jetproject). Do not edit.
// One pin per action, in package order (bit i = action i). -1 means no button.
// ESP32: GPIO number. STM32: PinName (port * 16 + pin).
// ui_left, ui_right, ui_up, ui_down, ui_accept, ui_cancel, move_up, move_down
// Buttons connect the pin to GND (internal pull-up). Bit i of input_active_high: the board's own button,
// pressed = high (it has its own pull-down).
static constexpr int input_pins[8] = {-1, -1, -1, -1, 45, -1, 95, 77};
static constexpr uint32_t input_active_high = 0x10;

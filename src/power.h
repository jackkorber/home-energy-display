#pragma once
// Board battery and power button: AXP2101 power chip (I2C 0x34) on the Waveshare 1.75C.
// Read from the "ui" task only (it shares the I2C bus with the touch panel).
#include <Arduino.h>
#include <Wire.h>

namespace power {

struct State {
  bool ok = false;           // power chip found
  bool battery = false;      // battery connected
  bool usb = false;          // powered by USB
  bool charging = false;
  bool care_paused = false;  // charging paused at the limit to protect the battery
  int8_t pct = -1;           // 0..100, -1 = unknown
  uint16_t mv = 0;           // battery voltage
};

void begin(TwoWire &wire);  // call in setup(), before the tasks start
void loop();                // call often from the ui task
State get();                // safe from any task
bool shutting_down();       // true once the PWR button asked to power off

}  // namespace power

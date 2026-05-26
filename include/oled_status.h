#pragma once

#include <Arduino.h>

/** SSD1306 (128×64 I2C). Classic ESP32: SDA 21 / SCL 22. ESP32-P4: tries GPIO8/7, 7/8, then 21/22 unless blocked by active GPIO pins. No-op if init fails. */
void oledStatusBegin(const int *blockedPins = nullptr, int blockedPinCount = 0);
/** Mark web interface ready; splash can transition to live status. */
void oledStatusSetReady(bool ready);
void oledStatusLoop();

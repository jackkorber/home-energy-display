#pragma once
#include <Arduino.h>

// USB logging. LOG_ENABLED=0 in platformio.ini strips it from the binary.
// If no computer is connected, nothing is written.
#ifndef LOG_ENABLED
#define LOG_ENABLED 1
#endif

extern volatile bool g_log_mute;  // true while a screenshot is being sent over USB

#if LOG_ENABLED
#define LOGF(...)                                        \
  do {                                                   \
    if (Serial && !g_log_mute) Serial.printf(__VA_ARGS__); \
  } while (0)
#else
#define LOGF(...) \
  do {            \
  } while (0)
#endif

#pragma once
#include <Wire.h>

// Sound via the board's ES8311 codec (I2C 0x18) + NS4150B amplifier (GPIO46)
namespace audio {
enum Sound : uint8_t { SND_SPEED_ALERT, SND_TEST, SND_DOOR, SND_POWER_ON, SND_POWER_OFF };
bool begin(TwoWire &wire);  // call in setup(), before creating the tasks (uses the I2C bus)
void play(Sound s);          // non-blocking; ignored if already playing (or if reserved for shutdown)
void reserve_for_shutdown();  // from now on only the shutdown sound plays (no new alerts)
void play_shutdown();         // the shutdown sound (requires reserve_for_shutdown() first)
bool ok();
bool busy();                 // true while playing (to wait before powering off the board)
void setMuted(bool m);          // mutes everything without changing the volume
void setVolume(uint8_t level);  // 0 = off ... VOL_LEVELS-1 = maximum
constexpr uint8_t VOL_LEVELS = 5;
}  // namespace audio

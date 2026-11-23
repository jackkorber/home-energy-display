#include "power.h"

#include <atomic>

#include "audio.h"
#include "config.h"
#include "log.h"

namespace power {
namespace {

constexpr uint8_t AXP = 0x34;
TwoWire *bus = nullptr;
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
State st;
std::atomic<bool> s_off{false};

bool rd(uint8_t reg, uint8_t *buf, uint8_t n) {
  bus->beginTransmission(AXP);
  bus->write(reg);
  if (bus->endTransmission(false) != 0) return false;
  if (bus->requestFrom(AXP, n) != n) return false;
  for (uint8_t i = 0; i < n; i++) buf[i] = bus->read();
  return true;
}
bool wr(uint8_t reg, uint8_t v) {
  bus->beginTransmission(AXP);
  bus->write(reg);
  bus->write(v);
  return bus->endTransmission() == 0;
}
bool setBit(uint8_t reg, uint8_t bit) {
  uint8_t v;
  if (!rd(reg, &v, 1)) return false;
  return (v & (1 << bit)) || wr(reg, v | (1 << bit));
}

// PWR held 2 s: shutdown sound, the screen fades out, then the AXP2101 cuts the power.
// On USB the chip keeps the board powered, so it goes to deep sleep instead (BOOT or a PWR press wakes it).
void powerOff() {
  LOGF("[pwr] powering off (PWR button)\n");
  s_off = true;  // the display task fades the screen to black
  audio::reserve_for_shutdown();
  uint32_t t0 = millis();
  while (audio::busy() && millis() - t0 < 1500) delay(20);
  audio::play_shutdown();
  t0 = millis();
  delay(50);
  while (audio::busy() && millis() - t0 < 2500) delay(20);
  delay(300);
  setBit(0x10, 0);  // AXP2101: power off
  delay(1500);
  LOGF("[pwr] power chip kept the power on (USB): deep sleep, press BOOT to wake\n");
  esp_sleep_enable_ext0_wakeup(GPIO_NUM_0, 0);
  esp_deep_sleep_start();
}

}  // namespace

void begin(TwoWire &wire) {
  bus = &wire;
  uint8_t id = 0;
  if (!rd(0x03, &id, 1)) {
    LOGF("[pwr] AXP2101 not responding\n");
    return;
  }
  setBit(0x30, 0);  // ADC: measure battery voltage
  setBit(0x18, 3);  // fuel gauge (percentage)
  uint8_t lvl;
  if (rd(0x27, &lvl, 1)) wr(0x27, (lvl & 0xC3) | (2 << 4) | (1 << 2));  // PWR: "hold" event at 2 s, hard off at 6 s
  uint8_t en;
  if (rd(0x41, &en, 1)) wr(0x41, en | (1 << 2));  // enable the PWR "hold" event
  wr(0x49, 0xFF);                                 // clear old events
  portENTER_CRITICAL(&mux);
  st.ok = true;
  portEXIT_CRITICAL(&mux);
  LOGF("[pwr] AXP2101 id 0x%02X\n", id);
}

void loop() {
  if (!bus || !st.ok || s_off) return;
  static uint32_t lastKey = 0, last = 0, lastLog = 0;
  static int8_t chargeOn = -1;  // last value written to the charger (-1 = not read yet)
  const uint32_t now = millis();
  if (now - lastKey >= 100) {  // PWR button: check the "hold" event 10 times per second
    lastKey = now;
    uint8_t irq;
    if (rd(0x49, &irq, 1) && (irq & (1 << 2))) {
      wr(0x49, 1 << 2);
      powerOff();
    }
  }
  if (now - last < 2000) return;
  last = now;
  uint8_t s[2], v[2], pct;
  if (!rd(0x00, s, 2)) return;
  State n;
  n.ok = true;
  n.battery = s[0] & 0x08;                         // REG00 bit3: battery present
  n.usb = s[0] & 0x20;                             // REG00 bit5: USB present
  n.charging = n.battery && ((s[1] >> 5) & 3) == 1;  // REG01 bits 6:5 = 01: charging
  if (rd(0x34, v, 2)) n.mv = (uint16_t)((v[0] & 0x1F) << 8 | v[1]);
  n.pct = (n.battery && rd(0xA4, &pct, 1) && pct <= 100) ? (int8_t)pct : (int8_t)-1;

  // Battery care: the board lives on USB, and sitting at 100% wears the cell. On USB, stop charging at
  // CHARGE_STOP_PCT and charge again below CHARGE_RESUME_PCT. Unknown percentage: never block charging.
  // AXP2101 REG18 bit 1 = charger enable.
  if (chargeOn < 0) {
    uint8_t r;
    if (rd(0x18, &r, 1)) chargeOn = (r & 0x02) ? 1 : 0;
  }
  if (chargeOn >= 0) {
    bool want = true;
    if (n.battery && n.usb && n.pct >= 0) {
      if (n.pct >= CHARGE_STOP_PCT) want = false;
      else if (n.pct > CHARGE_RESUME_PCT) want = chargeOn == 1;  // in between: keep what it was doing
    }
    if ((int8_t)want != chargeOn) {
      uint8_t r;
      if (rd(0x18, &r, 1) && wr(0x18, want ? (r | 0x02) : (r & ~0x02))) {
        chargeOn = want;
        LOGF("[pwr] charging %s (%d%%, usb=%d)\n", want ? "on" : "paused", (int)n.pct, n.usb);
      }
    }
    n.care_paused = chargeOn == 0 && n.usb && n.battery;
  }
  portENTER_CRITICAL(&mux);
  st = n;
  portEXIT_CRITICAL(&mux);
  if (now - lastLog > 60000) {
    lastLog = now;
    LOGF("[pwr] battery=%d %d%% %umV charging=%d usb=%d\n", n.battery, (int)n.pct, (unsigned)n.mv, n.charging, n.usb);
  }
}

State get() {
  portENTER_CRITICAL(&mux);
  const State s = st;
  portEXIT_CRITICAL(&mux);
  return s;
}

bool shutting_down() { return s_off; }

}  // namespace power

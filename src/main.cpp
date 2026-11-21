// Home energy and weather display — Waveshare ESP32-S3-Touch-AMOLED-1.75C
// Reads the Powerwall Gateway over the local network (works during an outage without internet, if the router
// is on backup) plus NWS alerts / NHC storms / Open-Meteo forecast.
//
// Swipe left/right or BOOT button -> change screen. Turns off at night; touching it turns it on.
// Over USB (115200), one line: "o" simulates an outage, "a" an alert, "n" forces night; "m ..." adjusts the month.
#include <Arduino.h>
#include <Wire.h>
#include <lvgl.h>
#include <math.h>
#include <time.h>

#include <atomic>

#include "TouchDrvCSTXXX.hpp"
#include "audio.h"
#include "config.h"
#include "display_drv.h"
#include "log.h"
#include "model.h"
#include "net.h"
#include "ui.h"

#define FW_VERSION "0.1.0"

#define PIN_BOOT 0
#define IIC_SDA 15
#define IIC_SCL 14
#define TP_INT 11
#define TP_RST 2
#define SPI_HZ 80000000

volatile bool g_log_mute = false;
static TouchDrvCST92xx touch;
static bool touchOk = false;
static volatile bool touchIrq = false;
static std::atomic<uint32_t> g_lastTouchMs{0};
static std::atomic<bool> g_screenOff{false};
static std::atomic<bool> g_nextPage{false};
static bool g_simOutage = false, g_simAlert = false, g_simNight = false;

// ---------- Touch (ui task): one full sample in an atomic word (x, y, pressed, swallow) ----------
static std::atomic<uint32_t> g_touch{0};
static uint32_t packTouch(int x, int y, bool pressed, bool swallow) {
  return (uint32_t)(x & 0xFFF) | (uint32_t)(y & 0xFFF) << 12 | (uint32_t)pressed << 24 | (uint32_t)swallow << 25;
}

static void pollTouch() {
  static uint32_t last = 0;
  const uint32_t cur = g_touch.load();
  const bool pressed = cur >> 24 & 1;
  if (!touchOk || (!touchIrq && !pressed)) return;
  const uint32_t now = millis();
  if (now - last < 10) return;
  last = now;
  touchIrq = false;
  int16_t x[5], y[5];
  if (touch.getPoint(x, y, 1)) {
    x[0] = (int16_t)constrain((int)x[0], 0, display::W - 1);
    y[0] = (int16_t)constrain((int)y[0], 0, display::H - 1);
    // the touch that wakes the display does nothing else
    const bool swallow = pressed ? (bool)(cur >> 25 & 1) : g_screenOff.load();
    g_touch = packTouch(x[0], y[0], true, swallow);
    g_lastTouchMs = now;
  } else {
    g_touch = packTouch(cur & 0xFFF, cur >> 12 & 0xFFF, false, false);
  }
}

static void touchRead(lv_indev_t *, lv_indev_data_t *d) {
  const uint32_t s = g_touch.load();
  d->point.x = s & 0xFFF;
  d->point.y = s >> 12 & 0xFFF;
  d->state = (s >> 24 & 1) && !(s >> 25 & 1) ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static void handleBootButton() {
  static bool wasDown = false;
  static uint32_t downAt = 0;
  const bool down = digitalRead(PIN_BOOT) == LOW;
  if (down && !wasDown) downAt = millis();
  if (!down && wasDown && millis() - downAt > 40) {
    if (!g_screenOff) g_nextPage = true;
    g_lastTouchMs = millis();  // also wakes the display
  }
  wasDown = down;
}

static void uiTask(void *) {
  for (;;) {
    handleBootButton();
    pollTouch();
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

// ---------- USB commands (one line each) ----------
// "o" simulated outage, "a" simulated alert, "n" forced night (each is cleared by repeating it)
// "m <solar> <home> <bought> <sold>": month-to-date kWh (from Netzero) so the month starts complete
static void handleSerial() {
  static char buf[96];
  static uint8_t n = 0;
  while (Serial.available()) {
    const char c = Serial.read();
    if (c != '\n' && c != '\r') {
      if (n < sizeof(buf) - 1) buf[n++] = c;
      continue;
    }
    buf[n] = 0;
    n = 0;
    if (!buf[0]) continue;
    if (buf[0] == 'm') {
      float v[4];
      if (sscanf(buf + 1, "%f %f %f %f", &v[0], &v[1], &v[2], &v[3]) == 4) {
        net::seed_month(v[0], v[1], v[2], v[3]);
        LOGF("[month] received: solar %.1f home %.1f import %.1f export %.1f\n", v[0], v[1], v[2], v[3]);
      }
      continue;
    }
    if (buf[0] == 'h') {  // "h YYYYMM solar home import export rate fixed": a past month for the history
      MonthRec r{};
      unsigned long key;
      if (sscanf(buf + 1, "%lu %f %f %f %f %f %f", &key, &r.solar, &r.home, &r.imp, &r.exp, &r.rate, &r.fixed) == 7) {
        r.key = key;
        r.from = 1;
        r.sell = r.rate;  // net metering: sold at the same price
        net::save_history(r);
      }
      continue;
    }
    if (buf[0] == 'v') {  // "v YYYYMM events kWh [YYYY-MM-DD last_event_kWh]": one month of VPP (from Netzero)
      VppRec r{};
      unsigned long key;
      unsigned evs;
      int y, mo, d;
      float lk;
      const int n = sscanf(buf + 1, "%lu %u %f %d-%d-%d %f", &key, &evs, &r.kwh, &y, &mo, &d, &lk);
      if (n >= 3) {
        r.key = key;
        r.events = evs;
        if (n == 7) {  // the last event: at 7 pm that day (Puerto Rico time), approximate
          struct tm tm = {};
          tm.tm_year = y - 1900;
          tm.tm_mon = mo - 1;
          tm.tm_mday = d;
          tm.tm_hour = 19;
          r.last_at = mktime(&tm);
          r.last_kwh = lk;
        }
        net::save_vpp(r);
      }
      continue;
    }
    if (buf[0] == 'o') g_simOutage = !g_simOutage;
    else if (buf[0] == 'a') g_simAlert = !g_simAlert;
    else if (buf[0] == 'n') g_simNight = !g_simNight;
    else continue;
    LOGF("[sim] outage=%d alert=%d night=%d\n", g_simOutage, g_simAlert, g_simNight);
  }
}

static void applySim(Energy &e, Weather &w) {
  if (g_simOutage) {
    e.status = GwStatus::OK;
    e.at = millis();
    e.grid = 0;
    if (!isfinite(e.pct)) e.pct = 86;
    if (!isfinite(e.usable_wh)) e.usable_wh = BATTERY_KWH * 1000 * e.pct / 100;
    if (!isfinite(e.net_avg_w)) e.net_avg_w = 1400;
    e.grid_w = 0;
  }
  if (g_simAlert && w.n_alerts < MAX_ALERTS) {
    memmove(&w.alerts[1], &w.alerts[0], sizeof(Alert) * (MAX_ALERTS - 1));
    Alert &a = w.alerts[0];
    strlcpy(a.event, "Hurricane Warning", sizeof(a.event));
    strlcpy(a.severity, "Extreme", sizeof(a.severity));
    time(&a.ends);
    a.ends += 6 * 3600;
    a.hash = 0x51A51A51;
    w.n_alerts++;
    if (!w.alerts_at) w.alerts_at = millis();
  }
}

static bool isNight() {
  if (g_simNight) return true;
  time_t t;
  time(&t);
  if (t < 1700000000) return false;  // no time yet: treat as daytime
  struct tm tm;
  localtime_r(&t, &tm);
  return NIGHT_START_H > NIGHT_END_H ? (tm.tm_hour >= NIGHT_START_H || tm.tm_hour < NIGHT_END_H)
                                     : (tm.tm_hour >= NIGHT_START_H && tm.tm_hour < NIGHT_END_H);
}

// Sounds: power goes out / comes back / new NWS alert. At night only the important ones.
static void sounds(const Energy &e, const Weather &w, bool night) {
  static int8_t lastGrid = -1;
  static uint32_t seen[16] = {};  // last alerts seen (ring)
  static uint8_t seenN = 0, seenPos = 0;
  static bool alertsPrimed = false;
  const bool fresh = e.at && millis() - e.at < 30000;
  if (fresh && e.grid >= 0) {
    if (lastGrid == 1 && e.grid == 0) {
      LOGF("[event] power went out\n");
      audio::play(audio::SND_SPEED_ALERT);  // at night too: it matters most
    } else if (lastGrid == 0 && e.grid == 1) {
      LOGF("[event] power is back\n");
      if (!night) audio::play(audio::SND_POWER_ON);
    }
    lastGrid = e.grid;
  }
  if (!w.alerts_at && !g_simAlert) return;
  for (int i = 0; i < w.n_alerts; i++) {
    const Alert &a = w.alerts[i];
    bool known = false;
    for (int k = 0; k < seenN; k++) known |= seen[k] == a.hash;
    if (known) continue;
    seen[seenPos] = a.hash;
    seenPos = (seenPos + 1) % 16;
    if (seenN < 16) seenN++;
    LOGF("[event] new alert: %s (%s)\n", a.event, a.severity);
    // Alerts already present when the display started do not sound
    if (alertsPrimed && strstr(a.event, "Warning") && (!night || ui::important_alert(a))) audio::play(audio::SND_DOOR);
  }
  alertsPrimed = true;
}

// ---------- Display task (core 0): the whole LVGL interface lives here ----------
static void renderTask(void *) {
  lv_init();
  lv_tick_set_cb([]() -> uint32_t { return millis(); });
  if (!display::begin(SPI_HZ)) {
    LOGF("[display] out of memory for the buffers\n");
    vTaskDelete(nullptr);
  }
  lv_indev_t *indev = lv_indev_create();
  lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(indev, touchRead);
  lv_timer_set_period(lv_indev_get_read_timer(indev), 10);
  ui::build();

  Energy e;
  Weather w;
  uint32_t last = millis(), lastUpdate = 0, lastShift = millis();
  float bright = 0;
  int shiftIdx = 0;
  // Shift everything 1-2 px every 2 min: the display is on all day and AMOLEDs burn in whatever doesn't move
  static const int8_t SHIFT[9][2] = {{0, 0}, {2, 0}, {2, 2}, {0, 2}, {-2, 2}, {-2, 0}, {-2, -2}, {0, -2}, {2, -2}};
  bool important = false;

  for (;;) {
    const uint32_t now = millis();
    const float dt = constrain((now - last) / 1000.0f, 0.001f, 0.1f);
    last = now;
    handleSerial();

    if (now - lastUpdate >= 250) {
      lastUpdate = now;
      model::get(e, w);
      applySim(e, w);
      important = ui::update(e, w, now);
      sounds(e, w, isNight());
    }
    if (g_nextPage.exchange(false)) ui::next_page();
    if (now - lastShift > 120000) {
      lastShift = now;
      shiftIdx = (shiftIdx + 1) % 9;
      ui::shift(SHIFT[shiftIdx][0], SHIFT[shiftIdx][1]);
    }

    // Brightness: fixed by day; off at night unless outage/severe alert (dim) or touched recently
    const bool night = isNight();
    const bool touched = now - g_lastTouchMs.load() < WAKE_MS;
    const float target = touched ? BRIGHT_DAY : !night ? BRIGHT_DAY : important ? BRIGHT_NIGHT : 0;
    g_screenOff = target == 0;
    if (fabsf(bright - target) > 0.5f) {
      bright += (target - bright) * fminf(1.0f, dt * 8.0f);
      if (fabsf(bright - target) < 2) bright = target;
      display::set_brightness((uint8_t)bright);
    }
    {
      static bool off = false;  // panel truly asleep (not just brightness 0)
      const bool want = target == 0 && bright < 1;
      if (want != off) {
        off = want;
        display::set_sleep(off);
        LOGF("[display] %s\n", off ? "off" : "on");
      }
    }
    if (g_screenOff && bright < 1) {
      lv_timer_handler();  // keep reading touch to wake up
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }
    const uint32_t wait = lv_timer_handler();
    vTaskDelay(pdMS_TO_TICKS(constrain(wait, 2u, 20u)));
  }
}

void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);  // with no computer connected, don't block on writes
  delay(200);
  pinMode(PIN_BOOT, INPUT_PULLUP);
  LOGF("[boot] home-energy v%s PSRAM %u\n", FW_VERSION, (unsigned)ESP.getPsramSize());

  Wire.begin(IIC_SDA, IIC_SCL);
  Wire.setClock(400000);
  touch.setPins(TP_RST, TP_INT);
  touchOk = touch.begin(Wire, 0x5A, -1, -1);
  if (touchOk) {
    touch.reset();
    touch.setMaxCoordinates(display::W, display::H);
    touch.setMirrorXY(true, true);
    attachInterrupt(TP_INT, []() { touchIrq = true; }, FALLING);
  }
  audio::begin(Wire);
  audio::setVolume(2);  // medium
  g_lastTouchMs = millis();
  LOGF("[boot] touch %d, audio %d\n", touchOk, audio::ok());

  model::begin();
  xTaskCreatePinnedToCore(renderTask, "render", 16384, nullptr, 2, nullptr, 0);
  xTaskCreatePinnedToCore(uiTask, "ui", 4096, nullptr, 2, nullptr, 1);
  net::begin();
}

void loop() { vTaskDelay(pdMS_TO_TICKS(1000)); }

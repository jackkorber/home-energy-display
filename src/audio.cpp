#include "audio.h"

#include <Arduino.h>
#include <math.h>

#include <atomic>

#include "driver/i2s_std.h"
#include "log.h"

// Pins from the ESP32-S3-Touch-AMOLED-1.75C schematic
namespace {
constexpr int PIN_MCLK = 16, PIN_BCLK = 9, PIN_WS = 45, PIN_DOUT = 8, PIN_PA = 46;  // 1.75C: MCLK on GPIO16 (schematic), not 42
constexpr uint8_t ES8311 = 0x18;
constexpr uint32_t RATE = 16000;  // MCLK = 256 x 16 kHz = 4.096 MHz

TwoWire *s_wire = nullptr;
i2s_chan_handle_t s_tx = nullptr;
QueueHandle_t s_q = nullptr;
bool s_ok = false;
std::atomic<bool> s_busy{false};  // claimed by play() with compare-exchange, released by the task
std::atomic<bool> s_muted{false};
std::atomic<bool> s_reserved{false};  // shutting down: normal alerts can no longer play
std::atomic<float> s_gain{0.55f};
// Precomputed sounds (PSRAM): rebuilt only if the volume changed (previously sinf/expf on every alert, ~50 ms)
constexpr int N_SOUNDS = 5;
int16_t *s_pre[N_SOUNDS] = {};
size_t s_preFrames[N_SOUNDS] = {};
float s_preGain[N_SOUNDS] = {-1, -1, -1, -1, -1};
constexpr size_t MAXF = RATE;  // up to 1 s of sound  // amplitude (1.0 = full scale)
const float GAINS[audio::VOL_LEVELS] = {0.0f, 0.12f, 0.3f, 0.55f, 0.8f};  // Off, Low, Medium, High, Max

bool wr(uint8_t reg, uint8_t v) {
  s_wire->beginTransmission(ES8311);
  s_wire->write(reg);
  s_wire->write(v);
  return s_wire->endTransmission() == 0;
}
int rd(uint8_t addr, uint8_t reg) {
  s_wire->beginTransmission(addr);
  s_wire->write(reg);
  if (s_wire->endTransmission(false) != 0) return -1;
  if (s_wire->requestFrom(addr, (uint8_t)1) != 1) return -1;
  return s_wire->read();
}

// ES8311 in slave mode, MCLK on its pin, 16-bit I2S, output only (DAC)
bool codecInit() {
  static const uint8_t SEQ[][2] = {
      {0x00, 0x1F}, {0x00, 0x00},              // reset
      {0x01, 0x3F},                            // clocks on, MCLK from pin
      {0x02, 0x00},                            // pre_div 1, x1
      {0x03, 0x10}, {0x04, 0x10},              // ADC and DAC OSR
      {0x05, 0x00},                            // ADC/DAC dividers = 1
      {0x06, 0x03}, {0x07, 0x00}, {0x08, 0xFF},  // BCLK/LRCK (only used as master)
      {0x09, 0x0C}, {0x0A, 0x0C},              // serial in/out: I2S 16-bit
      {0x0B, 0x00}, {0x0C, 0x00},
      {0x10, 0x1F}, {0x11, 0x7F},              // internal power
      {0x00, 0x80},                            // power on, slave
      {0x0D, 0x01}, {0x0E, 0x02},              // analog on
      {0x12, 0x00},                            // DAC on
      {0x13, 0x10},                            // output to the power stage
      {0x14, 0x1A}, {0x15, 0x40}, {0x16, 0x24}, {0x17, 0xBF},
      {0x1B, 0x0A}, {0x1C, 0x6A},
      {0x37, 0x08},                            // no DAC equalizer
      {0x31, 0x00},                            // unmuted
      {0x32, 0xD0},                            // DAC volume (0xBF = 0 dB; +8 dB)
  };
  for (auto &r : SEQ) {
    if (!wr(r[0], r[1])) return false;
    if (r[0] == 0x00 && r[1] == 0x1F) delay(20);
  }
  return true;
}

// Two-note chime, with a soft harmonic and exponential decay
size_t renderChime(int16_t *buf, size_t maxFrames, const float *notes, int n, float noteSec, float amp) {
  const size_t per = (size_t)(noteSec * RATE);
  size_t f = 0;
  for (int k = 0; k < n && f + per <= maxFrames; k++) {
    const float w = 2 * PI * notes[k] / RATE;
    for (size_t i = 0; i < per; i++, f++) {
      const float t = (float)i / RATE;
      const float env = fminf(1.0f, t / 0.004f) * expf(-t * 7.0f);
      const float v = amp * env * (sinf(w * i) + 0.25f * sinf(2 * w * i));
      buf[2 * f] = buf[2 * f + 1] = (int16_t)(v * 32767);
    }
  }
  return f;
}

void prerender(uint8_t s, float gain) {
  if (s >= N_SOUNDS || s_preGain[s] == gain) return;
  int16_t *buf = s_pre[s];
  if (s == audio::SND_SPEED_ALERT) {
    static const float N[] = {1046.5f, 1318.5f, 1046.5f, 1318.5f};  // C-E, twice
    s_preFrames[s] = renderChime(buf, MAXF, N, 4, 0.22f, gain);
  } else if (s == audio::SND_DOOR) {
    static const float N[] = {587.3f, 587.3f, 587.3f};  // three low beeps (D)
    s_preFrames[s] = renderChime(buf, MAXF, N, 3, 0.2f, gain);
  } else if (s == audio::SND_POWER_ON) {
    static const float N[] = {523.3f, 659.3f, 784.0f, 1046.5f};  // C-E-G-C, rising
    s_preFrames[s] = renderChime(buf, MAXF, N, 4, 0.14f, gain);
  } else if (s == audio::SND_POWER_OFF) {
    static const float N[] = {1046.5f, 784.0f, 659.3f, 523.3f};  // the same, falling
    s_preFrames[s] = renderChime(buf, MAXF, N, 4, 0.14f, gain);
  } else {
    static const float N[] = {880.0f};
    s_preFrames[s] = renderChime(buf, MAXF, N, 1, 0.5f, gain);
  }
  s_preGain[s] = gain;
}

void task(void *) {
  static const int16_t zeros[256] = {};
  for (int i = 0; i < N_SOUNDS; i++) prerender(i, s_gain.load());
  for (;;) {
    uint8_t s;
    if (xQueueReceive(s_q, &s, portMAX_DELAY) != pdTRUE) continue;
    if (s >= N_SOUNDS) {  // "volume changed" notice: recompute now, without playing
      for (int i = 0; i < N_SOUNDS; i++) prerender(i, s_gain.load());
      continue;
    }
    prerender(s, s_gain.load());  // in case the volume changed just before
    const int16_t *buf = s_pre[s];
    const size_t frames = s_preFrames[s];
    digitalWrite(PIN_PA, HIGH);
    size_t w;
    for (int i = 0; i < 8; i++) i2s_channel_write(s_tx, zeros, sizeof(zeros), &w, portMAX_DELAY);  // ~16 ms of silence
    i2s_channel_write(s_tx, buf, frames * 4, &w, portMAX_DELAY);
    for (int i = 0; i < 16; i++) i2s_channel_write(s_tx, zeros, sizeof(zeros), &w, portMAX_DELAY);
    digitalWrite(PIN_PA, LOW);
    s_busy.store(false);
  }
}
}  // namespace

namespace audio {
bool ok() { return s_ok; }
bool busy() { return s_busy.load(); }

bool begin(TwoWire &wire) {
  s_wire = &wire;
  pinMode(PIN_PA, OUTPUT);
  digitalWrite(PIN_PA, LOW);

  // What's on the bus (for diagnostics)
  char found[96] = "";
  for (uint8_t a = 0x08; a < 0x78; a++) {
    wire.beginTransmission(a);
    if (wire.endTransmission() == 0) snprintf(found + strlen(found), sizeof(found) - strlen(found), " %02X", a);
  }
  LOGF("[audio] I2C:%s\n", found);
  const int id1 = rd(ES8311, 0xFD), id2 = rd(ES8311, 0xFE);
  if (id1 < 0) {
    LOGF("[audio] ES8311 not responding\n");
    return false;
  }
  LOGF("[audio] ES8311 id %02X%02X\n", id1, id2);

  i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  cc.auto_clear = true;
  if (i2s_new_channel(&cc, &s_tx, nullptr) != ESP_OK) return false;
  i2s_std_config_t sc = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(RATE),
      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
      .gpio_cfg = {.mclk = (gpio_num_t)PIN_MCLK,
                   .bclk = (gpio_num_t)PIN_BCLK,
                   .ws = (gpio_num_t)PIN_WS,
                   .dout = (gpio_num_t)PIN_DOUT,
                   .din = I2S_GPIO_UNUSED,
                   .invert_flags = {.mclk_inv = false, .bclk_inv = false, .ws_inv = false}},
  };
  sc.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
  if (i2s_channel_init_std_mode(s_tx, &sc) != ESP_OK || i2s_channel_enable(s_tx) != ESP_OK) return false;
  delay(10);  // the codec needs MCLK before being configured

  if (!codecInit()) {
    LOGF("[audio] ES8311: configuration failed\n");
    return false;
  }
  bool mem = true;
  for (auto &b : s_pre) mem &= (b = (int16_t *)ps_malloc(MAXF * 4)) != nullptr;  // in PSRAM
  s_q = xQueueCreate(2, 1);
  if (!mem || !s_q || xTaskCreatePinnedToCore(task, "audio", 4096, nullptr, 1, nullptr, 1) != pdPASS) {
    LOGF("[audio] out of memory for sound\n");
    i2s_channel_disable(s_tx);
    return false;
  }
  s_ok = true;
  LOGF("[audio] ready\n");
  return true;
}

void setMuted(bool m) { s_muted.store(m); }
void setVolume(uint8_t level) {
  s_gain.store(GAINS[level < VOL_LEVELS ? level : VOL_LEVELS - 1]);
  if (s_ok) {
    const uint8_t v = N_SOUNDS;  // have the task recompute sounds now (if the queue is full, it does so on play)
    xQueueSend(s_q, &v, 0);
  }
}

void reserve_for_shutdown() { s_reserved = true; }
void play_shutdown() {
  if (!s_ok || s_muted.load() || s_gain.load() <= 0) return;
  bool idle = false;
  if (!s_busy.compare_exchange_strong(idle, true)) return;  // (the caller waits for the previous one to finish)
  const uint8_t v = SND_POWER_OFF;
  if (xQueueSend(s_q, &v, 0) != pdTRUE) s_busy.store(false);
}
void play(Sound s) {
  if (!s_ok || s_muted.load() || s_gain.load() <= 0 || s_reserved.load()) return;
  bool idle = false;
  if (!s_busy.compare_exchange_strong(idle, true)) return;  // already playing
  const uint8_t v = s;
  if (xQueueSend(s_q, &v, 0) != pdTRUE) s_busy.store(false);
}
}  // namespace audio

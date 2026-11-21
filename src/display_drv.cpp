// Custom CO5300 driver on esp_lcd: the pixel transfer runs via DMA in the background while
// LVGL draws the next chunk into the other buffer (partial double buffer in internal RAM).
#include "display_drv.h"

#include <Arduino.h>
#include <driver/spi_master.h>
#include <esp_heap_caps.h>
#include <esp_lcd_panel_io.h>
#if __has_include(<esp_lcd_io_spi.h>)
#include <esp_lcd_io_spi.h>
#endif

namespace display {
namespace {

// Waveshare ESP32-S3-Touch-AMOLED-1.75C pins
constexpr int PIN_CS = 12, PIN_SCLK = 38, PIN_D0 = 4, PIN_D1 = 5, PIN_D2 = 6, PIN_D3 = 7, PIN_RST = 1;
constexpr int BUF_LINES = 36;  // 466 x 36 x 2 B = 33.5 KB per buffer, x2 in DMA-capable internal RAM

esp_lcd_panel_io_handle_t io = nullptr;
lv_display_t *disp = nullptr;
int x_off = 6, y_off = 0;  // the panel has 480 columns; the visible image starts at column 6
volatile uint32_t flush_start = 0, flush_us = 0, frame_count = 0;

// CO5300 QSPI commands: 0x02 = write register (1 line), 0x32 = pixels (4 lines)
void cmd(uint8_t reg, const void *data = nullptr, size_t len = 0) {
  esp_lcd_panel_io_tx_param(io, (0x02u << 24) | ((uint32_t)reg << 8), data, len);
}
void cmd8(uint8_t reg, uint8_t v) { cmd(reg, &v, 1); }

bool IRAM_ATTR on_color_done(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t *, void *ctx) {
  flush_us = micros() - flush_start;
  lv_display_flush_ready((lv_display_t *)ctx);
  return false;
}

void flush_cb(lv_display_t *d, const lv_area_t *a, uint8_t *px) {
  const uint16_t x1 = a->x1 + x_off, x2 = a->x2 + x_off, y1 = a->y1 + y_off, y2 = a->y2 + y_off;
  const uint8_t col[4] = {(uint8_t)(x1 >> 8), (uint8_t)x1, (uint8_t)(x2 >> 8), (uint8_t)x2};
  const uint8_t row[4] = {(uint8_t)(y1 >> 8), (uint8_t)y1, (uint8_t)(y2 >> 8), (uint8_t)y2};
  cmd(0x2A, col, 4);  // waits for the previous transfer to finish
  cmd(0x2B, row, 4);
  const uint32_t n = lv_area_get_size(a);
  if (lv_display_flush_is_last(d)) frame_count++;
  lv_draw_sw_rgb565_swap(px, n);  // the panel expects big-endian RGB565
  flush_start = micros();
  esp_lcd_panel_io_tx_color(io, (0x32u << 24) | (0x2Cu << 8), px, n * 2);
}

// The CO5300 requires windows with even x/y and width/height
void rounder_cb(lv_event_t *e) {
  lv_area_t *a = (lv_area_t *)lv_event_get_param(e);
  a->x1 &= ~1;
  a->y1 &= ~1;
  a->x2 |= 1;
  a->y2 |= 1;
}

}  // namespace

lv_display_t *begin(uint32_t spi_hz) {
  const int lines = BUF_LINES;  // even (the rounder aligns to 2 lines)
  spi_bus_config_t bus = {};
  bus.sclk_io_num = PIN_SCLK;
  bus.data0_io_num = PIN_D0;
  bus.data1_io_num = PIN_D1;
  bus.data2_io_num = PIN_D2;
  bus.data3_io_num = PIN_D3;
  bus.max_transfer_sz = W * BUF_LINES * 2 + 16;
  bus.flags = SPICOMMON_BUSFLAG_MASTER | SPICOMMON_BUSFLAG_QUAD;
  ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));

  disp = lv_display_create(W, H);

  esp_lcd_panel_io_spi_config_t cfg = {};
  cfg.cs_gpio_num = PIN_CS;
  cfg.dc_gpio_num = -1;
  cfg.spi_mode = 0;
  cfg.pclk_hz = spi_hz;
  cfg.trans_queue_depth = 10;
  cfg.on_color_trans_done = on_color_done;
  cfg.user_ctx = disp;
  cfg.lcd_cmd_bits = 32;
  cfg.lcd_param_bits = 8;
  cfg.flags.quad_mode = 1;
  ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &cfg, &io));

  // CO5300 reset and init sequence (same as Arduino_GFX / Waveshare)
  pinMode(PIN_RST, OUTPUT);
  digitalWrite(PIN_RST, HIGH);
  delay(10);
  digitalWrite(PIN_RST, LOW);
  delay(20);
  digitalWrite(PIN_RST, HIGH);
  delay(120);
  cmd(0x11);  // sleep out
  delay(120);
  cmd8(0xFE, 0x00);
  cmd8(0xC4, 0x80);  // SPI mode
  cmd8(0x3A, 0x55);  // RGB565
  cmd8(0x53, 0x20);
  cmd8(0x63, 0xFF);
  cmd8(0x36, 0x00);  // MADCTL
  cmd(0x29);         // display on
  cmd8(0x51, 0x00);  // brightness 0 until the first frame is ready
  cmd8(0x58, 0x00);
  delay(10);

  // If DMA-capable internal RAM is short, try half the lines (still even); with no buffer, fail
  size_t bytes = W * lines * 2;
  void *b1 = heap_caps_malloc(bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  if (!b1) {
    bytes = W * (lines / 2) * 2;
    b1 = heap_caps_malloc(bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  }
  if (!b1) return nullptr;
  void *b2 = heap_caps_malloc(bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);  // without the second: single buffer
  lv_display_set_buffers(disp, b1, b2, bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_display_set_flush_cb(disp, flush_cb);
  lv_display_add_event_cb(disp, rounder_cb, LV_EVENT_INVALIDATE_AREA, nullptr);
  return disp;
}

void set_brightness(uint8_t v) { cmd8(0x51, v); }

// CO5300 timing (datasheet): at least 120 ms between SLPIN and SLPOUT (and vice versa); after each one,
// 5 ms with no other commands. If touched right as it turns off, waits the remainder (blocks <= 120 ms).
void set_sleep(bool on) {
  static bool asleep = false;
  static uint32_t lastAt = 0;
  if (on == asleep) return;
  asleep = on;
  const uint32_t since = millis() - lastAt;
  if (lastAt && since < 120) delay(120 - since);
  if (on) {
    cmd(0x28);  // display off
    cmd(0x10);  // sleep in
    lastAt = millis();
    delay(5);
  } else {
    cmd(0x11);  // sleep out
    lastAt = millis();
    delay(10);
    cmd(0x29);  // display on
    if (disp) lv_obj_invalidate(lv_display_get_screen_active(disp));  // redraw everything on wake
  }
}

void set_rotation180(bool on) {
  cmd8(0x36, on ? 0x07 : 0x00);  // mirror X + Y (like Arduino_CO5300::setRotation(2))
  x_off = on ? 0 : 6;
  y_off = 0;
  if (disp) lv_obj_invalidate(lv_display_get_screen_active(disp));
}

uint32_t last_flush_us() { return flush_us; }
uint32_t frames() { return frame_count; }

}  // namespace display

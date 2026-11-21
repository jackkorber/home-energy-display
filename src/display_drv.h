#pragma once
#include <lvgl.h>

// CO5300 466x466 AMOLED over QSPI with DMA (esp_lcd) + LVGL display in partial mode.
// Used only from the display task.
namespace display {

constexpr int W = 466, H = 466;

// starts bus, panel and LVGL (lv_init already called)
lv_display_t *begin(uint32_t spi_hz);
void set_brightness(uint8_t v);        // 0..255
void set_rotation180(bool on);
void set_sleep(bool on);              // panel truly off (0x28 + 0x10): saves more than brightness 0         // hardware rotation (MADCTL)
uint32_t last_flush_us();              // duration of the last flush (diagnostics)
uint32_t frames();                     // full frames sent (to measure fps)

}  // namespace display

#pragma once
#include <lvgl.h>

#include "model.h"

// LVGL interface (display task only). Three screens: energy, month and weather.
namespace ui {
void build();
// Updates everything with the data; returns true if something important (outage or severe alert) applies at night
bool update(const Energy &e, const Weather &w, uint32_t nowMs);
void shift(int dx, int dy);  // shift everything a couple of pixels (protects the AMOLED)
void next_page();
void show_page(int n);              // 0 energy, 1 now, 2 month, 3 VPP, 4 weather
void set_place(const char *name);   // weather title
bool important_alert(const Alert &a);  // alert that sounds at night and keeps the display on
}  // namespace ui

#include "model.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace model {
namespace {
SemaphoreHandle_t mux = nullptr;
Energy s_e;
Weather s_w;
}  // namespace

void begin() { mux = xSemaphoreCreateMutex(); }

void get(Energy &e, Weather &w) {
  xSemaphoreTake(mux, portMAX_DELAY);
  e = s_e;
  w = s_w;
  xSemaphoreGive(mux);
}

void put(const Energy &e) {
  xSemaphoreTake(mux, portMAX_DELAY);
  s_e = e;
  xSemaphoreGive(mux);
}

void put(const Weather &w) {
  xSemaphoreTake(mux, portMAX_DELAY);
  s_w = w;
  xSemaphoreGive(mux);
}
}  // namespace model

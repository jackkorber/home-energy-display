// LVGL memory in PSRAM: frees internal RAM for WiFi/TLS and the display's DMA buffers.
#include <lvgl.h>
#if LV_USE_STDLIB_MALLOC == LV_STDLIB_CUSTOM
#include <esp_heap_caps.h>

void lv_mem_init(void) {}
void lv_mem_deinit(void) {}
lv_mem_pool_t lv_mem_add_pool(void *mem, size_t bytes) {
  LV_UNUSED(mem);
  LV_UNUSED(bytes);
  return NULL;
}
void lv_mem_remove_pool(lv_mem_pool_t pool) { LV_UNUSED(pool); }

// Tested in v1.9.0: putting small blocks in internal RAM left only ~20 KB free (Bluetooth starts
// later and no longer has enough). All of LVGL stays in PSRAM.
void *lv_malloc_core(size_t size) {
  void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return p ? p : heap_caps_malloc(size, MALLOC_CAP_8BIT);
}
void *lv_realloc_core(void *p, size_t new_size) {
  void *n = heap_caps_realloc(p, new_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return n ? n : heap_caps_realloc(p, new_size, MALLOC_CAP_8BIT);
}
void lv_free_core(void *p) { heap_caps_free(p); }

void lv_mem_monitor_core(lv_mem_monitor_t *mon_p) {
  lv_memzero(mon_p, sizeof(lv_mem_monitor_t));
  mon_p->total_size = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
  mon_p->free_size = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  mon_p->free_biggest_size = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
}
lv_result_t lv_mem_test_core(void) { return LV_RESULT_OK; }
#endif

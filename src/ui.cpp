// Screens: 1) home energy (below it: "Now")  2) month totals and bill  3) VPP  4) weather (NWS, forecast, NHC)
// 466 px round display: everything is positioned relative to the center.
#include "ui.h"

#include <Arduino.h>
#include <math.h>
#include <time.h>

#include "config.h"
#include "log.h"

LV_FONT_DECLARE(font_digits_150);
LV_FONT_DECLARE(font_digits_84);
LV_FONT_DECLARE(font_tech_40);
LV_FONT_DECLARE(font_ui_22);
LV_FONT_DECLARE(font_ui_30);

namespace ui {
namespace {

// Palette (AMOLED: pure black)
const lv_color_t C_BG = lv_color_hex(0x000000);
const lv_color_t C_TRACK = lv_color_hex(0x1C2228);
const lv_color_t C_DIM = lv_color_hex(0x7A848E);
const lv_color_t C_FAINT = lv_color_hex(0x3A4148);
const lv_color_t C_TEXT = lv_color_hex(0xFFFFFF);
const lv_color_t C_ACCENT = lv_color_hex(0x009EFF);
const lv_color_t C_GREEN = lv_color_hex(0x2EE66B);
const lv_color_t C_SOLAR = lv_color_hex(0xFFD23F);
const lv_color_t C_AMBER = lv_color_hex(0xFFB020);
const lv_color_t C_RED = lv_color_hex(0xFF3B3B);

lv_obj_t *tv, *tileE, *tileF, *tileM, *tileV, *tileW;
// Energy
lv_obj_t *arc, *clockL, *pill, *pillDot, *pillL, *pctRow, *pctNum, *pctSign, *backupL, *flowL;
lv_obj_t *alertRow, *alertHead, *alertL;
lv_obj_t *nowHint;
// Now (below energy): live power for solar, home, battery and grid
lv_obj_t *fT[4], *fV[4];
// Month
lv_obj_t *mTitle, *billRow, *billSign, *billNum, *billL, *mT[4], *mV[4], *mNetL, *mFromL, *mUp, *mDown;
int mSel = 0;    // 0 = current month; 1 = previous stored one; ...
int mCount = 1;  // months that can be viewed (current + history)
// VPP
lv_obj_t *vTitle, *vSign, *vNum, *vL, *vT[2], *vV[2], *vNowL, *vRateL, *vUp, *vDown;
int vSel = 0, vCount = 1;
// Weather
lv_obj_t *wxTitle, *tempRow, *tempNum, *tempUnit, *condL, *rangeL, *alertsBox, *alertName[2], *alertWhen[2], *moreL,
    *stormL1, *stormL2;

lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, lv_color_t c, const char *txt = "") {
  lv_obj_t *l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, c, 0);
  lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_text(l, txt);
  return l;
}

lv_obj_t *box(lv_obj_t *parent, lv_flex_flow_t flow) {
  lv_obj_t *b = lv_obj_create(parent);
  lv_obj_remove_style_all(b);
  lv_obj_set_size(b, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(b, flow);
  lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_remove_flag(b, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
  return b;
}

// Only touch LVGL if it changed (every change redraws)
void setText(lv_obj_t *l, const char *s) {
  if (strcmp(lv_label_get_text(l), s)) lv_label_set_text(l, s);
}
void setColor(lv_obj_t *l, lv_color_t c) {
  if (!lv_color_eq(lv_obj_get_style_text_color(l, 0), c)) lv_obj_set_style_text_color(l, c, 0);
}
void setHidden(lv_obj_t *o, bool h) {
  if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN) != h) {
    if (h) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
  }
}

// Power company name (config UTILITY_NAME) and its uppercase version for the small titles
const char *U = UTILITY_NAME;
char U_UP[24];
void initNames() {
  size_t i = 0;
  for (; U[i] && i < sizeof(U_UP) - 1; i++) U_UP[i] = (char)toupper((unsigned char)U[i]);
  U_UP[i] = 0;
}
// "DE LUMA", "Comprando a LUMA"...: build a label text with the company name
const char *withU(const char *fmt, bool upper = false) {
  static char buf[4][48];
  static int n = 0;
  char *b = buf[n++ % 4];
  snprintf(b, sizeof(buf[0]), fmt, upper ? U_UP : U);
  return b;
}
int g_pages = 4;  // horizontal pages (3 without VPP)
const char *g_place = HOME_NAME;  // weather title (demo mode shows a sample town)

void pageDots(lv_obj_t *tile, int active) {
  lv_obj_t *row = box(tile, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(row, 10, 0);
  lv_obj_align(row, LV_ALIGN_CENTER, 0, 212);
  for (int i = 0; i < g_pages; i++) {
    lv_obj_t *d = lv_obj_create(row);
    lv_obj_remove_style_all(d);
    lv_obj_set_size(d, 8, 8);
    lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(d, i == active ? C_DIM : C_FAINT, 0);
  }
}

// ---------- Text ----------
const char *const WDAY[] = {"dom", "lun", "mar", "mié", "jue", "vie", "sáb"};
const char *const MONTH[] = {"enero", "febrero", "marzo", "abril", "mayo", "junio", "julio",
                             "agosto", "septiembre", "octubre", "noviembre", "diciembre"};
const char *const MON3[] = {"ene", "feb", "mar", "abr", "may", "jun", "jul", "ago", "sep", "oct", "nov", "dic"};
const char *const MONTH_UP[] = {"ENERO", "FEBRERO", "MARZO", "ABRIL", "MAYO", "JUNIO", "JULIO",
                                "AGOSTO", "SEPTIEMBRE", "OCTUBRE", "NOVIEMBRE", "DICIEMBRE"};

void fmtTime(char *b, size_t n, time_t t) {
  struct tm tm;
  localtime_r(&t, &tm);
  const int h = tm.tm_hour % 12 ? tm.tm_hour % 12 : 12;
  snprintf(b, n, "%d:%02d %s", h, tm.tm_min, tm.tm_hour < 12 ? "am" : "pm");
}

void fmtKw(char *b, size_t n, float w) {
  if (!isfinite(w)) snprintf(b, n, "--");
  else snprintf(b, n, "%.1f kW", fabsf(w) / 1000.0f);
}

void fmtHours(char *b, size_t n, float h) {
  if (h >= 48) snprintf(b, n, "%d días", (int)lroundf(h / 24));
  else if (h >= 1) snprintf(b, n, "%d h", (int)lroundf(h));
  else snprintf(b, n, "%d min", max(1, (int)lroundf(h * 60)));
}

struct Tr {
  const char *en, *es;
};
// Names used by NWS San Juan in Spanish (Aviso = Warning, Vigilancia = Watch, Advertencia = Advisory)
const Tr EVENTS[] = {
    {"Flash Flood Warning", "Aviso de inundaciones repentinas"},
    {"Flash Flood Watch", "Vigilancia de inundaciones repentinas"},
    {"Flash Flood Statement", "Declaración de inundaciones repentinas"},
    {"Flood Warning", "Aviso de inundaciones"},
    {"Flood Watch", "Vigilancia de inundaciones"},
    {"Flood Advisory", "Advertencia de inundaciones"},
    {"Flood Statement", "Declaración de inundaciones"},
    {"Hurricane Warning", "Aviso de huracán"},
    {"Hurricane Watch", "Vigilancia de huracán"},
    {"Hurricane Local Statement", "Declaración local de huracán"},
    {"Tropical Storm Warning", "Aviso de tormenta tropical"},
    {"Tropical Storm Watch", "Vigilancia de tormenta tropical"},
    {"Storm Surge Warning", "Aviso de marejada ciclónica"},
    {"Storm Surge Watch", "Vigilancia de marejada ciclónica"},
    {"Tornado Warning", "Aviso de tornado"},
    {"Severe Thunderstorm Warning", "Aviso de tronada severa"},
    {"Special Weather Statement", "Declaración especial del tiempo"},
    {"Heat Advisory", "Advertencia de calor"},
    {"Excessive Heat Warning", "Aviso de calor excesivo"},
    {"Extreme Heat Warning", "Aviso de calor extremo"},
    {"Extreme Heat Watch", "Vigilancia de calor extremo"},
    {"High Surf Advisory", "Advertencia de oleaje alto"},
    {"High Surf Warning", "Aviso de oleaje alto"},
    {"Rip Current Statement", "Corrientes marinas peligrosas"},
    {"Small Craft Advisory", "Advertencia: embarcaciones pequeñas"},
    {"Coastal Flood Advisory", "Advertencia de inundaciones costeras"},
    {"Wind Advisory", "Advertencia de viento"},
    {"Tsunami Warning", "Aviso de tsunami"},
    {"Tsunami Advisory", "Advertencia de tsunami"},
    {"Tsunami Watch", "Vigilancia de tsunami"},
};
const char *eventEs(const char *en) {
  for (const Tr &t : EVENTS)
    if (!strcmp(t.en, en)) return t.es;
  return en;
}

lv_color_t alertColor(const Alert &a) {
  if (!strcmp(a.severity, "Extreme") || strstr(a.event, "Warning")) return C_RED;
  if (!strcmp(a.severity, "Severe") || strstr(a.event, "Watch")) return C_AMBER;
  return C_SOLAR;
}

void fmtUntil(char *b, size_t n, time_t ends) {
  if (!ends) {
    snprintf(b, n, "vigente");
    return;
  }
  char hm[16];
  fmtTime(hm, sizeof(hm), ends);
  time_t now;
  time(&now);
  struct tm a, e;
  localtime_r(&now, &a);
  localtime_r(&ends, &e);
  if (a.tm_yday == e.tm_yday && a.tm_year == e.tm_year) snprintf(b, n, "hasta las %s", hm);
  else snprintf(b, n, "hasta el %s %s", WDAY[e.tm_wday], hm);
}

const char *sky(int code) {
  switch (code) {
    case 0: return "Despejado";
    case 1: return "Mayormente despejado";
    case 2: return "Parcialmente nublado";
    case 3: return "Nublado";
    case 45: case 48: return "Neblina";
    case 51: case 53: case 55: case 56: case 57: return "Llovizna";
    case 61: return "Lluvia ligera";
    case 63: return "Lluvia";
    case 65: return "Lluvia fuerte";
    case 66: case 67: return "Lluvia helada";
    case 80: return "Aguaceros ligeros";
    case 81: return "Aguaceros";
    case 82: return "Aguaceros fuertes";
    case 95: return "Tronadas";
    case 96: case 99: return "Tronadas con granizo";
    default: return code >= 71 && code <= 86 ? "Nieve" : "--";
  }
}

void stormName(char *b, size_t n, const Storm &s) {
  const char *c = s.cls;
  if (!strcmp(c, "HU") || !strcmp(c, "MH")) {
    const int cat = s.mph >= 157 ? 5 : s.mph >= 130 ? 4 : s.mph >= 111 ? 3 : s.mph >= 96 ? 2 : 1;
    snprintf(b, n, "Huracán %s (cat. %d)", s.name, cat);
  } else if (!strcmp(c, "TS")) snprintf(b, n, "Tormenta tropical %s", s.name);
  else if (!strcmp(c, "TD")) snprintf(b, n, "Depresión tropical %s", s.name);
  else if (!strcmp(c, "STS")) snprintf(b, n, "Tormenta subtropical %s", s.name);
  else if (!strcmp(c, "SD")) snprintf(b, n, "Depresión subtropical %s", s.name);
  else if (!strcmp(c, "PTC") || !strcmp(c, "PC")) snprintf(b, n, "Posible ciclón %s", s.name);
  else snprintf(b, n, "%s %s", c, s.name);
}

// ---------- Construction ----------
void buildEnergy(lv_obj_t *t) {
  arc = lv_arc_create(t);
  lv_obj_set_size(arc, 446, 446);
  lv_obj_center(arc);
  lv_arc_set_bg_angles(arc, 135, 45);  // 270° with the gap at the bottom
  lv_arc_set_range(arc, 0, 100);
  lv_arc_set_value(arc, 0);
  lv_obj_remove_style(arc, nullptr, LV_PART_KNOB);
  lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_width(arc, 18, LV_PART_MAIN);
  lv_obj_set_style_arc_width(arc, 18, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(arc, C_TRACK, LV_PART_MAIN);
  lv_obj_set_style_arc_color(arc, C_GREEN, LV_PART_INDICATOR);
  lv_obj_set_style_arc_rounded(arc, true, LV_PART_INDICATOR);

  clockL = label(t, &font_ui_22, C_DIM, "--:--");
  lv_obj_align(clockL, LV_ALIGN_CENTER, 0, -172);

  pill = box(t, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_hor(pill, 16, 0);
  lv_obj_set_style_pad_ver(pill, 6, 0);
  lv_obj_set_style_pad_column(pill, 10, 0);
  lv_obj_set_style_radius(pill, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(pill, C_RED, 0);
  lv_obj_set_style_bg_opa(pill, LV_OPA_TRANSP, 0);
  lv_obj_align(pill, LV_ALIGN_CENTER, 0, -128);
  pillDot = lv_obj_create(pill);
  lv_obj_remove_style_all(pillDot);
  lv_obj_set_size(pillDot, 14, 14);
  lv_obj_set_style_radius(pillDot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_opa(pillDot, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(pillDot, C_DIM, 0);
  pillL = label(pill, &font_ui_22, C_TEXT, "Conectando...");

  pctRow = box(t, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(pctRow, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
  lv_obj_align(pctRow, LV_ALIGN_CENTER, 0, -32);
  pctNum = label(pctRow, &font_digits_150, C_TEXT, "--");
  pctSign = label(pctRow, &font_tech_40, C_DIM, "%");
  lv_obj_set_style_pad_bottom(pctSign, 22, 0);

  backupL = label(t, &font_ui_22, C_ACCENT, "");
  lv_obj_align(backupL, LV_ALIGN_CENTER, 0, 60);
  flowL = label(t, &font_ui_22, C_DIM, "");
  lv_obj_align(flowL, LV_ALIGN_CENTER, 0, 98);

  // NWS alert: a heading saying what it is + the alert name. Tapping it goes to the weather screen.
  alertRow = box(t, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(alertRow, 0, 0);
  lv_obj_set_style_pad_ver(alertRow, 4, 0);
  lv_obj_align(alertRow, LV_ALIGN_CENTER, 0, 96);
  lv_obj_add_flag(alertRow, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(alertRow, [](lv_event_t *) { lv_tileview_set_tile(tv, tileW, LV_ANIM_ON); }, LV_EVENT_CLICKED,
                      nullptr);
  alertHead = label(alertRow, &lv_font_montserrat_14, C_DIM, "");
  alertL = label(alertRow, &font_ui_22, C_AMBER, "");
  lv_label_set_long_mode(alertL, LV_LABEL_LONG_DOT);
  lv_obj_set_width(alertL, 340);
  lv_obj_add_flag(alertRow, LV_OBJ_FLAG_HIDDEN);

  // What's happening now (solar, home, battery, grid) is on the screen below: swipe up or tap here
  nowHint = label(t, &lv_font_montserrat_14, C_DIM, "AHORA " LV_SYMBOL_DOWN);
  lv_obj_set_style_pad_all(nowHint, 10, 0);
  lv_obj_align(nowHint, LV_ALIGN_CENTER, 0, 170);
  lv_obj_add_flag(nowHint, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(nowHint, [](lv_event_t *) { lv_tileview_set_tile(tv, tileF, LV_ANIM_ON); }, LV_EVENT_CLICKED,
                      nullptr);
  pageDots(t, 0);
}

void buildFlow(lv_obj_t *t) {
  lv_obj_t *up = label(t, &lv_font_montserrat_14, C_DIM, LV_SYMBOL_UP);
  lv_obj_set_style_pad_all(up, 10, 0);
  lv_obj_align(up, LV_ALIGN_CENTER, 0, -196);
  lv_obj_add_flag(up, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(up, [](lv_event_t *) { lv_tileview_set_tile(tv, tileE, LV_ANIM_ON); }, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *title = label(t, &font_ui_22, C_DIM, "AHORA");
  lv_obj_align(title, LV_ALIGN_CENTER, 0, -150);
  static const char *const TITLES[4] = {"Solar", "Casa", "Batería", ""};
  for (int i = 0; i < 4; i++) {
    lv_obj_t *row = box(t, LV_FLEX_FLOW_ROW);
    lv_obj_set_width(row, 330);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_align(row, LV_ALIGN_CENTER, 0, -82 + i * 64);
    fT[i] = label(row, &font_ui_22, C_DIM, i == 3 ? U : TITLES[i]);
    fV[i] = label(row, &font_ui_30, C_TEXT, "--");
    if (i < 3) {  // thin line between rows
      lv_obj_t *ln = lv_obj_create(t);
      lv_obj_remove_style_all(ln);
      lv_obj_set_size(ln, 330, 1);
      lv_obj_set_style_bg_opa(ln, LV_OPA_COVER, 0);
      lv_obj_set_style_bg_color(ln, C_TRACK, 0);
      lv_obj_align(ln, LV_ALIGN_CENTER, 0, -82 + i * 64 + 32);
    }
  }
}

void buildMonth(lv_obj_t *t) {
  mTitle = label(t, &font_ui_22, C_DIM, "ESTE MES");
  lv_obj_align(mTitle, LV_ALIGN_CENTER, 0, -178);

  billRow = box(t, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(billRow, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
  lv_obj_set_style_pad_column(billRow, 4, 0);
  lv_obj_align(billRow, LV_ALIGN_CENTER, 0, -112);
  billSign = label(billRow, &font_tech_40, C_DIM, "$");
  lv_obj_set_style_pad_top(billSign, 8, 0);
  billNum = label(billRow, &font_digits_84, C_TEXT, "--");
  billL = label(t, &font_ui_22, C_DIM, "");
  lv_obj_align(billL, LV_ALIGN_CENTER, 0, -50);

  static const char *const TITLES[2] = {"SOLAR", "CASA"};
  for (int i = 0; i < 4; i++) {
    lv_obj_t *c = box(t, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_width(c, 170);
    lv_obj_set_style_pad_row(c, 0, 0);
    lv_obj_align(c, LV_ALIGN_CENTER, i % 2 ? 85 : -85, i < 2 ? 2 : 72);
    mT[i] = label(c, &lv_font_montserrat_14, C_DIM, i < 2 ? TITLES[i] : withU(i == 2 ? "DE %s" : "A %s", true));
    mV[i] = label(c, &font_ui_30, C_TEXT, "--");
  }
  mNetL = label(t, &font_ui_22, C_DIM, "");
  lv_obj_align(mNetL, LV_ALIGN_CENTER, 0, 138);
  mFromL = label(t, &font_ui_22, C_FAINT, "");
  lv_obj_align(mFromL, LV_ALIGN_CENTER, 0, 168);
  // Arrows: there are older months (up) or newer ones (down). Swipe up = previous month.
  mUp = label(t, &lv_font_montserrat_14, C_DIM, LV_SYMBOL_UP);
  lv_obj_align(mUp, LV_ALIGN_CENTER, 0, -205);
  mDown = label(t, &lv_font_montserrat_14, C_DIM, LV_SYMBOL_DOWN);
  lv_obj_align(mDown, LV_ALIGN_CENTER, 0, 193);
  // LVGL sends the gesture to the first parent without GESTURE_BUBBLE: remove it here so this screen gets it
  // (otherwise it bubbles up to the tileview and never arrives)
  lv_obj_remove_flag(t, LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_add_event_cb(t, [](lv_event_t *) {
    const lv_dir_t d = lv_indev_get_gesture_dir(lv_indev_active());
    if (d == LV_DIR_TOP && mSel < mCount - 1) mSel++;
    else if (d == LV_DIR_BOTTOM && mSel > 0) mSel--;
    LOGF("[ui] gesture %d -> month %d of %d\n", (int)d, mSel, mCount);
  }, LV_EVENT_GESTURE, nullptr);
  pageDots(t, 1);
}

// Arrows + swipe up/down to change month (like on the month screen)
void historyNav(lv_obj_t *t, lv_obj_t **up, lv_obj_t **down, int *sel, int *count) {
  *up = label(t, &lv_font_montserrat_14, C_DIM, LV_SYMBOL_UP);
  lv_obj_align(*up, LV_ALIGN_CENTER, 0, -205);
  *down = label(t, &lv_font_montserrat_14, C_DIM, LV_SYMBOL_DOWN);
  lv_obj_align(*down, LV_ALIGN_CENTER, 0, 193);
  lv_obj_remove_flag(t, LV_OBJ_FLAG_GESTURE_BUBBLE);
  struct Nav {
    int *sel, *count;
  };
  Nav *nav = new Nav{sel, count};
  lv_obj_add_event_cb(t, [](lv_event_t *ev) {
    Nav *n = (Nav *)lv_event_get_user_data(ev);
    const lv_dir_t d = lv_indev_get_gesture_dir(lv_indev_active());
    if (d == LV_DIR_TOP && *n->sel < *n->count - 1) (*n->sel)++;
    else if (d == LV_DIR_BOTTOM && *n->sel > 0) (*n->sel)--;
  }, LV_EVENT_GESTURE, nav);
}

void buildVpp(lv_obj_t *t) {
  vTitle = label(t, &font_ui_22, C_DIM, "VPP");
  lv_obj_align(vTitle, LV_ALIGN_CENTER, 0, -178);
  lv_obj_t *row = box(t, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
  lv_obj_set_style_pad_column(row, 4, 0);
  lv_obj_align(row, LV_ALIGN_CENTER, 0, -112);
  vSign = label(row, &font_tech_40, C_GREEN, "$");
  lv_obj_set_style_pad_top(vSign, 8, 0);
  vNum = label(row, &font_digits_84, C_GREEN, "--");
  vL = label(t, &font_ui_22, C_GREEN, "ganado con la batería");
  lv_obj_align(vL, LV_ALIGN_CENTER, 0, -50);
  static const char *const TITLES[1] = {"EVENTOS"};
  for (int i = 0; i < 2; i++) {
    lv_obj_t *c = box(t, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_width(c, 170);
    lv_obj_align(c, LV_ALIGN_CENTER, i ? 85 : -85, 10);
    vT[i] = label(c, &lv_font_montserrat_14, C_DIM, i ? withU("ENVIADO A %s", true) : TITLES[i]);
    vV[i] = label(c, &font_ui_30, C_TEXT, "--");
  }
  vNowL = label(t, &font_ui_22, C_DIM, "");
  lv_obj_align(vNowL, LV_ALIGN_CENTER, 0, 85);
  vRateL = label(t, &font_ui_22, C_FAINT, "");
  lv_obj_align(vRateL, LV_ALIGN_CENTER, 0, 140);
  historyNav(t, &vUp, &vDown, &vSel, &vCount);
  pageDots(t, 2);
}

void buildWeather(lv_obj_t *t) {
  wxTitle = label(t, &font_ui_22, C_DIM, g_place);
  lv_obj_align(wxTitle, LV_ALIGN_CENTER, 0, -178);

  tempRow = box(t, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(tempRow, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
  lv_obj_set_style_pad_column(tempRow, 4, 0);
  lv_obj_align(tempRow, LV_ALIGN_CENTER, 0, -108);
  tempNum = label(tempRow, &font_digits_84, C_TEXT, "--");
  tempUnit = label(tempRow, &font_ui_30, C_DIM, "°F");
  lv_obj_set_style_pad_top(tempUnit, 10, 0);

  condL = label(t, &font_ui_22, C_TEXT, "");
  lv_obj_align(condL, LV_ALIGN_CENTER, 0, -48);
  rangeL = label(t, &font_ui_22, C_DIM, "");
  lv_obj_align(rangeL, LV_ALIGN_CENTER, 0, -18);

  alertsBox = box(t, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_width(alertsBox, 400);
  lv_obj_set_style_pad_row(alertsBox, 0, 0);
  lv_obj_align(alertsBox, LV_ALIGN_TOP_MID, 0, 233 + 14);
  for (int i = 0; i < 2; i++) {
    alertName[i] = label(alertsBox, &font_ui_22, C_RED, "");
    lv_label_set_long_mode(alertName[i], LV_LABEL_LONG_DOT);
    lv_obj_set_width(alertName[i], 390);
    alertWhen[i] = label(alertsBox, &font_ui_22, C_DIM, "");
    lv_obj_set_style_pad_bottom(alertWhen[i], 6, 0);
  }
  moreL = label(alertsBox, &font_ui_22, C_DIM, "");

  stormL1 = label(t, &font_ui_22, C_DIM, "");
  lv_obj_align(stormL1, LV_ALIGN_CENTER, 0, 152);
  stormL2 = label(t, &font_ui_22, C_DIM, "");
  lv_obj_align(stormL2, LV_ALIGN_CENTER, 0, 180);
  pageDots(t, g_pages - 1);
}

// ---------- Update ----------
lv_color_t pctColor(float p) { return p >= 50 ? C_GREEN : p >= 20 ? C_AMBER : C_RED; }

void updateEnergy(const Energy &e, const Weather &w, uint32_t nowMs) {
  char b[64];
  time_t now;
  time(&now);
  if (now > 1700000000) {
    fmtTime(b, sizeof(b), now);
    setText(clockL, b);
  }

  const bool fresh = e.at && nowMs - e.at < 30000;  // data less than 30 s old
  const bool outage = fresh && e.grid == 0;
  const bool blinkOn = (nowMs / 600) % 2;

  // Status pill: company name / APAGÓN (outage) / connection problems
  lv_color_t dot = C_DIM;
  lv_opa_t bgOpa = LV_OPA_TRANSP;
  const char *txt;
  switch (e.status) {
    case GwStatus::NO_CONFIG: txt = "Falta configurar"; dot = C_AMBER; break;
    case GwStatus::NO_WIFI: txt = "Sin WiFi"; dot = C_AMBER; break;
    case GwStatus::BAD_PASSWORD: txt = "Contraseña del Gateway"; dot = C_RED; break;
    case GwStatus::LOCKED: txt = "Gateway bloqueado"; dot = C_RED; break;
    case GwStatus::UNREACHABLE: txt = "Gateway sin respuesta"; dot = C_AMBER; break;
    case GwStatus::STARTING:
    case GwStatus::LOGIN: txt = "Conectando..."; break;
    default:
      if (!fresh) {
        txt = "Sin datos";
        dot = C_AMBER;
      } else if (outage) {
        txt = "APAGÓN";
        dot = C_TEXT;
        bgOpa = blinkOn ? LV_OPA_COVER : LV_OPA_40;
      } else if (e.grid == 1) {
        txt = U;
        dot = C_GREEN;
      } else {
        txt = withU("%s: cambiando");
        dot = C_AMBER;
      }
  }
  setText(pillL, txt);
  if (!lv_color_eq(lv_obj_get_style_bg_color(pillDot, 0), dot)) lv_obj_set_style_bg_color(pillDot, dot, 0);
  if (lv_obj_get_style_bg_opa(pill, 0) != bgOpa) lv_obj_set_style_bg_opa(pill, bgOpa, 0);

  // Battery
  const bool havePct = fresh && isfinite(e.pct);
  const int pct = havePct ? (int)lroundf(e.pct) : 0;
  if (havePct) snprintf(b, sizeof(b), "%d", pct);
  setText(pctNum, havePct ? b : "--");
  setColor(pctNum, havePct ? C_TEXT : C_DIM);
  lv_arc_set_value(arc, pct);
  const lv_color_t ac = havePct ? pctColor(e.pct) : C_FAINT;
  if (!lv_color_eq(lv_obj_get_style_arc_color(arc, LV_PART_INDICATOR), ac))
    lv_obj_set_style_arc_color(arc, ac, LV_PART_INDICATOR);

  // How long it lasts: in an outage, from home usage minus solar; on grid, from home consumption
  b[0] = 0;
  lv_color_t bc = C_ACCENT;
  if (fresh && isfinite(e.usable_wh)) {
    char h[16];
    if (outage) {
      if (isfinite(e.net_avg_w) && e.net_avg_w < 100) {
        snprintf(b, sizeof(b), "La solar cubre la casa");
        bc = C_GREEN;
      } else if (isfinite(e.net_avg_w)) {
        fmtHours(h, sizeof(h), e.usable_wh / e.net_avg_w);
        snprintf(b, sizeof(b), "Quedan ~%s", h);
        bc = e.pct >= 20 ? C_AMBER : C_RED;
      }
    } else if (isfinite(e.home_avg_w)) {
      fmtHours(h, sizeof(h), e.usable_wh / max(e.home_avg_w, 100.0f));
      snprintf(b, sizeof(b), "Respaldo ~%s", h);
    }
  }
  setText(backupL, b);
  setColor(backupL, bc);

  // Battery line, or the most severe NWS alert if there is one
  const bool vppNow = fresh && e.vpp_active;
  const bool showAlert = w.n_alerts > 0 && !vppNow;
  setHidden(alertRow, !showAlert);
  setHidden(flowL, showAlert);
  if (showAlert) {
    setText(alertL, eventEs(w.alerts[0].event));
    const lv_color_t c = alertColor(w.alerts[0]);
    setColor(alertL, c);
    setColor(alertHead, c);
    char head[40];
    if (w.n_alerts > 1) snprintf(head, sizeof(head), "%d AVISOS DEL TIEMPO " LV_SYMBOL_RIGHT, w.n_alerts);
    else snprintf(head, sizeof(head), "AVISO DEL TIEMPO " LV_SYMBOL_RIGHT);
    setText(alertHead, head);
  } else {
    b[0] = 0;
    lv_color_t fc = C_DIM;
    if (vppNow) {
      char kw[16];
      fmtKw(kw, sizeof(kw), e.vpp_w);
      snprintf(b, sizeof(b), "Evento VPP: enviando %s", kw);
      fc = C_GREEN;
    } else if (fresh && isfinite(e.battery_w)) {
      char kw[16];
      fmtKw(kw, sizeof(kw), e.battery_w);
      if (e.battery_w < -50) {
        snprintf(b, sizeof(b), "Cargando %s", kw);
        fc = C_GREEN;
      } else if (e.battery_w > 50) snprintf(b, sizeof(b), "Usando la batería %s", kw);
      else snprintf(b, sizeof(b), havePct && pct >= 100 ? "Batería llena" : "Batería en espera");
    }
    setText(flowL, b);
    setColor(flowL, fc);
  }
}

// "Now" screen: each row says what is happening (charging/using, buying/selling) and how much
void updateFlow(const Energy &e, uint32_t nowMs) {
  char b[32];
  const bool fresh = e.at && nowMs - e.at < 30000;
  const bool outage = fresh && e.grid == 0;
  fmtKw(b, sizeof(b), fresh ? e.solar_w : NAN);
  setText(fV[0], b);
  setColor(fV[0], fresh && e.solar_w > 50 ? C_SOLAR : C_TEXT);
  fmtKw(b, sizeof(b), fresh ? e.home_w : NAN);
  setText(fV[1], b);

  fmtKw(b, sizeof(b), fresh ? e.battery_w : NAN);
  setText(fV[2], b);
  if (fresh && e.battery_w < -50) {
    setText(fT[2], "Batería cargando");
    setColor(fV[2], C_GREEN);
  } else if (fresh && e.battery_w > 50) {
    setText(fT[2], "Usando la batería");
    setColor(fV[2], C_AMBER);
  } else {
    setText(fT[2], "Batería");
    setColor(fV[2], C_TEXT);
  }

  if (!fresh || outage) {
    setText(fT[3], outage ? withU("%s (apagón)") : U);
    setText(fV[3], "--");
    setColor(fV[3], outage ? C_RED : C_DIM);
  } else {
    const bool selling = e.grid_w < -50, buying = e.grid_w > 50;
    setText(fT[3], selling ? withU("Vendiendo a %s") : buying ? withU("Comprando a %s") : U);
    fmtKw(b, sizeof(b), e.grid_w);
    setText(fV[3], b);
    setColor(fV[3], selling ? C_GREEN : C_TEXT);
  }
}

// Estimated bill with net metering: (bought - sold) x rate + fixed charge. Negative means credit.
void updateMonth(const Energy &e) {
  char b[80];
  // Month list: the current one (live) and the stored ones, without repeating the current one
  int nPast = 0;
  const MonthRec *past[MAX_HIST];
  for (int i = e.n_hist - 1; i >= 0; i--)
    if (e.hist[i].key != e.month_key) past[nPast++] = &e.hist[i];
  mCount = 1 + nPast;
  if (mSel >= mCount) mSel = mCount - 1;
  setHidden(mUp, mSel >= mCount - 1);
  setHidden(mDown, mSel == 0);

  MonthRec r{};
  const bool live = mSel == 0;
  if (live) {
    if (!e.month_ok) {
      setText(mTitle, "ESTE MES");
      setText(billNum, "--");
      setText(billL, "Esperando datos del Gateway");
      return;
    }
    r = {e.month_key, e.m_solar, e.m_home, e.m_imp, e.m_exp, ENERGY_RATE, FIXED_CHARGE, e.month_from, SELL_RATE};
  } else {
    r = *past[mSel - 1];
  }
  const int month = r.key % 100, year = r.key / 100;
  if (live) snprintf(b, sizeof(b), "%s", MONTH_UP[(month + 11) % 12]);
  else snprintf(b, sizeof(b), "%s %d", MONTH_UP[(month + 11) % 12], year);
  setText(mTitle, b);
  setColor(mTitle, live ? C_DIM : C_ACCENT);

  const float net = r.imp - r.exp;  // net kWh bought (+) or sold (-)
  const float bill = r.imp * r.rate - r.exp * r.sell + r.fixed;
  snprintf(b, sizeof(b), "%.2f", fabsf(bill));
  setText(billNum, b);
  if (live) setText(billL, bill >= 0 ? withU("a pagar a %s") : withU("de crédito con %s"));
  else setText(billL, bill >= 0 ? "factura estimada" : "crédito estimado");
  setColor(billNum, bill >= 0 ? C_TEXT : C_GREEN);
  setColor(billSign, bill >= 0 ? C_DIM : C_GREEN);
  setColor(billL, bill >= 0 ? C_AMBER : C_GREEN);

  const float v[4] = {r.solar, r.home, r.imp, r.exp};
  const lv_color_t c[4] = {C_SOLAR, C_TEXT, C_TEXT, C_GREEN};
  for (int i = 0; i < 4; i++) {
    if (isfinite(v[i])) snprintf(b, sizeof(b), "%d kWh", (int)lroundf(v[i]));
    setText(mV[i], isfinite(v[i]) ? b : "--");
    setColor(mV[i], c[i]);
  }
  if (fabsf(r.sell - r.rate) > 0.0005f) {  // different price for selling: show both sides
    snprintf(b, sizeof(b), "Compra %d x $%.2f - venta %d x $%.2f", (int)lroundf(r.imp), r.rate, (int)lroundf(r.exp),
             r.sell);
  } else if (net >= 0) {
    snprintf(b, sizeof(b), "Neto: %d kWh de %s x $%.2f", (int)lroundf(net), U, r.rate);
  } else {
    snprintf(b, sizeof(b), "Neto: %d kWh a %s x $%.2f", (int)lroundf(-net), U, r.rate);
  }
  setText(mNetL, b);
  if (r.from > 1) snprintf(b, sizeof(b), "Contando desde el %d de %s", r.from, MONTH[(month + 11) % 12]);
  else snprintf(b, sizeof(b), "+ $%.0f cargo fijo - estimado", r.fixed);
  setText(mFromL, b);
}

void updateVpp(const Energy &e, uint32_t nowMs) {
  char b[80];
  int nPast = 0;
  const VppRec *past[MAX_HIST];
  for (int i = e.n_vhist - 1; i >= 0; i--)
    if (e.vhist[i].key != e.vpp.key) past[nPast++] = &e.vhist[i];
  vCount = 1 + nPast;
  if (vSel >= vCount) vSel = vCount - 1;
  setHidden(vUp, vSel >= vCount - 1);
  setHidden(vDown, vSel == 0);
  const bool live = vSel == 0;
  const VppRec &r = live ? e.vpp : *past[vSel - 1];
  if (!r.key) {
    setText(vTitle, "VPP");
    setText(vNum, "--");
    return;
  }
  const int month = r.key % 100;
  if (live) snprintf(b, sizeof(b), "VPP - %s", MONTH_UP[(month + 11) % 12]);
  else snprintf(b, sizeof(b), "VPP - %s %lu", MONTH_UP[(month + 11) % 12], (unsigned long)(r.key / 100));
  setText(vTitle, b);
  setColor(vTitle, live ? C_DIM : C_ACCENT);
  snprintf(b, sizeof(b), "%.2f", r.kwh * VPP_RATE);
  setText(vNum, b);
  snprintf(b, sizeof(b), "%u", r.events);
  setText(vV[0], b);
  snprintf(b, sizeof(b), "%.1f kWh", r.kwh);
  setText(vV[1], b);

  // Live: event now, or the last event
  if (live && e.vpp_active && e.at && nowMs - e.at < 30000) {
    char kw[16];
    fmtKw(kw, sizeof(kw), e.vpp_w);
    snprintf(b, sizeof(b), "EVENTO AHORA - %s - %.1f kWh", kw, e.vpp_event_kwh);
    setText(vNowL, b);
    setColor(vNowL, (nowMs / 600) % 2 ? C_GREEN : C_TEXT);
  } else if (r.last_at) {
    struct tm tm;
    localtime_r(&r.last_at, &tm);
    snprintf(b, sizeof(b), "Último: %d %s - %.1f kWh", tm.tm_mday, MON3[tm.tm_mon], r.last_kwh);
    setText(vNowL, b);
    setColor(vNowL, C_DIM);
  } else {
    setText(vNowL, r.events ? "" : "Sin eventos este mes");
    setColor(vNowL, C_DIM);
  }
  snprintf(b, sizeof(b), "$%.2f por kWh enviado", VPP_RATE);
  setText(vRateL, b);
}

void updateWeather(const Weather &w, uint32_t nowMs) {
  char b[80];
  const bool fcFresh = w.fc_at && nowMs - w.fc_at < 60 * 60000UL;
  const bool alertsFresh = w.alerts_at && nowMs - w.alerts_at < 15 * 60000UL;

  if (!w.alerts_at) snprintf(b, sizeof(b), "%s", g_place);
  else if (!alertsFresh) snprintf(b, sizeof(b), "%s - sin internet (%lu min)", g_place, (nowMs - w.alerts_at) / 60000UL);
  else snprintf(b, sizeof(b), "%s", g_place);
  setText(wxTitle, b);
  setColor(wxTitle, alertsFresh || !w.alerts_at ? C_DIM : C_AMBER);

  if (fcFresh && isfinite(w.temp)) snprintf(b, sizeof(b), "%d", (int)lroundf(w.temp));
  setText(tempNum, fcFresh && isfinite(w.temp) ? b : "--");
  setText(condL, fcFresh ? sky(w.code) : "");
  b[0] = 0;
  if (fcFresh && isfinite(w.tmax) && isfinite(w.tmin)) {
    if (w.rain >= 0) snprintf(b, sizeof(b), "%d° / %d° - lluvia %d%%", (int)lroundf(w.tmax), (int)lroundf(w.tmin), w.rain);
    else snprintf(b, sizeof(b), "%d° / %d°", (int)lroundf(w.tmax), (int)lroundf(w.tmin));
  }
  setText(rangeL, b);

  // Alerts (the two most severe)
  for (int i = 0; i < 2; i++) {
    const bool has = i < w.n_alerts;
    setHidden(alertName[i], !has);
    setHidden(alertWhen[i], !has);
    if (!has) continue;
    setText(alertName[i], eventEs(w.alerts[i].event));
    setColor(alertName[i], alertColor(w.alerts[i]));
    fmtUntil(b, sizeof(b), w.alerts[i].ends);
    setText(alertWhen[i], b);
  }
  if (!w.alerts_at) setText(moreL, "Buscando avisos...");
  else if (w.n_alerts == 0) setText(moreL, "Sin avisos del NWS");
  else if (w.n_alerts > 2) {
    snprintf(b, sizeof(b), "y %d más", w.n_alerts - 2);
    setText(moreL, b);
  } else setText(moreL, "");
  setColor(moreL, w.n_alerts == 0 && w.alerts_at ? C_GREEN : C_DIM);

  // Closest storm (NHC)
  if (!w.storms_at) {
    setText(stormL1, "");
    setText(stormL2, "");
  } else if (!w.storm.valid) {
    setText(stormL1, "Sin tormentas activas");
    setText(stormL2, "");
    setColor(stormL1, C_DIM);
  } else {
    stormName(b, sizeof(b), w.storm);
    setText(stormL1, b);
    setColor(stormL1, w.storm.dist_mi < 300 ? C_RED : w.storm.dist_mi < 800 ? C_AMBER : C_TEXT);
    snprintf(b, sizeof(b), "%d mph - a %d mi al %s", w.storm.mph, w.storm.dist_mi, w.storm.bearing);
    setText(stormL2, b);
  }
}

}  // namespace

bool important_alert(const Alert &a) {
  if (!strcmp(a.severity, "Extreme")) return true;
  for (const char *k : {"Hurricane Warning", "Tornado Warning", "Tsunami Warning", "Flash Flood Warning",
                        "Storm Surge Warning", "Extreme Wind Warning"})
    if (!strcmp(a.event, k)) return true;
  return false;
}

void build() {
  initNames();
  g_pages = VPP_ENABLED ? 4 : 3;
  lv_obj_t *scr = lv_screen_active();
  lv_obj_set_style_bg_color(scr, C_BG, 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  tv = lv_tileview_create(scr);
  lv_obj_set_style_bg_color(tv, C_BG, 0);
  lv_obj_set_style_bg_opa(tv, LV_OPA_COVER, 0);
  lv_obj_set_scrollbar_mode(tv, LV_SCROLLBAR_MODE_OFF);
  tileE = lv_tileview_add_tile(tv, 0, 0, (lv_dir_t)(LV_DIR_RIGHT | LV_DIR_BOTTOM));
  tileF = lv_tileview_add_tile(tv, 0, 1, LV_DIR_TOP);  // below energy: swipe up
  tileM = lv_tileview_add_tile(tv, 1, 0, (lv_dir_t)(LV_DIR_LEFT | LV_DIR_RIGHT));
#if VPP_ENABLED
  tileV = lv_tileview_add_tile(tv, 2, 0, (lv_dir_t)(LV_DIR_LEFT | LV_DIR_RIGHT));
  tileW = lv_tileview_add_tile(tv, 3, 0, LV_DIR_LEFT);
#else
  tileV = nullptr;
  tileW = lv_tileview_add_tile(tv, 2, 0, LV_DIR_LEFT);
#endif
  lv_obj_add_event_cb(tv, [](lv_event_t *) {
    if (lv_tileview_get_tile_active(tv) != tileM) mSel = 0;  // when returning, start at the current month
    if (lv_tileview_get_tile_active(tv) != tileV) vSel = 0;
  }, LV_EVENT_VALUE_CHANGED, nullptr);
  buildEnergy(tileE);
  buildFlow(tileF);
  buildMonth(tileM);
  if (tileV) buildVpp(tileV);
  buildWeather(tileW);
}

bool update(const Energy &e, const Weather &w, uint32_t nowMs) {
  updateEnergy(e, w, nowMs);
  updateFlow(e, nowMs);
  updateMonth(e);
  if (tileV) updateVpp(e, nowMs);
  updateWeather(w, nowMs);
  bool important = e.at && nowMs - e.at < 30000 && e.grid == 0;
  for (int i = 0; i < w.n_alerts; i++) important |= important_alert(w.alerts[i]);
  return important;
}

void shift(int dx, int dy) {
  lv_obj_set_style_translate_x(tv, dx, 0);
  lv_obj_set_style_translate_y(tv, dy, 0);
}

void set_place(const char *name) { g_place = name; }

// 0 energy, 1 "Ahora", 2 month, 3 VPP (if enabled), last = weather. No animation (for screenshots).
void show_page(int n) {
  lv_obj_t *t[] = {tileE, tileF, tileM, tileV ? tileV : tileW, tileW};
  if (n >= 0 && n < 5) lv_tileview_set_tile(tv, t[n], LV_ANIM_OFF);
}

void next_page() {
  lv_obj_t *cur = lv_tileview_get_tile_active(tv);
  lv_tileview_set_tile(tv, cur == tileE || cur == tileF ? tileM : cur == tileM ? (tileV ? tileV : tileW) : cur == tileV ? tileW : tileE, LV_ANIM_ON);
}

}  // namespace ui

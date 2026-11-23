#include "net.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <math.h>

#include <algorithm>
#include <atomic>

#include "config.h"
#include "log.h"
#include "model.h"
#include "secrets.h"

namespace net {
namespace {

// ---------------------------------------------------------------------------------------------------------------
// Powerwall Gateway: local HTTPS API (Gateway's self-signed certificate -> not verified)
// ---------------------------------------------------------------------------------------------------------------
const char *const GW[] = GW_HOSTS;
constexpr int GW_N = sizeof(GW) / sizeof(GW[0]);

WiFiClientSecure gwTls;
HTTPClient gwHttp;
int gwIdx = 0;
String gwToken;
uint32_t gwNextLoginAt = 0, gwLoginFails = 0;
uint32_t gwLastPoll = 0, gwLastStatus = 0, gwLastAvg = 0;
uint32_t gwFails = 0;   // consecutive reads that failed due to network
float fullWh = NAN, remainWh = NAN;
Energy E;

// ---- Month totals: Gateway lifetime counters (Wh) and their value at the start of the month (saved in NVS)
enum { C_SOLAR, C_HOME, C_IMP, C_EXP, C_N };
const char *const BASE_KEYS[C_N] = {"solar", "home", "imp", "exp"};
Preferences monthPrefs;
double baseWh[C_N] = {};
uint32_t baseMonth = 0;  // YYYYMM of the saved baseline (0 = none)
uint8_t baseFrom = 1;
std::atomic<bool> seedReq{false};
float seedKwh[C_N];

void loadBase() {
  monthPrefs.begin("month", false);
  baseMonth = monthPrefs.getULong("key", 0);
  baseFrom = monthPrefs.getUChar("from", 1);
  for (int i = 0; i < C_N; i++) baseWh[i] = monthPrefs.getDouble(BASE_KEYS[i], 0);
}
void saveBase() {
  monthPrefs.putULong("key", baseMonth);
  monthPrefs.putUChar("from", baseFrom);
  for (int i = 0; i < C_N; i++) monthPrefs.putDouble(BASE_KEYS[i], baseWh[i]);
}

// ---- History: one record per month in NVS ("hist", key "hYYYYMM"). Loaded at boot.
Preferences histPrefs;
QueueHandle_t histQ = nullptr;  // records arriving over USB (written by this task)

void histKey(char *k, uint32_t key) { snprintf(k, 12, "h%lu", (unsigned long)key); }

void addToModel(const MonthRec &r) {
  // replace if present; otherwise insert in order (oldest to newest), keeping the last MAX_HIST
  for (int i = 0; i < E.n_hist; i++)
    if (E.hist[i].key == r.key) {
      E.hist[i] = r;
      return;
    }
  int pos = E.n_hist;
  while (pos > 0 && E.hist[pos - 1].key > r.key) pos--;
  if (E.n_hist == MAX_HIST) {
    if (pos == 0) return;  // older than everything stored
    memmove(&E.hist[0], &E.hist[1], sizeof(MonthRec) * (pos - 1));
    pos--;
  } else {
    memmove(&E.hist[pos + 1], &E.hist[pos], sizeof(MonthRec) * (E.n_hist - pos));
    E.n_hist++;
  }
  E.hist[pos] = r;
}

void storeHist(const MonthRec &r) {
  char k[12];
  histKey(k, r.key);
  histPrefs.putBytes(k, &r, sizeof(r));
  addToModel(r);
  LOGF("[hist] %lu: solar %.0f home %.0f import %.0f export %.0f at $%.4f + $%.0f\n", (unsigned long)r.key, r.solar, r.home,
       r.imp, r.exp, r.rate, r.fixed);
}

void loadHist() {
  histPrefs.begin("hist", false);
  // Preferences can't list keys: try every month from 2024 to 2035 (144 reads, once at boot)
  for (int y = 2024; y <= 2035; y++)
    for (int m = 1; m <= 12; m++) {
      char k[12];
      const uint32_t key = y * 100 + m;
      histKey(k, key);
      MonthRec r;
      if (!histPrefs.isKey(k)) continue;
      // Records saved before the "sell" field existed are shorter: same price for selling as for buying
      const size_t len = histPrefs.getBytesLength(k);
      if (len == sizeof(r) && histPrefs.getBytes(k, &r, sizeof(r)) == sizeof(r)) addToModel(r);
      else if (len == offsetof(MonthRec, sell) && histPrefs.getBytes(k, &r, sizeof(r)) == len) {
        r.sell = r.rate;
        addToModel(r);
      }
    }
  LOGF("[hist] %d months stored\n", E.n_hist);
}

// ---- VPP: detect events from the power the battery sends to the grid; save per month in NVS ("vpp")
Preferences vppPrefs;
QueueHandle_t vppQ = nullptr;
uint32_t vppStartAt = 0, vppLowAt = 0, vppLastSave = 0, vppLastT = 0;
float vppPreKwh = 0;
bool gsWas = false;

void vppAddHist(const VppRec &r) {
  for (int i = 0; i < E.n_vhist; i++)
    if (E.vhist[i].key == r.key) {
      E.vhist[i] = r;
      return;
    }
  int pos = E.n_vhist;
  while (pos > 0 && E.vhist[pos - 1].key > r.key) pos--;
  if (E.n_vhist == MAX_HIST) {
    if (pos == 0) return;
    memmove(&E.vhist[0], &E.vhist[1], sizeof(VppRec) * (pos - 1));
    pos--;
  } else {
    memmove(&E.vhist[pos + 1], &E.vhist[pos], sizeof(VppRec) * (E.n_vhist - pos));
    E.n_vhist++;
  }
  E.vhist[pos] = r;
}

void vppSaveCur() { vppPrefs.putBytes("cur", &E.vpp, sizeof(E.vpp)); }

void vppStore(const VppRec &r) {
  // Is it the current month? (if there is no Gateway data yet, decide by the clock)
  uint32_t cur = E.vpp.key;
  time_t t;
  time(&t);
  if (!cur && t > 1700000000) {
    struct tm tm;
    localtime_r(&t, &tm);
    cur = (tm.tm_year + 1900) * 100 + tm.tm_mon + 1;
  }
  if (r.key == cur) {
    E.vpp = r;
    vppSaveCur();
  } else {
    char k[12];
    snprintf(k, sizeof(k), "v%lu", (unsigned long)r.key);
    vppPrefs.putBytes(k, &r, sizeof(r));
    vppAddHist(r);
  }
  LOGF("[vpp] %lu: %u events, %.1f kWh\n", (unsigned long)r.key, r.events, r.kwh);
}

void vppLoad() {
  vppPrefs.begin("vpp", false);
  if (vppPrefs.isKey("cur")) vppPrefs.getBytes("cur", &E.vpp, sizeof(E.vpp));
  for (int y = 2024; y <= 2035; y++)
    for (int m = 1; m <= 12; m++) {
      char k[12];
      snprintf(k, sizeof(k), "v%d", y * 100 + m);
      VppRec r;
      if (vppPrefs.isKey(k) && vppPrefs.getBytes(k, &r, sizeof(r)) == sizeof(r)) vppAddHist(r);
    }
  LOGF("[vpp] month %lu: %u events; %d months stored\n", (unsigned long)E.vpp.key, E.vpp.events, E.n_vhist);
}

// Current month: if it changed, the previous month moves to history and counting restarts at zero
void vppMonth(uint32_t key) {
  if (E.vpp.key == key) return;
  if (E.vpp.key) {
    char k[12];
    snprintf(k, sizeof(k), "v%lu", (unsigned long)E.vpp.key);
    vppPrefs.putBytes(k, &E.vpp, sizeof(E.vpp));
    vppAddHist(E.vpp);
  }
  E.vpp = VppRec{key, 0, 0, 0, 0};
  vppSaveCur();
}

void vppDetect(uint32_t now) {
  // What leaves the battery and the house doesn't use (after solar) goes to the grid
  float b2g = 0;
  if (isfinite(E.battery_w) && isfinite(E.home_w) && isfinite(E.solar_w) && E.grid == 1 && E.battery_w > 0)
    b2g = max(0.0f, E.battery_w - max(0.0f, E.home_w - E.solar_w));
  E.vpp_w = b2g;
  const float dt = vppLastT ? (now - vppLastT) / 1000.0f : 0;
  vppLastT = now;
  const float kwh = b2g * dt / 3.6e6f;
  time_t t;
  time(&t);
  if (!E.vpp_active) {
    if (b2g > VPP_START_W) {
      if (!vppStartAt) {
        vppStartAt = now;
        vppPreKwh = 0;
      }
      vppPreKwh += kwh;
      if (now - vppStartAt >= VPP_START_MS && E.vpp.key) {
        E.vpp_active = true;
        E.vpp_event_kwh = vppPreKwh;
        E.vpp.events++;
        E.vpp.kwh += vppPreKwh;
        E.vpp.last_at = t - VPP_START_MS / 1000;
        E.vpp.last_kwh = E.vpp_event_kwh;
        vppLowAt = 0;
        vppSaveCur();
        vppLastSave = now;
        LOGF("[vpp] EVENT started (%.0f W, grid_services_active=%d)\n", b2g, E.gs_active);
      }
    } else {
      vppStartAt = 0;
    }
    return;
  }
  E.vpp_event_kwh += kwh;
  E.vpp.kwh += kwh;
  E.vpp.last_kwh = E.vpp_event_kwh;
  if (b2g < VPP_END_W) {
    if (!vppLowAt) vppLowAt = now;
    if (now - vppLowAt >= VPP_END_MS) {
      E.vpp_active = false;
      vppStartAt = 0;
      vppSaveCur();
      LOGF("[vpp] event ended: %.2f kWh\n", E.vpp_event_kwh);
    }
  } else {
    vppLowAt = 0;
  }
  if (E.vpp_active && now - vppLastSave > 300000) {  // save every 5 min during the event
    vppLastSave = now;
    vppSaveCur();
  }
}

void updateMonth(const double cnt[C_N]) {
  for (int i = 0; i < C_N; i++)
    if (!(cnt[i] > 0)) return;  // a counter is missing
  time_t t;
  time(&t);
  if (t < 1700000000) return;  // without the time we don't know which month it is
  struct tm tm;
  localtime_r(&t, &tm);
  const uint32_t key = (tm.tm_year + 1900) * 100 + tm.tm_mon + 1;
  bool reset = baseMonth != key;
  for (int i = 0; i < C_N; i++) reset |= cnt[i] < baseWh[i];  // counter reset (Gateway replaced)
  if (reset) {
    // New month: if the old baseline was from the previous month, the month starts complete; otherwise from today
    const uint32_t prev = tm.tm_mon == 0 ? (tm.tm_year + 1899) * 100 + 12 : key - 1;
    // Close the finished month and save it to history (with the current prices)
    bool counterOk = true;
    for (int i = 0; i < C_N; i++) counterOk &= cnt[i] >= baseWh[i];
    if (baseMonth == prev && counterOk) {
      MonthRec r{};
      r.key = baseMonth;
      r.solar = (cnt[C_SOLAR] - baseWh[C_SOLAR]) / 1000.0;
      r.home = (cnt[C_HOME] - baseWh[C_HOME]) / 1000.0;
      r.imp = (cnt[C_IMP] - baseWh[C_IMP]) / 1000.0;
      r.exp = (cnt[C_EXP] - baseWh[C_EXP]) / 1000.0;
      r.rate = ENERGY_RATE;
      r.fixed = FIXED_CHARGE;
      r.sell = SELL_RATE;
      r.from = baseFrom;
      storeHist(r);
    }
    baseFrom = (baseMonth == prev && baseMonth) ? 1 : tm.tm_mday;
    baseMonth = key;
    for (int i = 0; i < C_N; i++) baseWh[i] = cnt[i];
    saveBase();
    LOGF("[month] new baseline %lu from day %d\n", (unsigned long)key, baseFrom);
  }
  if (seedReq.exchange(false)) {
    for (int i = 0; i < C_N; i++) baseWh[i] = cnt[i] - seedKwh[i] * 1000.0;
    baseFrom = 1;
    saveBase();
    LOGF("[month] baseline adjusted: full month\n");
  }
  vppMonth(key);
  E.month_ok = true;
  E.month_key = key;
  E.month = tm.tm_mon + 1;
  E.month_from = baseFrom;
  E.m_solar = (cnt[C_SOLAR] - baseWh[C_SOLAR]) / 1000.0;
  E.m_home = (cnt[C_HOME] - baseWh[C_HOME]) / 1000.0;
  E.m_imp = (cnt[C_IMP] - baseWh[C_IMP]) / 1000.0;
  E.m_exp = (cnt[C_EXP] - baseWh[C_EXP]) / 1000.0;
}

void gwBegin(const char *path) {
  gwHttp.setReuse(true);  // keep the TLS connection open (the TLS handshake takes ~1 s)
  gwHttp.setConnectTimeout(3000);
  gwHttp.setTimeout(4000);
  gwHttp.begin(gwTls, String("https://") + GW[gwIdx] + path);
}

int gwGet(const char *path, String &out) {
  gwBegin(path);
  if (gwToken.length()) {
    gwHttp.addHeader("Authorization", "Bearer " + gwToken);
    gwHttp.addHeader("Cookie", "AuthCookie=" + gwToken);
  }
  const int code = gwHttp.GET();
  if (code > 0) out = gwHttp.getString();  // always read it (otherwise the connection can't be reused)
  gwHttp.end();
  return code;
}

// "customer" login. If the password is wrong, don't keep trying: the Gateway locks out after several attempts.
bool gwLogin() {
  JsonDocument d;
  d["username"] = "customer";
  d["password"] = GW_PASSWORD;
  d["email"] = GW_EMAIL;
  d["force_sm_off"] = false;
  String body, resp;
  serializeJson(d, body);
  gwBegin("/api/login/Basic");
  gwHttp.addHeader("Content-Type", "application/json");
  const int code = gwHttp.POST(body);
  if (code > 0) resp = gwHttp.getString();
  gwHttp.end();
  if (code == 200) {
    JsonDocument r;
    if (deserializeJson(r, resp) == DeserializationError::Ok && r["token"].is<const char *>()) {
      gwToken = r["token"].as<const char *>();
      gwLoginFails = 0;
      LOGF("[gw] login ok (%s)\n", GW[gwIdx]);
      return true;
    }
  }
  gwToken = "";
  gwLoginFails++;
  // Wait before retrying: 1, 2, 4... up to 30 min. Wrong password or locked out: at least 10 min.
  uint32_t wait = min<uint32_t>(60000UL << min<uint32_t>(gwLoginFails - 1, 5), 30 * 60000UL);
  if (code == 401 || code == 403) {
    E.status = GwStatus::BAD_PASSWORD;
    wait = max<uint32_t>(wait, 10 * 60000UL);
  } else if (code == 429) {
    E.status = GwStatus::LOCKED;
    wait = max<uint32_t>(wait, 10 * 60000UL);
  } else {
    E.status = GwStatus::UNREACHABLE;
    wait = 15000;  // no response (not a failed attempt for the Gateway): try the other IP soon
    gwLoginFails = 0;
    gwIdx = (gwIdx + 1) % GW_N;
  }
  gwNextLoginAt = millis() + wait;
  LOGF("[gw] login failed: %d (%s), retry in %lu s\n", code, GW[gwIdx], (unsigned long)(wait / 1000));
  return false;
}

// A failed request: no response -> count and switch IP; 401/403 -> session expired, log in again
bool gwCheck(int code) {
  if (code == 200) return true;
  if (code == 401 || code == 403) {
    LOGF("[gw] session expired (%d)\n", code);
    gwToken = "";
    gwNextLoginAt = 0;
  } else if (code <= 0) {
    if (++gwFails >= 3) {
      gwFails = 0;
      gwIdx = (gwIdx + 1) % GW_N;
      gwTls.stop();
      E.status = GwStatus::UNREACHABLE;
      LOGF("[gw] no response, trying %s\n", GW[gwIdx]);
    }
  }
  return false;
}

// Exponential moving average with time constant tau (s)
void ema(float &avg, float v, float dt, float tau) {
  if (!isfinite(v)) return;
  if (!isfinite(avg)) avg = v;
  else avg += (v - avg) * (1.0f - expf(-dt / tau));
}

void gwPoll() {
  const uint32_t now = millis();
  if (!gwToken.length()) {
    if ((int32_t)(now - gwNextLoginAt) < 0) return;
    if (E.status != GwStatus::BAD_PASSWORD && E.status != GwStatus::LOCKED) E.status = GwStatus::LOGIN;
    model::put(E);
    if (!gwLogin()) {
      model::put(E);
      return;
    }
  }
  if (now - gwLastPoll < GW_POLL_MS) return;
  gwLastPoll = now;

  String s;
  int code = gwGet("/api/meters/aggregates", s);
  if (!gwCheck(code)) return;
  {
    JsonDocument f;
    for (const char *k : {"site", "battery", "load", "solar"}) {
      f[k]["instant_power"] = true;
      f[k]["energy_imported"] = true;
      f[k]["energy_exported"] = true;
    }
    JsonDocument d;
    if (deserializeJson(d, s, DeserializationOption::Filter(f)) != DeserializationError::Ok) return;
    E.grid_w = d["site"]["instant_power"] | NAN;
    E.battery_w = d["battery"]["instant_power"] | NAN;
    E.home_w = d["load"]["instant_power"] | NAN;
    E.solar_w = d["solar"]["instant_power"] | NAN;
    // Lifetime counters (Wh): solar production, home consumption (including the car), grid import and export
    const double cnt[C_N] = {d["solar"]["energy_exported"] | 0.0, d["load"]["energy_imported"] | 0.0,
                             d["site"]["energy_imported"] | 0.0, d["site"]["energy_exported"] | 0.0};
    updateMonth(cnt);
  }

  code = gwGet("/api/system_status/soe", s);
  if (!gwCheck(code)) return;
  {
    JsonDocument d;
    if (deserializeJson(d, s) == DeserializationError::Ok && d["percentage"].is<float>()) {
      // The Gateway reserves 5% that the app doesn't show: app = (gateway - 5) / 0.95
      const float gwPct = d["percentage"].as<float>();
      E.pct = constrain((gwPct - 5.0f) / 0.95f, 0.0f, 100.0f);
    }
  }

  code = gwGet("/api/system_status/grid_status", s);
  if (!gwCheck(code)) return;
  {
    JsonDocument d;
    if (deserializeJson(d, s) == DeserializationError::Ok) {
      const char *g = d["grid_status"] | "";
      // SystemGridConnected = on grid; SystemIslanded* / TransitionToIsland / MicroGridFaulted = outage
      E.gs_active = d["grid_services_active"] | false;
      if (E.gs_active != gsWas) {
        gsWas = E.gs_active;
        LOGF("[vpp] grid_services_active=%d (battery %.0f W, grid %.0f W)\n", gsWas, E.battery_w, E.grid_w);
      }
      if (!strcmp(g, "SystemGridConnected") || !strcmp(g, "SystemTransitionToGrid")) E.grid = 1;
      else if (strstr(g, "Island") || strstr(g, "MicroGrid")) E.grid = 0;
      else E.grid = -1;
    }
  }

  // Actual pack energy (changes as the batteries age), once per minute
  if (!gwLastStatus || now - gwLastStatus >= GW_STATUS_MS) {
    gwLastStatus = now;
    if (gwGet("/api/system_status", s) == 200) {
      JsonDocument f;
      f["nominal_full_pack_energy"] = true;
      f["nominal_energy_remaining"] = true;
      JsonDocument d;
      if (deserializeJson(d, s, DeserializationOption::Filter(f)) == DeserializationError::Ok) {
        fullWh = d["nominal_full_pack_energy"] | NAN;
        remainWh = d["nominal_energy_remaining"] | NAN;
      }
    }
  }
  // Pack energy is read every minute; between reads it is adjusted using battery power
  if (isfinite(remainWh) && isfinite(E.battery_w) && gwLastAvg) remainWh -= E.battery_w * (now - gwLastAvg) / 3.6e6f;
  // Usable = remaining minus the hidden 5%. Without that data: nominal kWh x app %.
  if (isfinite(fullWh) && isfinite(remainWh) && fullWh > 1000) E.usable_wh = max(0.0f, remainWh - 0.05f * fullWh);
  else if (isfinite(E.pct)) E.usable_wh = BATTERY_KWH * 1000.0f * E.pct / 100.0f;

  const float dt = gwLastAvg ? (now - gwLastAvg) / 1000.0f : 0;
  gwLastAvg = now;
  ema(E.home_avg_w, E.home_w, dt, 600);
  if (isfinite(E.home_w) && isfinite(E.solar_w)) ema(E.net_avg_w, E.home_w - E.solar_w, dt, 600);

#if VPP_ENABLED
  vppDetect(now);
#endif
  gwFails = 0;
  E.status = GwStatus::OK;
  E.at = now;
  model::put(E);
}

// ---------------------------------------------------------------------------------------------------------------
// Weather: NWS (alerts), Open-Meteo (forecast), NHC (storms). Public data; certificate not verified.
// ---------------------------------------------------------------------------------------------------------------
WiFiClientSecure wxTls;
Weather W;
uint32_t wxAlertsAt = 0, wxFcAt = 0, wxStormsAt = 0;
bool wxAlertsTried = false, wxFcTried = false, wxStormsTried = false;

// ISO 8601 date with zone ("2026-11-21T17:45:00-04:00") -> UTC seconds
time_t parseIso(const char *s) {
  int Y, M, D, h, m, sec;
  if (!s || sscanf(s, "%d-%d-%dT%d:%d:%d", &Y, &M, &D, &h, &m, &sec) != 6) return 0;
  long off = 0;
  const char *tz = s + 19;
  while (*tz && *tz != '+' && *tz != '-' && *tz != 'Z') tz++;
  if (*tz == '+' || *tz == '-') {
    int oh = 0, om = 0;
    sscanf(tz + 1, "%d:%d", &oh, &om);
    off = (oh * 3600L + om * 60L) * (*tz == '-' ? -1 : 1);
  }
  // days since 1970 (H. Hinnant's days_from_civil algorithm)
  Y -= M <= 2;
  const long era = (Y >= 0 ? Y : Y - 399) / 400;
  const unsigned yoe = (unsigned)(Y - era * 400);
  const unsigned doy = (153 * (M + (M > 2 ? -3 : 9)) + 2) / 5 + D - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  const long days = era * 146097 + (long)doe - 719468;
  return (time_t)(days * 86400L + h * 3600L + m * 60L + sec - off);
}

uint32_t fnv(const char *s) {
  uint32_t h = 2166136261u;
  while (s && *s) h = (h ^ (uint8_t)*s++) * 16777619u;
  return h;
}

int sevRank(const char *sev, const char *event) {
  int r = !strcmp(sev, "Extreme") ? 40 : !strcmp(sev, "Severe") ? 30 : !strcmp(sev, "Moderate") ? 20 : 10;
  if (strstr(event, "Warning")) r += 3;
  else if (strstr(event, "Watch")) r += 2;
  return r;
}

bool wxBegin(HTTPClient &http, const String &url) {
  http.setConnectTimeout(5000);
  http.setTimeout(8000);
  http.setUserAgent("home-energy-display/0.1 (ESP32)");  // the NWS requires a User-Agent
  return http.begin(wxTls, url);
}

bool fetchAlerts() {
  HTTPClient http;
  http.useHTTP10(true);  // no "chunked": the JSON can be read straight from the stream (may be large)
  char url[128];
  snprintf(url, sizeof(url), "https://api.weather.gov/alerts/active?point=%.4f,%.4f", HOME_LAT, HOME_LON);
  if (!wxBegin(http, url)) return false;
  http.addHeader("Accept", "application/geo+json");
  const int code = http.GET();
  if (code != 200) {
    LOGF("[wx] NWS %d\n", code);
    http.end();
    return false;
  }
  JsonDocument f;
  JsonObject p = f["features"][0]["properties"].to<JsonObject>();
  for (const char *k : {"id", "event", "severity", "ends", "expires", "messageType"}) p[k] = true;
  JsonDocument d;
  const DeserializationError err = deserializeJson(d, http.getStream(), DeserializationOption::Filter(f));
  http.end();
  if (err) {
    LOGF("[wx] NWS json: %s\n", err.c_str());
    return false;
  }
  time_t now;
  time(&now);
  Alert list[12];
  int n = 0;
  for (JsonObject ft : d["features"].as<JsonArray>()) {
    JsonObject pr = ft["properties"];
    const char *ev = pr["event"] | "";
    if (!*ev || !strcmp(pr["messageType"] | "", "Cancel")) continue;
    time_t ends = parseIso(pr["ends"].as<const char *>());
    if (!ends) ends = parseIso(pr["expires"].as<const char *>());
    if (ends && now > 1700000000 && ends < now) continue;  // already ended
    // NWS San Juan publishes each alert in English and Spanish: same event and same time = one alert
    bool dup = false;
    for (int i = 0; i < n; i++) dup |= !strcmp(list[i].event, ev) && list[i].ends == ends;
    if (dup || n >= 12) continue;
    Alert &a = list[n++];
    strlcpy(a.event, ev, sizeof(a.event));
    strlcpy(a.severity, pr["severity"] | "Unknown", sizeof(a.severity));
    a.ends = ends;
    char key[80];
    snprintf(key, sizeof(key), "%s|%ld", ev, (long)ends);
    a.hash = fnv(key);
  }
  // Most severe first
  std::sort(list, list + n, [](const Alert &a, const Alert &b) {
    return sevRank(a.severity, a.event) > sevRank(b.severity, b.event);
  });
  W.n_alerts = min(n, MAX_ALERTS);
  for (int i = 0; i < W.n_alerts; i++) W.alerts[i] = list[i];
  W.alerts_at = millis();
  LOGF("[wx] %d active alerts\n", n);
  return true;
}

bool fetchForecast() {
  HTTPClient http;
  char url[300];
  snprintf(url, sizeof(url),
           "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
           "&current=temperature_2m,weather_code&daily=temperature_2m_max,temperature_2m_min,"
           "precipitation_probability_max&temperature_unit=fahrenheit&timezone=America%%2FPuerto_Rico&forecast_days=1",
           HOME_LAT, HOME_LON);
  if (!wxBegin(http, url)) return false;
  const int code = http.GET();
  String s = code == 200 ? http.getString() : String();
  http.end();
  if (code != 200) {
    LOGF("[wx] Open-Meteo %d\n", code);
    return false;
  }
  JsonDocument d;
  if (deserializeJson(d, s)) return false;
  W.temp = d["current"]["temperature_2m"] | NAN;
  W.code = d["current"]["weather_code"] | -1;
  W.tmax = d["daily"]["temperature_2m_max"][0] | NAN;
  W.tmin = d["daily"]["temperature_2m_min"][0] | NAN;
  W.rain = d["daily"]["precipitation_probability_max"][0] | -1;
  W.fc_at = millis();
  return true;
}

const char *compass(float deg) {
  static const char *const D[] = {"N", "NE", "E", "SE", "S", "SO", "O", "NO"};
  return D[(int)((fmodf(deg + 360.0f, 360.0f) + 22.5f) / 45.0f) % 8];
}

float numOrStr(JsonVariantConst v) {
  if (v.is<const char *>()) return atof(v.as<const char *>());
  return v | NAN;
}

bool fetchStorms() {
  HTTPClient http;
  if (!wxBegin(http, "https://www.nhc.noaa.gov/CurrentStorms.json")) return false;
  const int code = http.GET();
  String s = code == 200 ? http.getString() : String();
  http.end();
  if (code != 200) {
    LOGF("[wx] NHC %d\n", code);
    return false;
  }
  JsonDocument d;
  if (deserializeJson(d, s)) return false;
  W.n_storms = 0;
  W.storm.valid = false;
  float best = 1e9f;
  const float la1 = HOME_LAT * DEG_TO_RAD, lo1 = HOME_LON * DEG_TO_RAD;
  for (JsonObject st : d["activeStorms"].as<JsonArray>()) {
    const float lat = numOrStr(st["latitudeNumeric"]), lon = numOrStr(st["longitudeNumeric"]);
    if (!isfinite(lat) || !isfinite(lon)) continue;
    W.n_storms++;
    const float la2 = lat * DEG_TO_RAD, lo2 = lon * DEG_TO_RAD;
    const float a = sinf((la2 - la1) / 2) * sinf((la2 - la1) / 2) +
                    cosf(la1) * cosf(la2) * sinf((lo2 - lo1) / 2) * sinf((lo2 - lo1) / 2);
    const float mi = 3958.8f * 2 * atan2f(sqrtf(a), sqrtf(1 - a));
    if (mi >= best) continue;
    best = mi;
    const float brg = atan2f(sinf(lo2 - lo1) * cosf(la2), cosf(la1) * sinf(la2) - sinf(la1) * cosf(la2) * cosf(lo2 - lo1));
    Storm &s2 = W.storm;
    s2.valid = true;
    strlcpy(s2.name, st["name"] | "?", sizeof(s2.name));
    strlcpy(s2.cls, st["classification"] | "", sizeof(s2.cls));
    const float kt = numOrStr(st["intensity"]);
    s2.mph = isfinite(kt) ? (int)lroundf(kt * 1.15078f) : 0;
    s2.dist_mi = (int)lroundf(mi);
    strlcpy(s2.bearing, compass(brg * RAD_TO_DEG), sizeof(s2.bearing));
  }
  W.storms_at = millis();
  LOGF("[wx] %d active storms\n", W.n_storms);
  return true;
}

// One thing per pass (each HTTPS request takes 1-3 s; this way the Gateway doesn't wait long)
void wxPoll() {
  const uint32_t now = millis();
  time_t t;
  time(&t);
  if (t < 1700000000) return;  // no time yet: alerts depend on it
  auto due = [&](uint32_t last, bool tried, uint32_t every) { return !tried || now - last >= every; };
  if (due(wxAlertsAt, wxAlertsTried, WX_ALERTS_MS)) {
    wxAlertsTried = true;
    wxAlertsAt = fetchAlerts() ? now : now - WX_ALERTS_MS + 60000UL;
  } else if (due(wxFcAt, wxFcTried, WX_FORECAST_MS)) {
    wxFcTried = true;
    wxFcAt = fetchForecast() ? now : now - WX_FORECAST_MS + 60000UL;
  } else if (due(wxStormsAt, wxStormsTried, WX_STORMS_MS)) {
    wxStormsTried = true;
    wxStormsAt = fetchStorms() ? now : now - WX_STORMS_MS + 60000UL;
  } else {
    return;
  }
  model::put(W);  // if it failed, retry in 1 min (the timestamp is shifted for that)
}

void task(void *) {
  if (!*WIFI_SSID || !*GW_PASSWORD) {
    E.status = GwStatus::NO_CONFIG;
    model::put(E);
    LOGF("[net] src/secrets.h needs to be filled in\n");
    vTaskDelete(nullptr);
  }
  loadBase();
  loadHist();
  vppLoad();
  model::put(E);
  gwTls.setInsecure();
  wxTls.setInsecure();
  WiFi.onEvent([](arduino_event_id_t, arduino_event_info_t info) {
    static int last = -1;
    const int r = info.wifi_sta_disconnected.reason;
    if (r != last) LOGF("[net] WiFi disconnected, reason %d\n", r);  // 15 = password rejected
    last = r;
  }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  LOGF("[net] WiFi MAC %s\n", WiFi.macAddress().c_str());
  configTzTime(TZ_INFO, "pool.ntp.org", "time.nist.gov");
  bool wasUp = false;
  uint32_t lastWait = millis();
  unsigned apTry = 0;
  for (;;) {
    const bool up = WiFi.status() == WL_CONNECTED;
    // Not connected after 20 s: the network can have several access points with the same name, and one of them
    // may reject the password (seen: a mesh node failing the handshake while another AP works). Try each AP
    // with our network name in turn, strongest first.
    if (!up && millis() - lastWait > 20000) {
      lastWait = millis();
      WiFi.disconnect();
      delay(200);
      const int n = WiFi.scanNetworks();
      struct Ap { uint8_t bssid[6]; int ch, rssi; } aps[8];
      int na = 0;
      for (int i = 0; i < n && na < 8; i++) {
        if (WiFi.SSID(i) != WIFI_SSID) continue;
        memcpy(aps[na].bssid, WiFi.BSSID(i), 6);
        aps[na].ch = WiFi.channel(i);
        aps[na++].rssi = WiFi.RSSI(i);
      }
      WiFi.scanDelete();
      std::sort(aps, aps + na, [](const Ap &a, const Ap &b) { return a.rssi > b.rssi; });
      if (na == 0) {
        LOGF("[net] WiFi \"%s\" not found, retrying\n", WIFI_SSID);
        WiFi.begin(WIFI_SSID, WIFI_PASS);
      } else {
        const Ap &a = aps[apTry++ % na];
        LOGF("[net] WiFi: trying access point %d of %d (%02X:%02X:%02X:%02X:%02X:%02X, %d dBm)\n", (int)((apTry - 1) % na) + 1,
             na, a.bssid[0], a.bssid[1], a.bssid[2], a.bssid[3], a.bssid[4], a.bssid[5], a.rssi);
        WiFi.begin(WIFI_SSID, WIFI_PASS, a.ch, a.bssid);
      }
    }
    if (up != wasUp) {
      wasUp = up;
      LOGF("[net] WiFi %s %s %d dBm\n", up ? "connected" : "disconnected", up ? WiFi.localIP().toString().c_str() : "", up ? WiFi.RSSI() : 0);
      if (!up) {
        gwTls.stop();
        E.status = GwStatus::NO_WIFI;
        model::put(E);
      }
    }
    MonthRec r;
    if (histQ && xQueueReceive(histQ, &r, 0) == pdTRUE) {
      storeHist(r);
      model::put(E);
    }
    VppRec v;
    if (vppQ && xQueueReceive(vppQ, &v, 0) == pdTRUE) {
      vppStore(v);
      model::put(E);
    }
    if (up) {
      gwPoll();
      wxPoll();
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}
}  // namespace

void seed_month(float solar, float home, float imp, float exp) {
  seedKwh[C_SOLAR] = solar;
  seedKwh[C_HOME] = home;
  seedKwh[C_IMP] = imp;
  seedKwh[C_EXP] = exp;
  seedReq = true;
}

void save_history(const MonthRec &r) {
  if (histQ) xQueueSend(histQ, &r, 0);
}

void save_vpp(const VppRec &r) {
  if (vppQ) xQueueSend(vppQ, &r, 0);
}

void begin() {
  histQ = xQueueCreate(16, sizeof(MonthRec));
  vppQ = xQueueCreate(16, sizeof(VppRec));
  xTaskCreatePinnedToCore(task, "net", 16384, nullptr, 1, nullptr, 1);
}

}  // namespace net

#pragma once
// Shared data: the network task writes it, the display task reads it (full copies under a mutex)
#include <Arduino.h>
#include <math.h>
#include <time.h>

// A closed month (or loaded from Netzero), with that month's LUMA price
struct MonthRec {
  uint32_t key;  // YYYYMM
  float solar, home, imp, exp;  // kWh
  float rate, fixed;            // $/kWh and that month's fixed charge
  uint8_t from;                 // 1 = full month; otherwise, day counting started from
};
constexpr int MAX_HIST = 24;

// VPP for one month
struct VppRec {
  uint32_t key;     // YYYYMM
  uint16_t events;
  float kwh;        // sent to LUMA during events
  time_t last_at;   // start of the last event (0 = unknown)
  float last_kwh;
};

enum class GwStatus : uint8_t { STARTING, NO_CONFIG, NO_WIFI, LOGIN, OK, BAD_PASSWORD, LOCKED, UNREACHABLE };

struct Energy {
  GwStatus status = GwStatus::STARTING;
  uint32_t at = 0;        // millis() of the last good reading
  float pct = NAN;        // battery on the app's scale
  int8_t grid = -1;       // 1 = LUMA connected, 0 = outage (island), -1 = unknown
  float solar_w = NAN, home_w = NAN;
  float battery_w = NAN;  // + discharging, - charging
  float grid_w = NAN;     // + buying from LUMA, - selling
  float usable_wh = NAN;  // energy left to use
  float home_avg_w = NAN; // home consumption, averaged (~10 min)
  float net_avg_w = NAN;  // home minus solar, averaged (~10 min)
  // Month totals (kWh), from the Gateway counters minus their values at the start of the month
  bool month_ok = false;
  uint32_t month_key = 0; // YYYYMM of the current month
  uint8_t month = 0;      // 1..12
  uint8_t month_from = 1; // day counting starts from (1 = full month)
  float m_solar = NAN, m_home = NAN, m_imp = NAN, m_exp = NAN;
  uint8_t n_hist = 0;
  MonthRec hist[MAX_HIST];  // previous months, oldest to newest
  // VPP
  bool vpp_active = false;   // event in progress
  float vpp_w = NAN;         // what the battery is sending to LUMA now (W)
  float vpp_event_kwh = 0;   // sent during the current event
  bool gs_active = false;    // Gateway's grid_services_active (for comparison only)
  VppRec vpp;                // current month (key 0 = no data yet)
  uint8_t n_vhist = 0;
  VppRec vhist[MAX_HIST];    // previous months, oldest to newest
};

constexpr int MAX_ALERTS = 4;
struct Alert {
  char event[48];    // NWS name in English ("Flash Flood Warning"); the display translates it
  char severity[12]; // Extreme / Severe / Moderate / Minor / Unknown
  time_t ends;       // 0 = no end time
  uint32_t hash;     // to sound only for new alerts
};

struct Storm {
  bool valid = false;
  char name[24];
  char cls[4];  // HU, TS, TD, STS, PTC...
  int mph;
  int dist_mi;
  char bearing[3];  // from the house: N, NE, E...
};

struct Weather {
  uint32_t fc_at = 0, alerts_at = 0, storms_at = 0;  // millis() of the last good reading of each
  float temp = NAN, tmax = NAN, tmin = NAN;
  int code = -1;  // WMO sky code
  int rain = -1;  // today's chance of rain (%)
  uint8_t n_alerts = 0;
  Alert alerts[MAX_ALERTS];
  uint8_t n_storms = 0;
  Storm storm;  // the closest one
};

namespace model {
void begin();
void get(Energy &e, Weather &w);
void put(const Energy &e);
void put(const Weather &w);
}  // namespace model

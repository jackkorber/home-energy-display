#pragma once
#include "model.h"
// Network task (core 1): WiFi, NTP time, Powerwall Gateway (local) and weather (NWS, NHC, Open-Meteo)
namespace net {
void begin();
// Month-to-date (kWh) from another source (Netzero): adjusts the baseline so the month starts complete
void seed_month(float solar, float home, float imp, float exp);
// Save a past month (from Netzero) into the board's history
void save_history(const MonthRec &r);
// VPP data for one month (from Netzero). If it is the current month, it replaces the counters.
void save_vpp(const VppRec &r);
}

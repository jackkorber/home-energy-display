#pragma once
// Settings. Anything defined in secrets.h (written by install.sh) overrides the defaults below.
#if __has_include("secrets.h")
#include "secrets.h"
#endif

// Home location: NWS alerts, forecast and distance to tropical storms
#ifndef HOME_LAT
#define HOME_LAT 18.4655f  // San Juan
#endif
#ifndef HOME_LON
#define HOME_LON -66.1057f
#endif
#ifndef HOME_NAME
#define HOME_NAME "San Juan"
#endif

// Puerto Rico: AST all year (no daylight saving time)
#ifndef TZ_INFO
#define TZ_INFO "AST4"
#endif

// Fallback when the Gateway does not report the real pack energy (13.5 kWh per Powerwall 2)
#ifndef BATTERY_KWH
#define BATTERY_KWH 13.5f
#endif

// Power company. The display shows this name ("LUMA", "PREPA", "PG&E"...): keep it short and ASCII.
#ifndef UTILITY_NAME
#define UTILITY_NAME "LUMA"
#endif
// Older secrets.h files used LUMA_RATE / LUMA_FIXED
#if !defined(ENERGY_RATE) && defined(LUMA_RATE)
#define ENERGY_RATE LUMA_RATE
#endif
#if !defined(FIXED_CHARGE) && defined(LUMA_FIXED)
#define FIXED_CHARGE LUMA_FIXED
#endif
// Price per kWh bought. Default: LUMA Oct-Dec 2026 (Energy Bureau): 33.86 ¢/kWh all-in for 800 kWh = $270.86,
// i.e. $8 fixed customer charge + 800 x 32.86 ¢. LUMA changes it every quarter: update from your bill.
#ifndef ENERGY_RATE
#define ENERGY_RATE 0.3286f  // $ per kWh bought
#endif
#ifndef FIXED_CHARGE
#define FIXED_CHARGE 8.0f  // fixed customer charge per month ($)
#endif
// Price per kWh sold. Net metering (LUMA) credits exports at the same price as imports.
#ifndef SELL_RATE
#define SELL_RATE ENERGY_RATE
#endif

// VPP / battery sharing program (LUMA: Customer Battery Energy Sharing). 0 = no VPP screen and no event detection.
#ifndef VPP_ENABLED
#define VPP_ENABLED 1
#endif
// What you are paid per kWh your battery sends during an event
#ifndef VPP_RATE
#define VPP_RATE 1.00f
#endif
// An event = the battery sending energy to the grid (with exports set to "solar only" that never happens outside
// an event). The Gateway's grid_services_active flag is not reliable: it also turns on while charging from the grid.
#define VPP_START_W 500     // starts when sending more than this...
#define VPP_START_MS 60000  // ...for 1 min
#define VPP_END_W 200       // ends when sending less than this...
#define VPP_END_MS 180000   // ...for 3 min

// Board battery care: on USB, stop charging at this % and charge again below the second one
#ifndef CHARGE_STOP_PCT
#define CHARGE_STOP_PCT 80
#endif
#ifndef CHARGE_RESUME_PCT
#define CHARGE_RESUME_PCT 70
#endif

// Night: screen off (touch wakes it) unless there is an outage or a serious alert; only critical sounds
#ifndef NIGHT_START_H
#define NIGHT_START_H 22
#endif
#ifndef NIGHT_END_H
#define NIGHT_END_H 7
#endif
#define BRIGHT_DAY 170   // 0..255
#define BRIGHT_NIGHT 40  // at night, only during an outage or serious alert
#define WAKE_MS 30000    // how long it stays on after a touch at night

// Polling intervals
#define GW_POLL_MS 5000
#define GW_STATUS_MS 60000           // pack energy (/api/system_status)
#define WX_ALERTS_MS (2 * 60000)     // NWS alerts
#define WX_FORECAST_MS (15 * 60000)  // Open-Meteo
#define WX_STORMS_MS (30 * 60000)    // NHC

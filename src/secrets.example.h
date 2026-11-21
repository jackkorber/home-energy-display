#pragma once
// Example of the file install.sh writes as src/secrets.h. secrets.h is git-ignored: never commit it.

#define WIFI_SSID "MyNetwork"
#define WIFI_PASS "my-wifi-password"

// Powerwall Gateway on the home network. It may have two IPs (Ethernet and Wi-Fi): the second is tried if the
// first does not answer. Reserve them in your router so they don't change.
#define GW_HOSTS {"192.168.1.50"}
// Gateway "customer" login (the one you use at https://<gateway-ip>). The email can be anything.
#define GW_PASSWORD "gateway-password"
#define GW_EMAIL "display@home.local"

// Optional: everything below has a default in config.h
#define HOME_LAT 18.4655f
#define HOME_LON -66.1057f
#define HOME_NAME "San Juan"
#define BATTERY_KWH 13.5f  // 13.5 per Powerwall 2
#define LUMA_RATE 0.3286f
#define LUMA_FIXED 8.0f
#define VPP_RATE 1.00f

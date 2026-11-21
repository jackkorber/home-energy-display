#!/usr/bin/env bash
# Home Energy Display installer: writes src/secrets.h, checks the Powerwall Gateway login, builds and flashes.
#
# Fill in the values below (or leave them empty and the script will ask), then run:  ./install.sh
# Llena los valores de abajo (o déjalos vacíos y el script te los pide) y corre:     ./install.sh
set -euo pipefail

# ---------------------------------------------------------------------------------------------------------------
WIFI_SSID=""          # Wi-Fi name / nombre del WiFi (2.4 GHz)
WIFI_PASS=""          # Wi-Fi password / contraseña del WiFi
GW_HOST=""            # Gateway IP, e.g. 192.168.1.50 (empty = search the network / vacío = buscarlo en la red)
GW_PASSWORD=""        # Gateway "customer" password / contraseña del Gateway
TOWN=""               # Town for weather, e.g. Ponce / pueblo para el tiempo
POWERWALLS=""         # Number of Powerwall 2 / cantidad de Powerwall 2
LUMA_RATE="0.3286"    # $/kWh (Oct–Dec 2026). Update from your LUMA bill / actualízalo con tu factura
LUMA_FIXED="8"        # Fixed monthly charge / cargo fijo mensual ($)
VPP_RATE="1.00"       # $ per kWh sent during a VPP event / $ por kWh enviado en un evento VPP
# ---------------------------------------------------------------------------------------------------------------
# Tip: put the same variables in a file named settings.env instead (it is git-ignored), so your passwords never end
# up in a commit. / Consejo: pon las mismas variables en settings.env (git lo ignora) para no subir tus claves.

cd "$(dirname "$0")"
[ -f settings.env ] && . ./settings.env
say()  { printf '\033[1m%s\033[0m\n' "$*"; }
warn() { printf '\033[33m%s\033[0m\n' "$*"; }
die()  { printf '\033[31m%s\033[0m\n' "$*" >&2; exit 1; }
ask()  { local v; read -r -p "$1: " v; printf '%s' "$v"; }
ask_secret() { local v; read -r -s -p "$1: " v; echo >&2; printf '%s' "$v"; }
# Escape a value for a C string literal
cstr() { printf '%s' "$1" | sed -e 's/\\/\\\\/g' -e 's/"/\\"/g'; }

command -v curl >/dev/null || die "curl is required / hace falta curl"
command -v python3 >/dev/null || die "python3 is required / hace falta python3"

# ---- PlatformIO
PIO=""
if command -v pio >/dev/null; then PIO=pio
elif [ -x "$HOME/.platformio/penv/bin/pio" ]; then PIO="$HOME/.platformio/penv/bin/pio"
else
  warn "PlatformIO not found / no encontré PlatformIO."
  read -r -p "Install it now with pip? / ¿Instalarlo con pip? [y/N] " yn
  [ "${yn:-n}" = "y" ] || [ "${yn:-n}" = "Y" ] || die "Install PlatformIO: https://platformio.org/install/cli"
  python3 -m pip install --user -U platformio
  PIO="$(python3 -m site --user-base)/bin/pio"
  [ -x "$PIO" ] || die "pio was installed but not found in PATH"
fi

# ---- Wi-Fi
[ -n "$WIFI_SSID" ] || WIFI_SSID="$(ask 'Wi-Fi name / nombre del WiFi')"
[ -n "$WIFI_PASS" ] || WIFI_PASS="$(ask_secret 'Wi-Fi password / contraseña del WiFi')"
[ -n "$WIFI_SSID" ] || die "Wi-Fi name is required"

# ---- Find the Gateway: any host on this /24 answering /api/status with a "din"
find_gateways() {
  local router prefix
  if command -v route >/dev/null && route -n get default >/dev/null 2>&1; then
    router="$(route -n get default | awk '/gateway:/{print $2}')"
  else
    router="$(ip route 2>/dev/null | awk '/^default/{print $3; exit}')"
  fi
  [ -n "$router" ] || return 0
  prefix="${router%.*}"
  for i in $(seq 1 254); do
    ( curl -sk -m 2 "https://$prefix.$i/api/status" 2>/dev/null | grep -q '"din"' && echo "$prefix.$i" ) &
  done
  wait
}

if [ -z "$GW_HOST" ]; then
  say "Searching for the Powerwall Gateway... / Buscando el Gateway..."
  FOUND="$(find_gateways | sort -t. -k4 -n | tr '\n' ' ')"
  [ -n "$FOUND" ] || die "Gateway not found. Set GW_HOST in install.sh / No lo encontré: pon GW_HOST en install.sh"
  say "Found / Encontrado: $FOUND"
  GW_HOSTS_LIST="$FOUND"
else
  GW_HOSTS_LIST="$GW_HOST"
fi
FIRST_GW="${GW_HOSTS_LIST%% *}"

# ---- Check the login (the password goes through stdin, not the command line)
[ -n "$GW_PASSWORD" ] || GW_PASSWORD="$(ask_secret 'Gateway password / contraseña del Gateway')"
BODY="$(GW_PASSWORD="$GW_PASSWORD" python3 -c 'import json,os;print(json.dumps({"username":"customer","password":os.environ["GW_PASSWORD"],"email":"display@home.local","force_sm_off":False}))')"
CODE="$(printf '%s' "$BODY" | curl -sk -m 8 -o /dev/null -w '%{http_code}' -H 'Content-Type: application/json' --data-binary @- "https://$FIRST_GW/api/login/Basic" || true)"
case "$CODE" in
  200) say "Gateway login OK" ;;
  401|403) die "Wrong Gateway password (HTTP $CODE). Too many tries lock the Gateway for a while. / Contraseña incorrecta." ;;
  429) die "Gateway locked after too many tries (HTTP 429). Wait ~10 min. / Gateway bloqueado, espera ~10 min." ;;
  *) die "Gateway did not answer the login (HTTP $CODE) at $FIRST_GW" ;;
esac

# ---- Location (Open-Meteo geocoding, no key)
LAT="" LON="" NAME=""
[ -n "$TOWN" ] || TOWN="$(ask 'Town for weather, Enter = San Juan / pueblo (Enter = San Juan)')"
if [ -n "$TOWN" ]; then
  GEO="$(curl -s -m 10 -G "https://geocoding-api.open-meteo.com/v1/search" --data-urlencode "name=$TOWN" \
           --data-urlencode "count=1" --data-urlencode "countryCode=PR" || true)"
  read -r LAT LON NAME <<<"$(printf '%s' "$GEO" | python3 -c '
import json,sys
try:
    r=json.load(sys.stdin)["results"][0]; print(r["latitude"], r["longitude"], r["name"])
except Exception: pass')" || true
  if [ -z "$LAT" ]; then warn "Town not found, using San Juan / no encontré el pueblo, uso San Juan"; NAME=""; fi
fi

# ---- Battery size
[ -n "$POWERWALLS" ] || POWERWALLS="$(ask 'How many Powerwall 2? / ¿Cuántos Powerwall 2? (Enter = 1)')"
POWERWALLS="${POWERWALLS:-1}"
case "$POWERWALLS" in ''|*[!0-9]*) die "Powerwall count must be a number" ;; esac
BATTERY_KWH="$(python3 -c "print(13.5*$POWERWALLS)")"

# ---- Write src/secrets.h
HOSTS_C="$(for h in $GW_HOSTS_LIST; do printf '"%s", ' "$h"; done)"
umask 077
{
  echo '#pragma once'
  echo '// Written by install.sh. Do not commit this file.'
  echo "#define WIFI_SSID \"$(cstr "$WIFI_SSID")\""
  echo "#define WIFI_PASS \"$(cstr "$WIFI_PASS")\""
  echo "#define GW_HOSTS {${HOSTS_C%, }}"
  echo "#define GW_PASSWORD \"$(cstr "$GW_PASSWORD")\""
  echo '#define GW_EMAIL "display@home.local"'
  if [ -n "$LAT" ]; then
    echo "#define HOME_LAT ${LAT}f"
    echo "#define HOME_LON ${LON}f"
    echo "#define HOME_NAME \"$(cstr "$NAME")\""
  fi
  echo "#define BATTERY_KWH ${BATTERY_KWH}f"
  echo "#define LUMA_RATE ${LUMA_RATE}f"
  echo "#define LUMA_FIXED ${LUMA_FIXED}f"
  echo "#define VPP_RATE ${VPP_RATE}f"
} > src/secrets.h
say "Wrote src/secrets.h"

# ---- Build and flash
# The board shows up as a USB modem (macOS) or ttyACM (Linux); PlatformIO's auto-detect can pick another device
PORTS="$(ls /dev/cu.usbmodem* /dev/ttyACM* 2>/dev/null || true)"
while [ -z "$PORTS" ]; do
  read -r -p "Plug the board in by USB, then press Enter / Conecta la placa por USB y presiona Enter " _
  PORTS="$(ls /dev/cu.usbmodem* /dev/ttyACM* 2>/dev/null || true)"
done
PORT="$(printf '%s\n' $PORTS | head -1)"
[ "$(printf '%s\n' $PORTS | wc -l)" -gt 1 ] && warn "Several boards found, using $PORT / varias placas, uso $PORT"
say "Building and flashing on $PORT / Compilando y flasheando en $PORT..."
"$PIO" run -t upload --upload-port "$PORT"
say "Done! / ¡Listo! To see the log / para ver el registro:  $PIO device monitor -b 115200 -p $PORT"

#!/usr/bin/env bash
# Build one firmware variant.
#
#   ./build.sh wifi              Wi-Fi + MQTT + web UI           (needs ESP-IDF only)
#   ./build.sh matter-wifi       Matter over Wi-Fi (+ web, MQTT) (needs ESP-IDF + esp-matter)
#   ./build.sh matter-thread     Matter over Thread              (needs ESP-IDF + esp-matter)
#   ./build.sh all               Everything: Wi-Fi only, Matter/Wi-Fi or Matter/Thread, picked on the device
#
# Extra arguments go to idf.py, e.g.  ./build.sh wifi flash monitor -p /dev/cu.usbmodem1101
# Each variant has its own build directory and sdkconfig, so they do not disturb each other.
set -euo pipefail
cd "$(dirname "$0")"

variant="${1:-}"
if [ -z "$variant" ]; then
  sed -n '2,9p' "$0"
  exit 1
fi
shift

if [ -z "${IDF_PATH:-}" ]; then
  echo "IDF_PATH is not set: run the ESP-IDF export.sh first (see docs/BUILDING.md)" >&2
  exit 1
fi
export AUTOTERM_VARIANT="$variant"

case "$variant" in
  wifi)
    defaults="sdkconfig.defaults;sdkconfig.defaults.wifi"
    ;;
  matter-wifi|matter-thread|all)
    if [ -z "${ESP_MATTER_PATH:-}" ]; then
      echo "ESP_MATTER_PATH is not set: point it at your esp-matter checkout (see docs/MATTER.md)" >&2
      exit 1
    fi
    light="$ESP_MATTER_PATH/examples/light"
    # esp-matter's own known-good defaults first, ours after so ours win
    defaults="$light/sdkconfig.defaults"
    if [ ! -f "$defaults" ]; then
      echo "missing $defaults" >&2
      exit 1
    fi
    if [ "$variant" = matter-thread ]; then
      if [ ! -f "$light/sdkconfig.defaults.c6_thread" ]; then
        echo "missing $light/sdkconfig.defaults.c6_thread" >&2
        exit 1
      fi
      defaults="$defaults;$light/sdkconfig.defaults.c6_thread"
    fi
    if [ "$variant" = all ]; then
      if [ ! -f "$light/sdkconfig.defaults.c6_wifi_thread" ]; then
        echo "missing $light/sdkconfig.defaults.c6_wifi_thread" >&2
        exit 1
      fi
      defaults="$defaults;$light/sdkconfig.defaults.c6_wifi_thread"
    fi
    defaults="$defaults;sdkconfig.defaults;sdkconfig.defaults.$variant"
    ;;
  *)
    echo "unknown variant: $variant" >&2
    exit 1
    ;;
esac

common=(-B "build/$variant" -D "SDKCONFIG=build/$variant/sdkconfig" -D "SDKCONFIG_DEFAULTS=$defaults")

# set-target only needs to run once per build directory
if [ ! -f "build/$variant/sdkconfig" ]; then
  idf.py "${common[@]}" set-target esp32c6
fi
exec idf.py "${common[@]}" "$@"

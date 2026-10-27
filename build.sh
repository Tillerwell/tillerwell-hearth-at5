#!/usr/bin/env bash
# Build the Hearth firmware: one image, with Wi-Fi only, Matter over Wi-Fi and Matter over Thread chosen at run time.
# Needs ESP-IDF and esp-matter (see docs/BUILDING.md and docs/MATTER.md).
#
#   ./build.sh build                          build, output build/all/hearth.bin
#   ./build.sh flash monitor -p /dev/cu.usbmodem1101
#
# Every argument goes to idf.py. The build directory is build/all.
set -euo pipefail
cd "$(dirname "$0")"

if [ -z "${IDF_PATH:-}" ]; then
  echo "IDF_PATH is not set: run the ESP-IDF export.sh first (see docs/BUILDING.md)" >&2
  exit 1
fi
if [ -z "${ESP_MATTER_PATH:-}" ]; then
  echo "ESP_MATTER_PATH is not set: point it at your esp-matter checkout (see docs/MATTER.md)" >&2
  exit 1
fi
light="$ESP_MATTER_PATH/examples/light"
if [ ! -f "$light/sdkconfig.defaults" ]; then
  echo "missing $light/sdkconfig.defaults" >&2
  exit 1
fi
if [ ! -f "$light/sdkconfig.defaults.c6_wifi_thread" ]; then
  echo "missing $light/sdkconfig.defaults.c6_wifi_thread" >&2
  exit 1
fi

# esp-matter's own known-good defaults first, ours after so ours win
defaults="$light/sdkconfig.defaults;$light/sdkconfig.defaults.c6_wifi_thread;sdkconfig.defaults;sdkconfig.defaults.all"
common=(-B build/all -D SDKCONFIG=build/all/sdkconfig -D "SDKCONFIG_DEFAULTS=$defaults")

# set-target only needs to run once per build directory
if [ ! -f build/all/sdkconfig ]; then
  idf.py "${common[@]}" set-target esp32c6
fi
exec idf.py "${common[@]}" "$@"

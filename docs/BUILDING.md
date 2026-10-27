# Building and flashing

## Firmware (ESP-IDF and esp-matter)

There is one image, `all`. It needs ESP-IDF v5.5.x, which supports the ESP32-C6, and esp-matter (see [MATTER.md](MATTER.md) for installing it and setting `ESP_MATTER_PATH`).

```bash
git clone --depth 1 --branch v5.5.1 --recursive --shallow-submodules https://github.com/espressif/esp-idf.git ~/esp/esp-idf
cd ~/esp/esp-idf && ./install.sh esp32c6
python3 tools/idf_tools.py install cmake ninja     # if cmake/ninja are not already installed
. ./export.sh
```

Then, from this repository:

```bash
./build.sh build
./build.sh -p /dev/cu.usbmodemXXXX flash monitor
```

First flash is over the board's USB-C port (hold BOOT while plugging in if it isn't recognised). **Never power the 5V pin and USB together.** After that, updates can go over the air from the web page: upload `build/all/hearth.bin`.

The component manager downloads `espressif/mdns` on the first build, so that needs internet.

## Tests without hardware

```bash
make -C host_tests
```

Runs the protocol, relay/injector (against a simulated heater and controller), thermostat, timer and time zone tests with address and undefined-behaviour sanitizers. To regenerate the reference vectors from the base project's Python: `make -C host_tests vectors BASE=/path/to/autoterm-heater-control/autoterm`.

## Matter

See [MATTER.md](MATTER.md).

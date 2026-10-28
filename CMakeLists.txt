cmake_minimum_required(VERSION 3.16)

# Needs esp-matter; build.sh checks it.
#   ESP_MATTER_PATH  = path to an esp-matter checkout
# Mirrors esp-matter's examples/light/CMakeLists.txt.
if(NOT DEFINED ENV{ESP_MATTER_PATH})
    message(FATAL_ERROR "ESP_MATTER_PATH is not set (see docs/MATTER.md)")
endif()
# esp-matter's shared app_reset component needs a button; the hollow device supplies a stub one
set(ENV{ESP_MATTER_DEVICE_PATH} $ENV{ESP_MATTER_PATH}/device_hal/device/hollow)
set(ESP_MATTER_PATH $ENV{ESP_MATTER_PATH})
set(MATTER_SDK_PATH ${ESP_MATTER_PATH}/connectedhomeip/connectedhomeip)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

include(${ESP_MATTER_PATH}/examples/common/cmake_common/components_include.cmake)
set(EXTRA_COMPONENT_DIRS
    "${ESP_MATTER_PATH}/examples/common"
    "${MATTER_SDK_PATH}/config/esp32/components"
    "${ESP_MATTER_PATH}/components"
    # only the stub button from device_hal: its led_driver pulls in led_strip 1.x, which clashes
    # with the 2.x our own LED code uses
    "${ESP_MATTER_PATH}/device_hal/button_driver/button")

# RoamOS version for Hearth: one digit, a point, four digits (brand guidelines,
# Naming). Bump the four digits by one for every release. This is the only place
# the version is set: PROJECT_VER is what the web page, the API and Matter's
# version string show, and PROJECT_VER_NUMBER (0.1234 -> 1234, 1.0002 -> 10002)
# is Matter's software version, which its OTA update check compares.
set(ROAMOS_VERSION "1.0034")
include(roamos_version.cmake)
roamos_version_number("${ROAMOS_VERSION}" PROJECT_VER_NUMBER)
set(PROJECT_VER "${ROAMOS_VERSION}")
project(hearth)

idf_build_set_property(CXX_COMPILE_OPTIONS "-std=gnu++17;-Os;-DCHIP_HAVE_CONFIG_H;-Wno-overloaded-virtual" APPEND)
idf_build_set_property(C_COMPILE_OPTIONS "-Os" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-Wno-format-nonliteral;-Wno-format-security" APPEND)

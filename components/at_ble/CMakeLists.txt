# env, not CONFIG_: requirements are resolved before sdkconfig exists. The plain Wi-Fi and the unified images have Bluetooth
# setup (the unified one only in Wi-Fi mode); the Matter-only images use the radio's Bluetooth for Matter commissioning instead.
if("$ENV{AUTOTERM_VARIANT}" MATCHES "^(wifi|all)$")
    idf_component_register(SRCS "at_ble.c"
                           INCLUDE_DIRS "include"
                           REQUIRES at_app at_net at_led
                           PRIV_REQUIRES bt json esp_timer esp_hw_support log)
else()
    idf_component_register()
endif()

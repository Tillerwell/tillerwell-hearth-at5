# env, not CONFIG_: requirements are resolved before sdkconfig exists
if("$ENV{AUTOTERM_VARIANT}" STREQUAL "matter-thread")
    idf_component_register()
else()
    idf_component_register(SRCS "at_mqtt.c"
                           INCLUDE_DIRS "include"
                           REQUIRES at_app
                           PRIV_REQUIRES mqtt json esp_timer log espressif__mosquitto)
endif()

idf_component_register(SRCS "at_ble.c"
                       INCLUDE_DIRS "include"
                       REQUIRES at_app at_net at_led
                       PRIV_REQUIRES bt json esp_timer esp_hw_support log)

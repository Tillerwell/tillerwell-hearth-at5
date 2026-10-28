idf_component_register(SRCS "at_led.c"
                       INCLUDE_DIRS "include"
                       REQUIRES at_app at_net at_access
                       PRIV_REQUIRES esp_timer log)

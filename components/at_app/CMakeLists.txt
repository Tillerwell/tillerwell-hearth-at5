idf_component_register(SRCS "at_app.c" "at_settings.c" "at_json.c" "at_app_timers.c"
                       INCLUDE_DIRS "include"
                       REQUIRES at_core at_proto
                       PRIV_REQUIRES json driver esp_adc nvs_flash esp_timer esp_system esp_hw_support esp_app_format log lwip)

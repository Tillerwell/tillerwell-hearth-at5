idf_component_register(SRCS "at_app.c" "at_settings.c" "at_json.c" "at_app_timers.c" "at_app_fuel.c" "at_crash.c"
                       INCLUDE_DIRS "include"
                       REQUIRES at_core at_proto
                       PRIV_REQUIRES json driver esp_adc nvs_flash esp_timer esp_system esp_hw_support esp_app_format log lwip)
# at_crash.c keeps the crash point across the restart: the panic handler is wrapped so it runs first.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_panic_handler")

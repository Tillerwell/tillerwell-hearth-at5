# env, not CONFIG_: requirements are resolved before sdkconfig exists
if("$ENV{AUTOTERM_VARIANT}" STREQUAL "matter-thread")
    idf_component_register()
else()
    idf_component_register(SRCS "at_web.c"
                           INCLUDE_DIRS "include"
                           EMBED_TXTFILES "www/index.html"
                           REQUIRES at_app at_net at_mqtt
                           PRIV_REQUIRES esp_http_server json app_update esp_system esp_timer mbedtls esp_app_format log)
endif()

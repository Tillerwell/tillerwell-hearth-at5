idf_component_register(SRCS "at_autoupdate.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES at_app at_fwupdate at_cloudlink at_net json mbedtls esp_http_client esp_app_format esp_timer esp_system heap log)

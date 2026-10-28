idf_component_register(SRCS "at_fwupdate_policy.c" "at_fwupdate.c"
                       INCLUDE_DIRS "include"
                       REQUIRES at_core
                       PRIV_REQUIRES json esp_https_ota esp_http_client mbedtls app_update esp_app_format esp_system esp_partition log)

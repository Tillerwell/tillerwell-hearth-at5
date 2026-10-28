idf_component_register(SRCS "at_access.c" "at_access_idf.c"
                       INCLUDE_DIRS "include"
                       REQUIRES at_auth
                       PRIV_REQUIRES at_app esp_timer esp_hw_support esp_system log)

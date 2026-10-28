idf_component_register(SRCS "at_cloudlink_policy.c" "at_cloudlink.c"
                       INCLUDE_DIRS "include"
                       REQUIRES at_cloud
                       PRIV_REQUIRES at_app at_access at_auth at_net mqtt json esp_timer esp_system esp_app_format esp_hw_support heap log)

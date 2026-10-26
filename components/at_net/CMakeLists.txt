idf_component_register(SRCS "at_net.c"
                       INCLUDE_DIRS "include"
                       REQUIRES at_app
                       PRIV_REQUIRES esp_wifi esp_netif esp_event esp_timer esp_hw_support lwip log)

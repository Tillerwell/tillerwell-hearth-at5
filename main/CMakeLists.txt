idf_component_register(SRCS "main.c" "console.c"
                       INCLUDE_DIRS "."
                       REQUIRES at_access at_cloud at_cloudlink at_autoupdate at_app at_net at_web at_mqtt at_matter at_ble at_led
                       PRIV_REQUIRES console esp_driver_gpio esp_system nvs_flash)

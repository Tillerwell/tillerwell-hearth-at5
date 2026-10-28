idf_component_register(SRCS "at_cloud.c" "at_cloud_idf.c"
                       INCLUDE_DIRS "include"
                       EMBED_TXTFILES "certs/hearth-root-ca.pem"
                       PRIV_REQUIRES at_app mbedtls esp_hw_support log)

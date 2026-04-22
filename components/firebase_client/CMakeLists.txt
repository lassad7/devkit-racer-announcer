idf_component_register(
    SRCS "firebase_client.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES
        esp_http_client
        esp_wifi
        esp_event
        esp_netif
        mbedtls
)

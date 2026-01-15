idf_component_register(
    SRCS "webrtc_client.c"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash esp_http_client esp_https_ota
)


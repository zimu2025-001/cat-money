idf_component_register(SRCS "main.c" "cam_udp_protocol.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_wifi esp_timer nvs_flash esp32-camera)

idf_component_register(SRCS "main.c" "jpeg_enc.c" "blimp_control.c" "blimp_udp.c" "blimp_mdns.c" "mpu6050.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_video esp_cam_sensor driver imx219 esp_timer esp_wifi esp_http_server esp_driver_jpeg nvs_flash esp_netif json espressif__mdns)

# The demosaic loop is the frame-rate bottleneck; the inherited sdkconfig uses
# -Og, so optimize this application component without changing IDF libraries.
target_compile_options(${COMPONENT_LIB} PRIVATE -O2)

idf_component_register(SRCS "main.c" "tw_proto.c" "tw_led.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_wifi esp_netif esp_timer nvs_flash esp_driver_usb_serial_jtag esp_psram esp_driver_rmt
                                app_update esp_partition esp_app_format mbedtls)

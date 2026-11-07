idf_component_register(SRCS "main.c" "tw_proto.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_wifi esp_netif esp_timer nvs_flash esp_driver_usb_serial_jtag esp_psram)

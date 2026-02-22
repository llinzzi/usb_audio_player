idf_component_register(SRCS usb_audio_player_main.c
                    INCLUDE_DIRS "."
                    REQUIRES fatfs sdmmc esp_driver_spi esp_driver_i2c)

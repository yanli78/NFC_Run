idf_component_register(SRCS "pn532_reader.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_driver_i2c json log status_led ha_client)
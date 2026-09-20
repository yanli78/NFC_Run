idf_component_register(SRCS "button_reader.c"
                       INCLUDE_DIRS "include"
                       REQUIRES log status_led ha_client driver)
idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS "."
                    REQUIRES sys_defs drv_espnow_receiver freertos nvs_flash)

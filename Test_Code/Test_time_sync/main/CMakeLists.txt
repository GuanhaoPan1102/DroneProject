idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS "."
                    REQUIRES sys_defs drv_gnss_sync drv_sd_card freertos)

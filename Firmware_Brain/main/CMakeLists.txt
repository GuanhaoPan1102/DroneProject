idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS "."
                    REQUIRES sys_defs drv_uart_recv drv_sd_card drv_espnow_receiver freertos)

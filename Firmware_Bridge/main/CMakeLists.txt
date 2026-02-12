idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS "."
                    REQUIRES sys_defs drv_gnss drv_uart_recv drv_sd_card freertos)

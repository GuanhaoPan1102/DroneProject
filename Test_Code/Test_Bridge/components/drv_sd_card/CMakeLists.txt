idf_component_register(SRCS "drv_sd_card.c"
                    INCLUDE_DIRS "include"
                    REQUIRES sys_defs drv_espnow_sender freertos driver sdmmc fatfs esp_timer vfs log)

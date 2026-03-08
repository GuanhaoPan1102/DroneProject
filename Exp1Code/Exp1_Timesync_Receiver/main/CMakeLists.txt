idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS "."
                    REQUIRES drv_gnss_sync freertos esp_wifi nvs_flash driver sdmmc fatfs)

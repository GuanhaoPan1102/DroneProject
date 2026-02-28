#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "sys/time.h"

#include "drv_espnow_receiver.h"

void app_main(void) {
    // 初始化 NVS (Wi-Fi 需要)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 啟動接收端
    espnow_receiver_init();
    
    // Master 可以去做自己的事，或者就在這裡掛著
    while(1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
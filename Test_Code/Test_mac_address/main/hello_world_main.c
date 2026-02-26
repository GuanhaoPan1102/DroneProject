#include <stdio.h>
#include "esp_mac.h"  // 讀取 MAC 需要這個標頭檔
#include "esp_log.h"

static const char *TAG = "GET_MAC";

void app_main(void)
{
    uint8_t mac[6];
    
    // 讀取 Wi-Fi Station 的預設 MAC Address
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    
    ESP_LOGI(TAG, "=========================================");
    ESP_LOGI(TAG, "請複製這組 Master MAC Address 到 Slave 的程式裡：");
    ESP_LOGI(TAG, "{0x%02X, 0x%02X, 0x%02X, 0x%02X, 0x%02X, 0x%02X}", 
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    ESP_LOGI(TAG, "=========================================");
}
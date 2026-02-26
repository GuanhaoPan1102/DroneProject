#include <stdio.h>
#include "esp_log.h"
#include "esp_mac.h"
#include "driver/uart.h"
#include "sys_defs.h"

#include <time.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "drv_gnss_sync.h"
#include "drv_uart_recv.h"
#include "drv_sd_card.h"

static const char *TAG = "MAIN";

void app_main(void)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    ESP_LOGI(TAG, "MAC Address {0x%02X, 0x%02X, 0x%02X, 0x%02X, 0x%02X, 0x%02X}", 
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    
	// 建立 Queue
    QueueHandle_t recv_queue = xQueueCreate(50, sizeof(ble_packet_queue_item_t));

    // 初始化 SDcard, UART 模組
    gnss_sync_init(UART_NUM_2, 23, 18, 19, 9600);

    configure_neo7m(UART_NUM_2);
    //configure_atgm336h(UART_NUM_2);


    ESP_LOGI(TAG, "Waiting for System Ready (Position Fix & Time Sync)...");

    while (1) {
        gps_fix_t status = gnss_get_fix();

        // 條件：位置鎖定 (is_fixed) 且 時間同步 (is_time_synced)
        if (status.is_fixed && status.is_time_synced) {
            
            // A. 設定時區 (台灣是 UTC+8，POSIX 寫法是 "CST-8")
            setenv("TZ", "CST-8", 1);
            tzset();

            // B. 取得系統時間 (這個時間已經被 GNSS Driver 裡的 PPS 校正過了)
            time_t now;
            struct tm timeinfo;
            time(&now);
            localtime_r(&now, &timeinfo);

            // C. 格式化輸出字串 (例如: 2026-02-14 12:00:01)
            char time_str[64];
            strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &timeinfo);

            ESP_LOGI(TAG, "============================================");
            ESP_LOGI(TAG, "System Ready & Synced!");
            ESP_LOGI(TAG, " [Position] Lat: %d, Lon: %d, Alt: %.2f m", 
                     status.latitude, status.longitude, status.altitude);
            ESP_LOGI(TAG, " [Time]     %s (Taiwan Time)", time_str);
            ESP_LOGI(TAG, "============================================");
            
            break; // 成功 跳出迴圈
        }

        // 顯示等待進度
        if (!status.is_fixed) {
            ESP_LOGI(TAG, "Wait: Surveying Position... (Need 60 samples)");
        } else if (!status.is_time_synced) {
            ESP_LOGI(TAG, "Wait: Position Fixed. Waiting for next PPS pulse...");
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    // 初始化其他周邊 (SD 卡, Master 通訊)
    ESP_LOGI(TAG, "Step 2: Initializing SD Card & UART...");
    
    sd_card_init(recv_queue);
    uart_init(recv_queue);
    
    return;
}

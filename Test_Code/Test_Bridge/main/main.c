#include <stdio.h>
#include "esp_log.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "sys_defs.h"
#include "nvs_flash.h" // 引入 NVS

#include <time.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "drv_gnss_sync.h"
#include "drv_uart_recv.h"
#include "drv_sd_card.h"
#include "drv_espnow_sender.h" // 引入 ESP-NOW 發送模組

static const char *TAG = "MAIN_BRIDGE";

// 定義狀態指示燈腳位
#define LED_GPIO 21 

// ==========================================
// ⚠️ 【節點設定】請依據不同開發板修改 ID 與 Master MAC
// ==========================================
#define MY_NODE_ID 4
static const uint8_t MASTER_MAC[6] = {0x2C, 0xBC, 0xBB, 0xA8, 0x4A, 0x70}; 

void app_main(void)
{
    // ==========================================
    // 1. 初始化 NVS (ESP-NOW 與 Wi-Fi 啟動的先決條件)
    // ==========================================
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // ==========================================
    // 2. 初始化狀態指示燈 (LED)
    // ==========================================
    gpio_config_t led_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << LED_GPIO),
        .pull_down_en = 0,
        .pull_up_en = 0,
    };
    gpio_config(&led_conf);
    gpio_set_level(LED_GPIO, 0);

    // ==========================================
    // 3. 建立資料佇列與初始化 GNSS
    // ==========================================
    QueueHandle_t recv_queue = xQueueCreate(50, sizeof(ble_packet_queue_item_t));

    gnss_sync_init(UART_NUM_2, 23, 18, 19, 9600);
    configure_atgm336h(UART_NUM_2); 

    ESP_LOGI(TAG, "Waiting for System Ready (Position Fix & PPS Time Sync)...");

    // ==========================================
    // 4. 嚴格等待微秒級時間同步
    // ==========================================
    while (1) {
        gps_fix_t status = gnss_get_fix();

        if (status.is_fixed && status.is_time_synced) {
            
            time_t now;
            time(&now);
            time_t now_tw = now + (8 * 3600);
            struct tm timeinfo;
            gmtime_r(&now_tw, &timeinfo);

            char time_str[64];
            strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &timeinfo);

            ESP_LOGI(TAG, "============================================");
            ESP_LOGI(TAG, " Bridge System Ready & PPS Time Synced!");
            ESP_LOGI(TAG, " [Position] Lat: %d, Lon: %d, Alt: %.2f m", 
                     status.latitude, status.longitude, status.altitude);
            ESP_LOGI(TAG, " [Time]     %s (Taiwan Time)", time_str);
            ESP_LOGI(TAG, "============================================");
            
            gpio_set_level(LED_GPIO, 1);
            
            // ==========================================
            // 5. 初始化 ESP-NOW 並發送【真實註冊封包】
            // ==========================================
            ESP_LOGI(TAG, "Initializing ESP-NOW and sending Register Packet...");
            ESP_ERROR_CHECK(espnow_sender_init(MASTER_MAC));

            // 取得當下的高精度時間
            struct timeval tv_reg;
            gettimeofday(&tv_reg, NULL);

            espnow_payload_t reg_msg = {0};
            reg_msg.msg_type = MSG_TYPE_REGISTER;
            reg_msg.node_id  = MY_NODE_ID;
            
            // 填入剛抓到的真實 GPS 數據
            reg_msg.data.reg.lat = status.latitude;  
            reg_msg.data.reg.lon = status.longitude;
            reg_msg.data.reg.alt = status.altitude;
            
            reg_msg.data.reg.timestamp = ((int64_t)tv_reg.tv_sec * 1000) + (tv_reg.tv_usec / 1000);

            esp_err_t send_res = espnow_sender_send(&reg_msg);
            if (send_res == ESP_OK) {
                ESP_LOGI(TAG, "-> 註冊封包發送成功！Master 已收到 (包含時間戳)。");
            } else {
                ESP_LOGE(TAG, "-> 註冊封包發送失敗！請檢查 Master 是否開機。");
            }

            break; // 成功，跳出無窮迴圈
        }

        if (!status.is_fixed) {
            ESP_LOGW(TAG, "Wait: Surveying Position... (Need clear sky)");
        } else if (!status.is_time_synced) {
            ESP_LOGW(TAG, "Wait: Position Fixed. Waiting for PPS pulse to sync time...");
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    // ==========================================
    // 6. 啟動後端任務 (SD 卡與 UART 接收)
    // ==========================================
    ESP_LOGI(TAG, "Step 2: Initializing SD Card & UART Receiver...");
    
    sd_card_init(recv_queue);
    uart_init(recv_queue);
    
    return;
}
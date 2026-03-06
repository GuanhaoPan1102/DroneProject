#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"

#include "drv_gnss_sync.h"
#include "drv_sd_card.h"

static const char *TAG = "MAIN_APP";

#define TRIGGER_GPIO    22
#define LED_GPIO        21
#define GPS_TX_PIN      23
#define GPS_RX_PIN      18
#define GPS_PPS_PIN     4   

#define ESP_INTR_FLAG_DEFAULT 0

static QueueHandle_t sd_packet_queue = NULL;
static uint32_t test_seq_num = 1;

// GPIO 22 中斷服務 (防彈跳 + 抓時間)
static void IRAM_ATTR gpio_22_isr_handler(void* arg)
{
    static uint32_t last_intr_time = 0;
    uint32_t now = xTaskGetTickCountFromISR();
    // 軟體防抖：200ms 內只採樣一次
    if (now - last_intr_time > pdMS_TO_TICKS(200)) {
        gpio_set_level(LED_GPIO, 0);
        sync_log_event_t event;
        // 第一時間抓取精確時間
        gettimeofday(&event.tv, NULL); 
        event.seq_num = test_seq_num++; 

        BaseType_t xHigherPriorityTaskWoken = pdFALSE;
        xQueueSendFromISR(sd_packet_queue, &event, &xHigherPriorityTaskWoken);
        
        last_intr_time = now;
        if (xHigherPriorityTaskWoken) {
            portYIELD_FROM_ISR();
        }
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Starting GNSS Sync Test System...");

    // 1. 初始化 GNSS (驅動不變)
    gnss_sync_init(UART_NUM_2, GPS_TX_PIN, GPS_RX_PIN, GPS_PPS_PIN, 9600);
    //configure_atgm336h(UART_NUM_2);
    configure_neo7m(UART_NUM_2);
    //configure_neo6m(UART_NUM_2);

    // 2. 初始化 SD 卡與佇列 (改用新的結構大小)
    sd_packet_queue = xQueueCreate(20, sizeof(sync_log_event_t));
    if (sd_card_init(sd_packet_queue) != ESP_OK) {
        ESP_LOGE(TAG, "SD Card Mount Failed!");
    }

    // 3. 設定 GPIO 22
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_POSEDGE,
        .pin_bit_mask = (1ULL << TRIGGER_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_down_en = 1,
        .pull_up_en = 0,
    };
    gpio_config(&io_conf);

    // 安全地註冊 GPIO 中斷服務 (如果已安裝則忽略錯誤)
    esp_err_t isr_ret = gpio_install_isr_service(ESP_INTR_FLAG_DEFAULT);
    if (isr_ret != ESP_OK && isr_ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Failed to install ISR service: %s", esp_err_to_name(isr_ret));
    }
    gpio_isr_handler_add(TRIGGER_GPIO, gpio_22_isr_handler, NULL);

    gpio_config_t led_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << LED_GPIO),
        .pull_down_en = 0,
        .pull_up_en = 0,
    };
    gpio_config(&led_conf);
    gpio_set_level(LED_GPIO, 0); // 預設先關閉，等同步完再開

    ESP_LOGI(TAG, "System Ready! Press button to log exact time.");

    // 新增一個變數，用來記錄是否已經印過同步時間
    bool has_printed_time = false;

    // 主迴圈
    while (1) {
        gps_fix_t fix = gnss_get_fix(); 
        
        if (fix.is_fixed) {
            // 如果鎖定了，且「還沒印過時間」
            if (!has_printed_time) {
                // 1. 獲取當前精確的 UTC+0 系統時間
                struct timeval tv;
                gettimeofday(&tv, NULL);
                
                // 2. 加上 8 小時的秒數 (28800秒)，轉換為 UTC+8
                time_t tw_sec = tv.tv_sec + 28800;
                
                // 3. 轉換為人類可讀的年月日時分秒
                struct tm timeinfo;
                localtime_r(&tw_sec, &timeinfo);
                
                ESP_LOGI(TAG, "Initial Synced Time (UTC+8): %04d-%02d-%02d %02d:%02d:%02d.%06ld",
                         timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                         timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec,
                         (long)tv.tv_usec);
                
                // 標記為已印出，下次就不會再進來了
                gpio_set_level(LED_GPIO, 1);
                has_printed_time = true; 
            }
            // 已經印過之後，迴圈到這裡什麼都不做，保持終端機安靜
            
        } else {
            // 還沒鎖定前，每 5 秒提醒一次
            ESP_LOGW(TAG, "Waiting for GNSS Fix and Time Sync...");
        }
        
        vTaskDelay(pdMS_TO_TICKS(5000)); 
    }
}
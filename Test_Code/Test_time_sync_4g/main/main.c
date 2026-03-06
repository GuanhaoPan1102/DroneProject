#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "esp_log.h"

#include "drv_4g_sync.h"
#include "drv_sd_card.h"

static const char *TAG = "MAIN_4G_SYNC";

#define TRIGGER_GPIO    22
#define ESP_INTR_FLAG_DEFAULT 0

static QueueHandle_t sd_packet_queue = NULL;
static uint32_t test_seq_num = 1;
static char rx_buf[2048];

static void IRAM_ATTR gpio_22_isr_handler(void* arg)
{
    static uint32_t last_intr_time = 0;
    uint32_t now = xTaskGetTickCountFromISR();

    if (now - last_intr_time > pdMS_TO_TICKS(200)) {
        sync_log_event_t event;
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
    esp_err_t err;
    ESP_LOGI(TAG, "========== 4G 模組獨立時間同步測試啟動 ==========");

    // 1. 初始化 SD 卡
    sd_packet_queue = xQueueCreate(20, sizeof(sync_log_event_t));
    if (sd_card_init(sd_packet_queue) != ESP_OK) {
        ESP_LOGE(TAG, "SD Card Mount Failed!");
    }

    // 2. 初始化 4G UART 並開啟 GPS
    ESP_LOGI(TAG, "\n--> Step 1: 初始化 4G 模組...");
    drv_4g_init();
    
    err = drv_4g_send_at_cmd("AT\r\n", "OK", 2000, NULL, 0);
    if (err == ESP_OK) ESP_LOGI(TAG, "[PASS] AT 握手成功");
    
    memset(rx_buf, 0, sizeof(rx_buf));
    err = drv_4g_send_at_cmd("ATI\r\n", "OK", 2000, rx_buf, sizeof(rx_buf));
    if (err == ESP_OK) ESP_LOGI(TAG, "[PASS] 取得模組資訊:\n%s", rx_buf);

    ESP_LOGI(TAG, "\n--> Step 2: 開啟 4G 模組 GPS 電源...");
    drv_4g_gnss_power(true);

    ESP_LOGI(TAG, "\n--> [驗證] 啟動 NMEA 串流，純監聽印出 15 秒...");
    
    // 1. 手動要求模組開始吐 NMEA
    drv_4g_start_nmea_stream();
    
    // 2. 劃一條分隔線，並呼叫純監聽函式
    printf("\n================ NMEA RAW DATA START ================\n");
    drv_4g_listen_and_print(120000); // 卡在這裡聽 15 秒 (15000ms)
    printf("================ NMEA RAW DATA END ==================\n\n");

    // 3. 測試完畢先關閉串流，讓 UART 恢復乾淨
    drv_4g_stop_nmea_stream();
    
    ESP_LOGI(TAG, "NMEA 原始資料驗證結束。接下來進入正式同步流程...");
    vTaskDelay(pdMS_TO_TICKS(2000)); // 喘息 2 秒

    // 3. 執行時間同步
    ESP_LOGI(TAG, "\n--> Step 3: 等待 GPS 定位與時間同步 (最長等待 5 分鐘)...");
    
    err = drv_4g_sync_time_from_gps(300000); 
    
    if (err == ESP_OK) {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        time_t tw_sec = tv.tv_sec + 28800; 
        struct tm timeinfo;
        localtime_r(&tw_sec, &timeinfo);
        
        ESP_LOGI(TAG, "================================================");
        ESP_LOGI(TAG, " 🕒 系統時間已同步完成！");
        ESP_LOGI(TAG, " 本地時間: %04d-%02d-%02d %02d:%02d:%02d",
                 timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                 timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
        ESP_LOGI(TAG, "================================================\n");
    } else {
        ESP_LOGE(TAG, "❌ GPS 定位超時！時間未同步。請將天線移至空曠處重試。");
    }

    // 4. 設定測試觸發 GPIO 22
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_POSEDGE,
        .pin_bit_mask = (1ULL << TRIGGER_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_down_en = 1,
        .pull_up_en = 0,
    };
    gpio_config(&io_conf);

    esp_err_t isr_ret = gpio_install_isr_service(ESP_INTR_FLAG_DEFAULT);
    if (isr_ret != ESP_OK && isr_ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "ISR Service install failed.");
    }
    gpio_isr_handler_add(TRIGGER_GPIO, gpio_22_isr_handler, NULL);

    ESP_LOGI(TAG, "\n========== 系統準備就緒，請隨時按下按鈕觸發 INT 寫入 SD 卡 ==========");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
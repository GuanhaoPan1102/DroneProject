#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "esp_vfs_fat.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"

// 【已移除 GPS 驅動標頭檔】

static const char *TAG = "NODE_RX_NOGPS";
#define MOUNT_POINT "/sdcard"
#define LED_PIN 21

// ==========================================
// 🚀 燒錄前必改：設定這塊接收板的專屬名稱！
// ==========================================
#define MY_NODE_ID "S1"

// 雙方必須完全一致的通訊結構體 (配合 Master 的 S0)
typedef struct {
    char     node_name[4]; 
    uint32_t seq_num;
} __attribute__((packed)) sync_test_packet_t;

typedef struct {
    uint32_t seq_num;
    int64_t  tv_sec;
    int32_t  tv_usec;
} log_item_t;

static QueueHandle_t s_log_queue;

// ==========================================
// ESP-NOW 接收 Callback
// ==========================================
static void espnow_recv_cb(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len) 
{
    if (len != sizeof(sync_test_packet_t)) return;

    // 抓取系統時間 (注意：因無 GPS，這只是 ESP32 開機後的相對時間)
    struct timeval tv;
    gettimeofday(&tv, NULL);

    sync_test_packet_t *pkt = (sync_test_packet_t *)data;

    log_item_t item;
    item.seq_num = pkt->seq_num;
    
    // 依然保留時間格式以相容 Python 腳本
    item.tv_sec  = (int64_t)tv.tv_sec; 
    item.tv_usec = tv.tv_usec;

    xQueueSendFromISR(s_log_queue, &item, NULL); 
}

// ==========================================
// SD 卡寫入任務 (支援 5 秒 Timeout 自動分檔、防覆蓋與 LED 指示)
// ==========================================
static void sd_card_write_task(void *pvParameters)
{
    log_item_t item;
    FILE *f = NULL;
    int file_index = 1;
    bool is_recording = false;
    int count = 0;

    // 1. 啟動時掃描 SD 卡，自動依據 MY_NODE_ID 尋找下一個可用的檔名
    while (1) {
        char check_name[64];
        snprintf(check_name, sizeof(check_name), MOUNT_POINT "/%s_%03d.csv", MY_NODE_ID, file_index);
        FILE *temp = fopen(check_name, "r");
        if (temp != NULL) {
            fclose(temp); 
            file_index++; 
        } else {
            break; 
        }
    }
    
    ESP_LOGI(TAG, "SD 卡準備就緒！");
    ESP_LOGI(TAG, "等待 Master (S0) 廣播，下一份資料將存為: %s_%03d.csv", MY_NODE_ID, file_index);

    while (1) {
        // 2. 等待 Queue，Timeout 5000 毫秒 (5 秒)
        if (xQueueReceive(s_log_queue, &item, pdMS_TO_TICKS(5000)) == pdTRUE) {
            
            // 如果目前沒有在記錄中，代表這是「新的一輪實驗」的開始！
            if (!is_recording) {
                char filename[64];
                snprintf(filename, sizeof(filename), MOUNT_POINT "/%s_%03d.csv", MY_NODE_ID, file_index);
                f = fopen(filename, "w");
                
                if (f == NULL) {
                    ESP_LOGE(TAG, "無法建立檔案: %s", filename);
                    continue; 
                }
                
                // 寫入 CSV 標頭
                fprintf(f, "seq_num,tv_sec,tv_usec\n");
                is_recording = true;
                count = 0;

                // 【修正 LED 指示】回歸你的硬體邏輯：寫 1 點亮 LED！
                gpio_set_level(LED_PIN, 1); 

                ESP_LOGI(TAG, ">>> 收到 S0 廣播！開始新實驗，正在記錄至: %s", filename);
            }

            // 正常寫入資料
            if (f != NULL) {
                fprintf(f, "%lu,%lld,%ld\n", item.seq_num, item.tv_sec, item.tv_usec);
                count++;
                if (count >= 10) { 
                    fsync(fileno(f)); // 每 10 筆強制寫入實體卡，防掉電
                    count = 0;
                }
            }

        } else {
            // 3. Timeout 觸發 (連續 5 秒都沒有收到資料，代表 Master 停止發送了)
            if (is_recording) {
                ESP_LOGI(TAG, "--- 超過 5 秒未收到 S0 廣播，判斷本輪實驗結束 ---");
                
                // 【修正 LED 指示】回歸你的硬體邏輯：寫 0 熄滅 LED！
                gpio_set_level(LED_PIN, 0); 

                // 將檔案緩衝區推入 SD 卡並關閉檔案
                if (f != NULL) {
                    fflush(f);
                    fsync(fileno(f));
                    fclose(f);
                    f = NULL;
                }
                
                is_recording = false;
                file_index++; 
                ESP_LOGI(TAG, "節點待命中，下一輪實驗將存為: %s_%03d.csv", MY_NODE_ID, file_index);
            }
        }
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "初始化實驗 Receiver 節點 (%s) [無 GPS 輕量模式]...", MY_NODE_ID);

    // 【修正 LED 初始化】預設 0 = 接地 = 熄滅待命
    gpio_reset_pin(LED_PIN);
    gpio_set_direction(LED_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(LED_PIN, 0); 

    // 1. 初始化 NVS
    ESP_ERROR_CHECK(nvs_flash_init());

    // 【已移除】GPS 初始化與等待同步的區塊

    // 2. 基礎網路與 Wi-Fi 初始化
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_LR));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE));

    // 3. 初始化 SD 卡與 Queue
    s_log_queue = xQueueCreate(100, sizeof(log_item_t));
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 2,
        .allocation_unit_size = 16 * 1024
    };
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 1;
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    sdmmc_card_t *card;
    
    if (esp_vfs_fat_sdmmc_mount(MOUNT_POINT, &host, &slot_config, &mount_config, &card) == ESP_OK) {
        xTaskCreate(sd_card_write_task, "sd_task", 4096, NULL, 5, NULL);
    } else {
        ESP_LOGE(TAG, "SD 卡掛載失敗！請檢查硬體。");
    }

    // 4. 初始化 ESP-NOW 並註冊 Callback
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(espnow_recv_cb));

    ESP_LOGI(TAG, "Receiver 已進入監聽模式，等待 Master(S0) 廣播！");
}
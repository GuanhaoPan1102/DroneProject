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

// 引入你的 GPS 同步驅動
#include "drv_gnss_sync.h"

static const char *TAG = "NODE_RX";
#define MOUNT_POINT "/sdcard"
#define LED_PIN 21

typedef struct {
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

    // 收到廣播瞬間，立刻抓時間！
    struct timeval tv;
    gettimeofday(&tv, NULL);

    sync_test_packet_t *pkt = (sync_test_packet_t *)data;

    log_item_t item;
    item.seq_num = pkt->seq_num;
    
    // 【新增】加上 8 小時 (28800 秒) 轉換為台灣時間 (+8 時區)
    item.tv_sec  = (int64_t)tv.tv_sec + 28800; 
    item.tv_usec = tv.tv_usec;

    xQueueSendFromISR(s_log_queue, &item, NULL); // 使用 FromISR 確保中斷安全
}

// ==========================================
// SD 卡寫入任務 (支援 5 秒 Timeout 自動分檔與防覆蓋機制)
// ==========================================
static void sd_card_write_task(void *pvParameters)
{
    log_item_t item;
    FILE *f = NULL;
    int file_index = 1;
    bool is_recording = false;
    int count = 0;

    // 1. 啟動時先掃描 SD 卡，找到下一個可用的檔案編號，避免覆蓋舊資料
    while (1) {
        char check_name[64];
        snprintf(check_name, sizeof(check_name), MOUNT_POINT "/S4_%03d.csv", file_index);
        FILE *temp = fopen(check_name, "r");
        if (temp != NULL) {
            fclose(temp); // 檔案已存在，關閉它
            file_index++; // 編號 +1 繼續尋找下一個
        } else {
            break; // 找不到該檔案，代表這個編號是空的，可以使用！
        }
    }
    
    ESP_LOGI(TAG, "SD 卡準備就緒！");
    ESP_LOGI(TAG, "等待 Master 廣播中，下一份實驗資料將存為: S4_%03d.csv", file_index);

    while (1) {
        // 2. 等待 Queue，將無限期等待改為 Timeout 5000 毫秒 (5 秒)
        if (xQueueReceive(s_log_queue, &item, pdMS_TO_TICKS(5000)) == pdTRUE) {
            
            // 如果目前沒有在記錄中，代表這是「新的一輪實驗」的開始！
            if (!is_recording) {
                char filename[64];
                snprintf(filename, sizeof(filename), MOUNT_POINT "/S4_%03d.csv", file_index);
                f = fopen(filename, "w");
                
                if (f == NULL) {
                    ESP_LOGE(TAG, "無法建立檔案: %s", filename);
                    continue; // 建立失敗，略過這次寫入
                }
                
                fprintf(f, "seq_num,tv_sec,tv_usec\n");
                is_recording = true;
                count = 0;

                gpio_set_level(LED_PIN, 1);

                ESP_LOGI(TAG, ">>> 收到廣播！開始新一輪實驗，正在記錄至: %s", filename);
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
            // 3. Timeout 觸發 (連續 5 秒都沒有收到 Queue 傳來的資料)
            if (is_recording) {
                ESP_LOGI(TAG, "--- 超過 5 秒未收到廣播，判斷本輪實驗結束 ---");
                ESP_LOGI(TAG, "已安全封裝儲存: S4_%03d.csv", file_index);
                
                gpio_set_level(LED_PIN, 0);

                // 將檔案緩衝區推入 SD 卡並關閉檔案
                if (f != NULL) {
                    fflush(f);
                    fsync(fileno(f));
                    fclose(f);
                    f = NULL;
                }
                
                is_recording = false;
                file_index++; // 檔案編號 +1，準備迎接下一輪實驗
                ESP_LOGI(TAG, "節點待命中，下一輪實驗將存為: SYNC_TEST_%03d.csv", file_index);
            }
        }
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "初始化實驗 Receiver 節點...");

    gpio_reset_pin(LED_PIN);
    gpio_set_direction(LED_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(LED_PIN, 0); // 預設熄滅

    // 1. 初始化 NVS
    ESP_ERROR_CHECK(nvs_flash_init());

    // 2. 初始化 GPS 與等待同步 (這是確保實驗有意義的關鍵！)
    gnss_sync_init(UART_NUM_2, 23, 18, 19, 9600);
    configure_neo7m(UART_NUM_2); 

    ESP_LOGI(TAG, "等待 GPS 定位與 PPS 時間同步...");
    while (1) {
        gps_fix_t status = gnss_get_fix();
        if (status.is_fixed && status.is_time_synced) {
            ESP_LOGI(TAG, "====================================");
            ESP_LOGI(TAG, " 系統時間已與 GNSS PPS 完美對齊！");
            ESP_LOGI(TAG, " 準備啟動 ESP-NOW 測試模式...");
            ESP_LOGI(TAG, "====================================");
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    // 3. 基礎網路與 Wi-Fi 初始化
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_LR));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE));

    // 4. 初始化 SD 卡與 Queue
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

    // 5. 初始化 ESP-NOW 並註冊 Callback
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(espnow_recv_cb));

    ESP_LOGI(TAG, "節點已進入監聽模式，等待 Master 廣播！");
}
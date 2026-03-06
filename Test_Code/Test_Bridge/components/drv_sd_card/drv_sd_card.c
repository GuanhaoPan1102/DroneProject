#include <stdio.h>
#include <string.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "drv_sd_card.h"
#include "sys_defs.h"

#include <time.h>
#include <sys/time.h>

// 引入 ESP-NOW 發送模組，用於檔案儲存後的回報
#include "drv_espnow_sender.h"

static const char *TAG = "SD_CARD";
#define MOUNT_POINT "/sdcard"

// ⚠️ 【節點設定】確保這裡的 ID 與你的系統規劃一致
#ifndef MY_NODE_ID
#define MY_NODE_ID 1 
#endif

// 設定逾時時間：3秒無資料則關閉檔案
#define SESSION_TIMEOUT_MS 3000 
// 設定 Sync 頻率：每 10 筆資料做一次 fsync (防斷電)
#define SYNC_INTERVAL 10

#define LED_GPIO 21

// 內部變數
static sdmmc_card_t *card = NULL;
static bool is_mounted = false;
static TaskHandle_t s_sd_task_handle = NULL;
static int s_next_file_index = 1;

// --- 輔助函式：尋找下一個可用的檔案編號 ---
static void find_next_free_index(void) {
    char test_filename[64];
    struct stat st;
    int i = 1;
    while (1) {
        snprintf(test_filename, sizeof(test_filename), MOUNT_POINT "/data%d.csv", i);
        if (stat(test_filename, &st) != 0) {
            // 檔案不存在，代表這個編號可用
            s_next_file_index = i;
            ESP_LOGI(TAG, "Next available file index: %d (Filename: %s)", s_next_file_index, test_filename);
            break;
        }
        i++;
    }
}

// --- 主要的 SD 卡寫入任務 ---
static void sd_card_write_task(void *pvParameters)
{
    QueueHandle_t queue = (QueueHandle_t)pvParameters;
    ble_packet_queue_item_t item;
    
    FILE *f = NULL;
    char current_filename[64] = {0}; 
    char line_buffer[128];
    int unsaved_count = 0;

    ESP_LOGI(TAG, "ML Data Logger Task Started");

    while (1) {
        // 等待資料，逾時時間設為 3000ms (3秒)
        if (xQueueReceive(queue, &item, pdMS_TO_TICKS(SESSION_TIMEOUT_MS)) == pdTRUE) {
            
            if (!is_mounted) continue;

            // --- 狀態 1: 新的一筆資料進來，但檔案還沒開 (代表新 Session 開始) ---
            if (f == NULL) {
                gpio_set_level(LED_GPIO, 0);

                snprintf(current_filename, sizeof(current_filename), MOUNT_POINT "/data%d.csv", s_next_file_index);
                
                f = fopen(current_filename, "w");
                if (f == NULL) {
                    ESP_LOGE(TAG, "Failed to create file: %s", current_filename);
                    gpio_set_level(LED_GPIO, 1);
                    continue;
                }

                // 寫入 CSV 表頭 (保留微秒欄位)
                fprintf(f, "tv_sec,tv_usec,rssi,seq_num,lat,lon,alt_m,spd_kmh\n");
                
                ESP_LOGI(TAG, ">>> Start New Recording: %s", current_filename);
                s_next_file_index++; 
                unsaved_count = 0;
            }

            // --- 狀態 2: 處理資料寫入 ---
            struct timeval tv;
            gettimeofday(&tv, NULL);
            
            // 加上 8 小時 (28800 秒) 轉換為台灣時間 (依據您的需求設定)
            long long local_tv_sec = (long long)tv.tv_sec + 28800;

            // 格式化資料，分離 tv_sec 與 tv_usec 以保留最高精度
            int len = snprintf(line_buffer, sizeof(line_buffer), 
                    "%lld,%ld,%d,%u,%.7f,%.7f,%d,%.2f\n",
                    local_tv_sec,
                    (long)tv.tv_usec,
                    item.rssi,
                    item.payload.seq_num,
                    (double)item.payload.lat / 10000000.0,
                    (double)item.payload.lon / 10000000.0,
                    item.payload.alt,
                    (double)item.payload.spd / 100.0
            );

            fwrite(line_buffer, 1, len, f);
            unsaved_count++;

            // 定期 Sync (防止斷電資料遺失，但不關檔)
            if (unsaved_count >= SYNC_INTERVAL) {
                fsync(fileno(f));
                unsaved_count = 0;
            }

        } else {
            // --- 狀態 3: 逾時發生 (超過 3 秒沒收到資料) ---
            if (f != NULL) {
                // 關閉檔案，代表本次 Session 結束
                fclose(f);
                f = NULL;
                unsaved_count = 0;
                ESP_LOGI(TAG, "<<< Timeout (3s). Saved & Closed: %s", current_filename);
                gpio_set_level(LED_GPIO, 1);

                // ==========================================
                // 觸發 ESP-NOW：傳送「存檔回報」給 Master
                // ==========================================
                espnow_payload_t file_msg = {0};
                file_msg.msg_type = MSG_TYPE_FILE_SAVED;
                file_msg.node_id  = MY_NODE_ID;
                
                // 填入當下時間戳 (毫秒) 用於回報
                struct timeval tv;
                gettimeofday(&tv, NULL);
                file_msg.data.file.timestamp = ((int64_t)tv.tv_sec * 1000) + (tv.tv_usec / 1000); 

                // 擷取檔名部分 (去掉 "/sdcard/" 路徑，只保留 "dataX.csv")
                char *base_name = strrchr(current_filename, '/');
                if (base_name) base_name++; 
                else base_name = current_filename;
                
                strlcpy(file_msg.data.file.filename, base_name, sizeof(file_msg.data.file.filename));

                // 發送封包
                esp_err_t send_res = espnow_sender_send(&file_msg);
                if (send_res == ESP_OK) {
                    ESP_LOGI(TAG, "-> 存檔回報發送成功！通知 Master 已儲存: %s", base_name);
                } else {
                    ESP_LOGE(TAG, "-> 存檔回報發送失敗！");
                }
            }
        }
    }
}

// --- 初始化 SD 卡 ---
esp_err_t sd_card_init(QueueHandle_t packet_queue)
{
    if (is_mounted) return ESP_OK;
    if (packet_queue == NULL) return ESP_ERR_INVALID_ARG;

    ESP_LOGI(TAG, "Initializing SD (1-bit, 20MHz)...");

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = true,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024
    };

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 1;
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_err_t ret = esp_vfs_fat_sdmmc_mount(MOUNT_POINT, &host, &slot_config, &mount_config, &card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Mount failed: %s", esp_err_to_name(ret));
        return ret;
    }
    is_mounted = true;

    // 掃描 SD 卡，找出下一個可用的檔案編號
    find_next_free_index();

    // 啟動寫入任務
    xTaskCreate(sd_card_write_task, "sd_ml_writer", 4096, (void*)packet_queue, 2, &s_sd_task_handle);
    
    return ESP_OK;
}

// --- 卸載 SD 卡 ---
void sd_card_deinit(void)
{
    if (s_sd_task_handle != NULL) {
        vTaskDelete(s_sd_task_handle);
        s_sd_task_handle = NULL;
    }
    if (is_mounted) {
        esp_vfs_fat_sdcard_unmount(MOUNT_POINT, card);
        is_mounted = false;
        card = NULL;
    }
}
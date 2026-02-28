#include <stdio.h>
#include <string.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "drv_sd_card.h"
#include "sys_defs.h"

#include <time.h>
#include <sys/time.h>

static const char *TAG = "SD_CARD";
#define MOUNT_POINT "/sdcard"

// 設定逾時時間：3秒無資料則關閉檔案
#define SESSION_TIMEOUT_MS 3000 
// 設定 Sync 頻率：每 10 筆資料做一次 fsync (防斷電)
#define SYNC_INTERVAL 10

// 內部變數
static sdmmc_card_t *card = NULL;
static bool is_mounted = false;
static TaskHandle_t s_sd_task_handle = NULL;
static int s_next_file_index = 1; // 下一個檔案的編號

// --- 輔助函式：尋找下一個可用的檔案編號 ---
// 這樣可以確保重開機後，不會覆蓋掉舊的 data1, data2...
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

static void sd_card_write_task(void *pvParameters)
{
    QueueHandle_t queue = (QueueHandle_t)pvParameters;
    ble_packet_queue_item_t item;
    
    FILE *f = NULL;
    char current_filename[64] = {0}; // 保存當前檔名
    char line_buffer[128];
    int unsaved_count = 0;

    ESP_LOGI(TAG, "ML Data Logger Task Started");

    while (1) {
        // 等待資料，逾時時間設為 3000ms (3秒)
        if (xQueueReceive(queue, &item, pdMS_TO_TICKS(SESSION_TIMEOUT_MS)) == pdTRUE) {
            
            if (!is_mounted) continue;

            // --- 狀態 1: 新的一筆資料進來，但檔案還沒開 (代表新 Session 開始) ---
            if (f == NULL) {
                // 1. 決定新檔名：data{N}.csv
                snprintf(current_filename, sizeof(current_filename), MOUNT_POINT "/data%d.csv", s_next_file_index);
                
                // 2. 開啟檔案 (寫入模式 'w')
                f = fopen(current_filename, "w");
                if (f == NULL) {
                    ESP_LOGE(TAG, "Failed to create file: %s", current_filename);
                    continue;
                }

                // 3. 寫入 CSV 表頭
                fprintf(f, "unix_time_ms,rssi,seq_num,lat,lon,alt_m,spd_kmh\n");
                
                // 4. 印出檔名與開始訊息
                ESP_LOGI(TAG, ">>> Start New Recording: %s", current_filename);
                
                // 5. 準備下一次的編號
                s_next_file_index++; 
                unsaved_count = 0;
            }

            // --- 狀態 2: 處理資料寫入 ---
            
            struct timeval tv;
            gettimeofday(&tv, NULL);
            int64_t timestamp = ((int64_t)tv.tv_sec * 1000) + (tv.tv_usec / 1000);
            timestamp -= 57600000;

            // 格式化資料 (這段完全不用動，因為 timestamp 還是 int64_t)
            int len = snprintf(line_buffer, sizeof(line_buffer), 
                    "%lld,%d,%d,%.7f,%.7f,%d,%.2f\n",
                    timestamp,
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

                // 印出儲存訊息
                ESP_LOGI(TAG, "<<< Timeout (3s). Saved & Closed: %s", current_filename);
            }
        }
    }
}

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

    // 【新增】初始化時先掃描 SD 卡，找出目前應該從 data 幾號開始
    find_next_free_index();

    xTaskCreate(sd_card_write_task, "sd_ml_writer", 4096, (void*)packet_queue, 2, &s_sd_task_handle);
    
    return ESP_OK;
}

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
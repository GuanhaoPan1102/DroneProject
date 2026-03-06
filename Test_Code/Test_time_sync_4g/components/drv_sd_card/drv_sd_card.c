#include <stdio.h>
#include <string.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "esp_log.h"
#include <time.h>

#include "drv_sd_card.h"

static const char *TAG = "SD_CARD";
#define MOUNT_POINT "/sdcard"
#define SESSION_TIMEOUT_MS 3000 
#define SYNC_INTERVAL 5 // 每 5 筆做一次 fsync 防斷電

static sdmmc_card_t *card = NULL;
static bool is_mounted = false;
static TaskHandle_t s_sd_task_handle = NULL;
static int s_next_file_index = 1;

static void find_next_free_index(void) {
    char test_filename[64];
    struct stat st;
    int i = 1;
    while (1) {
        snprintf(test_filename, sizeof(test_filename), MOUNT_POINT "/sync%d.csv", i);
        if (stat(test_filename, &st) != 0) {
            s_next_file_index = i;
            ESP_LOGI(TAG, "Next file: %s", test_filename);
            break;
        }
        i++;
    }
}

static void sd_card_write_task(void *pvParameters)
{
    QueueHandle_t queue = (QueueHandle_t)pvParameters;
    sync_log_event_t event;
    
    FILE *f = NULL;
    char current_filename[64] = {0};
    char line_buffer[128];
    int unsaved_count = 0;

    ESP_LOGI(TAG, "Sync Test SD Task Started. Waiting for queue...");

    while (1) {
        if (xQueueReceive(queue, &event, pdMS_TO_TICKS(SESSION_TIMEOUT_MS)) == pdTRUE) {
            ESP_LOGI(TAG, "INT!!");
            // 阻礙 1：SD 卡沒掛載成功
            if (!is_mounted) {
                ESP_LOGE(TAG, "Data dropped! SD Card is NOT mounted.");
                continue; 
            }

            // 開新檔案
            if (f == NULL) {
                snprintf(current_filename, sizeof(current_filename), MOUNT_POINT "/sync%d.csv", s_next_file_index);
                f = fopen(current_filename, "w");
                
                // 阻礙 2：檔案建立失敗
                if (f == NULL) {
                    ESP_LOGE(TAG, "Failed to create file: %s", current_filename);
                    continue;
                }

                fprintf(f, "seq_num,tv_sec,tv_usec,formatted_time\n");
                ESP_LOGI(TAG, ">>> Start Recording: %s", current_filename);
                s_next_file_index++; 
                unsaved_count = 0;
            }

            struct tm timeinfo;
            localtime_r(&event.tv.tv_sec, &timeinfo);
            
            int len = snprintf(line_buffer, sizeof(line_buffer), 
                    "%lu,%ld,%ld,%04d-%02d-%02d %02d:%02d:%02d.%06ld\n",
                    event.seq_num,
                    (long)event.tv.tv_sec, 
                    (long)event.tv.tv_usec,
                    timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                    timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec,
                    (long)event.tv.tv_usec
            );

            fwrite(line_buffer, 1, len, f);
            unsaved_count++;

            if (unsaved_count >= SYNC_INTERVAL) {
                fsync(fileno(f));
                unsaved_count = 0;
            }

        } else {
            // 逾時處理
            if (f != NULL) {
                fclose(f);
                f = NULL;
                unsaved_count = 0;
                ESP_LOGI(TAG, "<<< Timeout (3s). Saved & Closed: %s", current_filename);
            }
        }
    }
}

esp_err_t sd_card_init(QueueHandle_t packet_queue)
{
    if (is_mounted) return ESP_OK;
    if (packet_queue == NULL) return ESP_ERR_INVALID_ARG;

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
    if (ret != ESP_OK) return ret;
    is_mounted = true;

    find_next_free_index();
    xTaskCreate(sd_card_write_task, "sd_writer", 4096, (void*)packet_queue, 2, &s_sd_task_handle);
    
    return ESP_OK;
}
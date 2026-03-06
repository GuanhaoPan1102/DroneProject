#ifndef DRV_SD_CARD_H
#define DRV_SD_CARD_H

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <sys/time.h>
#include "esp_err.h"

// 專門為同步測試定義的資料結構
typedef struct {
    uint32_t seq_num;     // 按下的次數
    struct timeval tv;    // 精確到微秒的時間結構
} sync_log_event_t;

esp_err_t sd_card_init(QueueHandle_t packet_queue);
void sd_card_deinit(void);

#endif // DRV_SD_CARD_H
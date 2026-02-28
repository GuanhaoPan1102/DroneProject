#include <stdio.h>
#include "esp_log.h"
#include "driver/uart.h"
#include "sys_defs.h"

#include <time.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "drv_uart_recv.h"
#include "drv_sd_card.h"
#include "drv_espnow_receiver.h"

static const char *TAG = "MAIN";

void app_main(void)
{
	// 建立 Queue
    QueueHandle_t recv_queue = xQueueCreate(50, sizeof(ble_packet_queue_item_t));

    // 初始化其他周邊 (SD 卡, Master 通訊)
    ESP_LOGI(TAG, "Step 2: Initializing SD Card & UART...");
    
    sd_card_init(recv_queue);
    uart_init(recv_queue);
    
    return;
}

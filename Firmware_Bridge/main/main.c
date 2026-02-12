#include <stdio.h>
#include "esp_log.h"
#include "driver/uart.h"
#include "sys_defs.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "drv_gnss.h"
#include "drv_uart_recv.h"
#include "drv_sd_card.h"

void app_main(void)
{
	// 1. 建立 Queue
    QueueHandle_t recv_queue = xQueueCreate(50, sizeof(ble_packet_queue_item_t));

    // 2. 初始化 SDcard, UART 模組
    sd_card_init(recv_queue);
    uart_init(recv_queue);
    
    return;
}

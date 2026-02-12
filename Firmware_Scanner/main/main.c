#include <stdio.h>
#include "esp_log.h"
#include "driver/uart.h"
#include "sys_defs.h"
#include "drv_ble_scan.h"
#include "drv_uart_send.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

static const char *TAG = "MAIN";

void app_main(void)
{
    // Set UART log level
    esp_log_level_set("UART_SEND", ESP_LOG_INFO);
    esp_log_level_set("BLE_SCANNER", ESP_LOG_INFO);

    // 建立 BLE 和 UART 串接的 Queue
    QueueHandle_t ble_to_uart_queue = xQueueCreate(20, sizeof(ble_packet_queue_item_t));
    if (ble_to_uart_queue == NULL) {
        ESP_LOGW(TAG, "Failed to create UART Queue.");
        return;
    }

    // Uart init
    uart_init(ble_to_uart_queue);
    // BLE init
    ble_scan_init(ble_to_uart_queue);

    return;
}
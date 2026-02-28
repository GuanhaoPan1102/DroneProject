#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"

#include "sys_defs.h"
#include "drv_uart_send.h"

#define UART_PORT_NUM      UART_NUM_1
#define UART_TX_PIN        22
#define UART_RX_PIN        16
#define UART_RTS_PIN       18   // Request to Send (輸出: 告訴對方我可以收了沒)
#define UART_CTS_PIN       19   // Clear to Send   (輸入: 偵測對方是否準備好接收)
#define UART_BAUD_RATE     921600
#define UART_BUF_SIZE      1024

static const char *TAG = "UART_SEND";

static void uart_send_task(void *pvParameters) {
    // 1. 取回 Queue Handle
    QueueHandle_t data_queue = (QueueHandle_t)pvParameters;
    
    // 暫存用的變數
    ble_packet_queue_item_t item; 

    ESP_LOGI(TAG, "UART Send Task Started on Core %d", xPortGetCoreID()); // 驗證一下 Core ID

    int count = 0;

    while (1) {
        // 2. 阻塞式等待 Queue (不耗 CPU 資源)
        if (xQueueReceive(data_queue, &item, portMAX_DELAY) == pdTRUE) {
            
            // 3. 寫入 UART (硬體流控會自動管理 CTS)
            // 將整個結構體 (Payload + RSSI) 一次發送出去
            int len = uart_write_bytes(UART_PORT_NUM, (const char*)&item, sizeof(item));
            
            if (len < 0) {
                ESP_LOGE(TAG, "UART write failed");
            } 
            // 測試初期可以開這行 Log 確認有在送，穩定後請註解掉以免拖慢速度
            //else {
            //    count++;
            //    ESP_LOGI(TAG, "Sent packet, RSSI: %d, num: %d", item.rssi, count);
            //}
        }
    }
}

void uart_init(QueueHandle_t data_queue)
{
	const uart_config_t uart_config = {
        .baud_rate = UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        
        // 開啟 CTS/RTS 硬體流控
        .flow_ctrl = UART_HW_FLOWCTRL_CTS_RTS, 
		//.flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 122, // 當 RX FIFO 滿到 122 bytes 時，自動拉高 RTS
        
        .source_clk = UART_SCLK_APB,
    };

    // 2. 套用設定
    ESP_ERROR_CHECK(uart_param_config(UART_PORT_NUM, &uart_config));

    // 3. 設定腳位 (TX, RX, RTS, CTS)
    ESP_ERROR_CHECK(uart_set_pin(UART_PORT_NUM, 
                                 UART_TX_PIN, 
                                 UART_RX_PIN, 
                                 UART_RTS_PIN, 
                                 UART_CTS_PIN));

    // 4. 安裝驅動
    // RX Buffer=1024, TX Buffer=0 (Blocking Mode), No Event Queue
    ESP_ERROR_CHECK(uart_driver_install(UART_PORT_NUM, UART_BUF_SIZE, 0, 0, NULL, 0));

    ESP_LOGI(TAG, "UART Initialized (921600 baud, CTS/RTS enabled)");

    // 5. 啟動發送任務
    xTaskCreatePinnedToCore(
        uart_send_task,       // Task 函式
        "uart_tx_task",       // Task 名稱
        4096,                 // Stack 大小
        (void *)data_queue,   // 參數 (Queue Handle)
        5,                    // 優先級 (5 比 IDLE 高即可)
        NULL,                 // Task Handle
        1                     // 指定 Core 1
    );
}

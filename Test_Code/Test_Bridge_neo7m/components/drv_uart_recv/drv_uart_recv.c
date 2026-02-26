#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"

#include "sys_defs.h"
#include "drv_uart_recv.h"

#define UART_PORT_NUM      UART_NUM_1
#define UART_TX_PIN        27
#define UART_RX_PIN        22
#define UART_RTS_PIN       33   // Request to Send (輸出: 告訴對方我可以收了沒)
#define UART_CTS_PIN       25   // Clear to Send   (輸入: 偵測對方是否準備好接收)
#define UART_BAUD_RATE     921600
#define UART_BUF_SIZE      1024

static const char *TAG = "UART_RECV";

static void uart_recv_task(void *pvParameters) {
    QueueHandle_t out_queue = (QueueHandle_t)pvParameters;
    
    // 暫存用的變數
    ble_packet_queue_item_t item;
    
    // 預期收到的長度
    const int expected_len = sizeof(ble_packet_queue_item_t);

    ESP_LOGI(TAG, "UART Recv Task Started on Core %d", xPortGetCoreID());

    while (1) {
        // 1. 從 UART 讀取資料
        // 參數: Port, Buffer, Length, Timeout
        // 這裡設定 Timeout 為 20ms (也就是如果在 20ms 內沒收到足夠資料就返回)
        int len = uart_read_bytes(UART_PORT_NUM, &item, expected_len, 20 / portTICK_PERIOD_MS);

        // 2. 檢查是否收到完整的封包
        if (len > 0) {
            if (len == expected_len) {
                // 收到完整封包，放入 Queue
                if (xQueueSend(out_queue, &item, 0) != pdTRUE) {
                    ESP_LOGW(TAG, "Queue full! Dropping packet.");
                } else {
                    // 測試用：每收 100 包印一次，避免刷屏
                    // static int count = 0;
                    // if (count++ % 100 == 0) ESP_LOGI(TAG, "Recv RSSI: %d", item.rssi);
                }
            } else {
                // 如果長度不對 (例如雜訊或不同步)，這是一個潛在風險
                // 簡單做法：只印錯誤 Log。進階做法：實作 Header 檢查機制。
                ESP_LOGW(TAG, "Partial packet received: %d bytes (Expected: %d)", len, expected_len);
                
                // 清空緩衝區以免錯位 (Flush)
                uart_flush_input(UART_PORT_NUM);
            }
        }
        // 如果 len == 0，代表沒資料，Task 會自動繼續下一次迴圈 (或是被 Timeout block 住)
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
        uart_recv_task,       // Task 函式
        "uart_rx_task",       // Task 名稱
        4096,                 // Stack 大小
        (void *)data_queue,   // 參數 (Queue Handle)
        5,                    // 優先級 (5 比 IDLE 高即可)
        NULL,                 // Task Handle
        1                     // 指定 Core 1
    );
}

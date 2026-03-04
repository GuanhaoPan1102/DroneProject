#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "drv_4G_module.h"

static const char *TAG = "4G_MODULE";

esp_err_t drv_4g_init(void)
{
    const uart_config_t uart_config = {
        .baud_rate = UART_4G_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_APB,
    };

    ESP_ERROR_CHECK(uart_param_config(UART_4G_PORT_NUM, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(UART_4G_PORT_NUM, UART_4G_TX_PIN, UART_4G_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(UART_4G_PORT_NUM, UART_4G_BUF_SIZE, UART_4G_BUF_SIZE, 0, NULL, 0));

    ESP_LOGI(TAG, "4G Module UART Initialized on Port %d (TX:%d, RX:%d)", UART_4G_PORT_NUM, UART_4G_TX_PIN, UART_4G_RX_PIN);
    return ESP_OK;
}

esp_err_t drv_4g_send_at_cmd(const char *cmd, const char *expected_response, uint32_t timeout_ms, char *out_response, size_t out_max_len)
{
    // 1. 核心防禦：發送前，清空 RX 緩衝區所有的殘留垃圾資料
    uart_flush_input(UART_4G_PORT_NUM);

    // 2. 發送指令
    if (cmd != NULL) {
        uart_write_bytes(UART_4G_PORT_NUM, cmd, strlen(cmd));
        ESP_LOGI(TAG, "Sent: %s", cmd);
    }

    // 3. 核心防禦：全域靜態 Buffer，絕對不會 Stack Overflow
    static char rx_buffer[2048]; 
    memset(rx_buffer, 0, sizeof(rx_buffer));
    
    int rx_pos = 0;
    TickType_t start_tick = xTaskGetTickCount();
    TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);

    while ((xTaskGetTickCount() - start_tick) < timeout_ticks) {
        
        // 餵狗保平安
        // esp_task_wdt_reset(); 

        int len = uart_read_bytes(UART_4G_PORT_NUM, (uint8_t*)&rx_buffer[rx_pos], sizeof(rx_buffer) - rx_pos - 1, pdMS_TO_TICKS(10));
        
        if (len > 0) {
            // 微小延遲，讓 UART 管線裡的連續字串抵達，避免字串被切碎
            vTaskDelay(pdMS_TO_TICKS(20));
            int more_len = uart_read_bytes(UART_4G_PORT_NUM, (uint8_t*)&rx_buffer[rx_pos + len], sizeof(rx_buffer) - rx_pos - len - 1, 0);
            len += more_len;

            rx_pos += len;
            rx_buffer[rx_pos] = '\0'; 

            // 🌟 檢查是否包含「預期回覆」
            if (expected_response != NULL && strstr(rx_buffer, expected_response) != NULL) {
                if (out_response != NULL && out_max_len > 0) {
                    strncpy(out_response, rx_buffer, out_max_len - 1);
                    out_response[out_max_len - 1] = '\0';
                }
                return ESP_OK; // 看到關鍵字，瞬間放行！
            }
        }
    }

    ESP_LOGE(TAG, "Command Timeout! Expected '%s', but got: \n%s", expected_response, rx_buffer);
    return ESP_ERR_TIMEOUT;
}

esp_err_t drv_4g_gnss_power(bool enable)
{
    const char *cmd = enable ? "AT+MGPSC=1\r\n" : "AT+MGPSC=0\r\n";
    return drv_4g_send_at_cmd(cmd, "OK", 3000, NULL, 0);
}

esp_err_t drv_4g_set_apn(void)
{
    return drv_4g_send_at_cmd("AT+QICSGP=1,1,\"internet\",\"\",\"\"\r\n", "OK", 3000, NULL, 0);
}

esp_err_t drv_4g_tcp_connect(const char *ip, int port)
{
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "AT+TCPNUM=1,\"%s\",%d\r\n", ip, port);
    
    // 根據你的 Log，連線成功會回覆 +CIPOPEN: SUCCESS,1
    return drv_4g_send_at_cmd(cmd, "+CIPOPEN: SUCCESS,1", 8000, NULL, 0);
}

esp_err_t drv_4g_tcp_send(const uint8_t *data, size_t len)
{
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "AT+CIPSEND=1,%d\r\n", len);
    
    // 步驟 1: 發送長度宣告，預期收到 '>' 符號
    esp_err_t err = drv_4g_send_at_cmd(cmd, ">", 3000, NULL, 0);
    if (err != ESP_OK) return err;

    // 步驟 2: 收到 '>' 後，立刻把真實資料灌進去
    uart_write_bytes(UART_4G_PORT_NUM, (const char*)data, len);

    // 步驟 3: 預期模組送完會回傳 +CIPSEND:SUCCESS
    return drv_4g_send_at_cmd(NULL, "+CIPSEND:SUCCESS", 5000, NULL, 0); 
}

esp_err_t drv_4g_tcp_close(void)
{
    // 根據你的 Log，關閉成功會回覆 +CIPCLOSE: SUCCESS,1
    return drv_4g_send_at_cmd("AT+CIPCLOSE=1\r\n", "+CIPCLOSE: SUCCESS,1", 3000, NULL, 0);
}

esp_err_t drv_4g_start_nmea_stream(void)
{
    // 預期模組會回傳 OK
    return drv_4g_send_at_cmd("AT+MGPSGET=ALL,1\r\n", "OK", 3000, NULL, 0);
}

esp_err_t drv_4g_stop_nmea_stream(void)
{
    // 預期模組會回傳 OK
    return drv_4g_send_at_cmd("AT+MGPSGET=ALL,0\r\n", "OK", 3000, NULL, 0);
}

void drv_4g_listen_and_print(uint32_t duration_ms)
{
    TickType_t start_tick = xTaskGetTickCount();
    TickType_t duration_ticks = pdMS_TO_TICKS(duration_ms);
    
    // 準備一個小 Buffer 來接連續的水流
    char stream_buf[512]; 
    uart_flush_input(UART_4G_PORT_NUM);
    while ((xTaskGetTickCount() - start_tick) < duration_ticks) {
        // 餵狗，因為會在這裡卡 5 秒，一定要處理看門狗
        // esp_task_wdt_reset(); 

        // 每次最多等 100ms 收資料
        int len = uart_read_bytes(UART_4G_PORT_NUM, (uint8_t*)stream_buf, sizeof(stream_buf) - 1, pdMS_TO_TICKS(100));
        
        if (len > 0) {
            stream_buf[len] = '\0'; // 補上結尾
            // 這裡故意用 printf 而不是 ESP_LOGI，
            // 這樣印出來的 NMEA 格式才會是最原始乾淨的，不會被系統標籤干擾
            printf("%s", stream_buf); 
        }
    }
}
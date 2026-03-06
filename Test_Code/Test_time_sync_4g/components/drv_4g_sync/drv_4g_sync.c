#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_log.h"

// 引入改名後的標頭檔
#include "drv_4g_sync.h"

static const char *TAG = "4G_SYNC";

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
    uart_flush_input(UART_4G_PORT_NUM);

    if (cmd != NULL) {
        uart_write_bytes(UART_4G_PORT_NUM, cmd, strlen(cmd));
        ESP_LOGI(TAG, "Sent: %s", cmd);
    }

    static char rx_buffer[2048]; 
    memset(rx_buffer, 0, sizeof(rx_buffer));
    
    int rx_pos = 0;
    TickType_t start_tick = xTaskGetTickCount();
    TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);

    while ((xTaskGetTickCount() - start_tick) < timeout_ticks) {
        int len = uart_read_bytes(UART_4G_PORT_NUM, (uint8_t*)&rx_buffer[rx_pos], sizeof(rx_buffer) - rx_pos - 1, pdMS_TO_TICKS(10));
        
        if (len > 0) {
            vTaskDelay(pdMS_TO_TICKS(20));
            int more_len = uart_read_bytes(UART_4G_PORT_NUM, (uint8_t*)&rx_buffer[rx_pos + len], sizeof(rx_buffer) - rx_pos - len - 1, 0);
            len += more_len;

            rx_pos += len;
            rx_buffer[rx_pos] = '\0'; 

            if (expected_response != NULL && strstr(rx_buffer, expected_response) != NULL) {
                if (out_response != NULL && out_max_len > 0) {
                    strncpy(out_response, rx_buffer, out_max_len - 1);
                    out_response[out_max_len - 1] = '\0';
                }
                return ESP_OK;
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
    return drv_4g_send_at_cmd(cmd, "+CIPOPEN: SUCCESS,1", 8000, NULL, 0);
}

esp_err_t drv_4g_tcp_send(const uint8_t *data, size_t len)
{
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "AT+CIPSEND=1,%d\r\n", len);
    esp_err_t err = drv_4g_send_at_cmd(cmd, ">", 3000, NULL, 0);
    if (err != ESP_OK) return err;

    uart_write_bytes(UART_4G_PORT_NUM, (const char*)data, len);
    return drv_4g_send_at_cmd(NULL, "+CIPSEND:SUCCESS", 5000, NULL, 0); 
}

esp_err_t drv_4g_tcp_close(void)
{
    return drv_4g_send_at_cmd("AT+CIPCLOSE=1\r\n", "+CIPCLOSE: SUCCESS,1", 3000, NULL, 0);
}

esp_err_t drv_4g_start_nmea_stream(void)
{
    return drv_4g_send_at_cmd("AT+MGPSGET=ALL,1\r\n", "OK", 3000, NULL, 0);
}

esp_err_t drv_4g_stop_nmea_stream(void)
{
    return drv_4g_send_at_cmd("AT+MGPSGET=ALL,0\r\n", "OK", 3000, NULL, 0);
}

void drv_4g_listen_and_print(uint32_t duration_ms)
{
    TickType_t start_tick = xTaskGetTickCount();
    TickType_t duration_ticks = pdMS_TO_TICKS(duration_ms);
    char stream_buf[512]; 
    
    uart_flush_input(UART_4G_PORT_NUM);
    while ((xTaskGetTickCount() - start_tick) < duration_ticks) {
        int len = uart_read_bytes(UART_4G_PORT_NUM, (uint8_t*)stream_buf, sizeof(stream_buf) - 1, pdMS_TO_TICKS(100));
        if (len > 0) {
            stream_buf[len] = '\0';
            printf("%s", stream_buf); 
        }
    }
}

// === 新增：解析 NMEA 並校時 ===
esp_err_t drv_4g_sync_time_from_gps(uint32_t timeout_ms)
{
    ESP_LOGI(TAG, "開始監聽 4G 模組 GPS 進行時間同步 (Timeout: %lu ms)...", timeout_ms);
    
    drv_4g_start_nmea_stream();

    char rx_buffer[256];
    int rx_pos = 0;
    TickType_t start_tick = xTaskGetTickCount();
    TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);
    bool is_synced = false;

    uart_flush_input(UART_4G_PORT_NUM);

    while ((xTaskGetTickCount() - start_tick) < timeout_ticks) {
        uint8_t c;
        int len = uart_read_bytes(UART_4G_PORT_NUM, &c, 1, pdMS_TO_TICKS(10));
        
        if (len > 0) {
            if (c == '\n' || rx_pos >= sizeof(rx_buffer) - 1) {
                rx_buffer[rx_pos] = '\0'; 
                
                if (strncmp(rx_buffer, "$GNRMC", 6) == 0 || strncmp(rx_buffer, "$GPRMC", 6) == 0) {
                    
                    char *tokens[15];
                    int token_count = 0;
                    char *rest = rx_buffer;
                    char *token;
                    
                    while ((token = strsep(&rest, ",")) != NULL && token_count < 15) {
                        tokens[token_count++] = token;
                    }
                    
                    if (token_count > 9 && strlen(tokens[2]) > 0 && tokens[2][0] == 'A') {
                        int raw_time = atoi(tokens[1]);
                        int raw_date = atoi(tokens[9]);
                        
                        struct tm tm_struct = {0};
                        tm_struct.tm_hour = raw_time / 10000;
                        tm_struct.tm_min  = (raw_time % 10000) / 100;
                        tm_struct.tm_sec  = raw_time % 100;
                        tm_struct.tm_mday = raw_date / 10000;
                        tm_struct.tm_mon  = ((raw_date % 10000) / 100) - 1; 
                        tm_struct.tm_year = (raw_date % 100) + 100;

                        time_t current_gps_time = mktime(&tm_struct);
                        
                        struct timeval tv = { .tv_sec = current_gps_time, .tv_usec = 0 };
                        settimeofday(&tv, NULL);
                        
                        ESP_LOGI(TAG, "✅ 時間同步成功！(UTC: %04d-%02d-%02d %02d:%02d:%02d)",
                                 tm_struct.tm_year + 1900, tm_struct.tm_mon + 1, tm_struct.tm_mday,
                                 tm_struct.tm_hour, tm_struct.tm_min, tm_struct.tm_sec);
                        is_synced = true;
                        break; 
                    }
                }
                rx_pos = 0; 
            } else if (c != '\r') {
                rx_buffer[rx_pos++] = (char)c;
            }
        }
    }

    drv_4g_stop_nmea_stream();
    return is_synced ? ESP_OK : ESP_ERR_TIMEOUT;
}
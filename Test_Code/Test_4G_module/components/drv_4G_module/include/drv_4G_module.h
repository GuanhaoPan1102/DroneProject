#ifndef DRV_4G_MODULE_H
#define DRV_4G_MODULE_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

// ==========================================
// 硬體腳位與參數設定 (已更新為 TX:23, RX:18)
// ==========================================
#define UART_4G_PORT_NUM      UART_NUM_2 
#define UART_4G_TX_PIN        23  
#define UART_4G_RX_PIN        18  
#define UART_4G_BAUD_RATE     115200
#define UART_4G_BUF_SIZE      2048

// ==========================================
// 外部呼叫 API 宣告
// ==========================================

/**
 * @brief 初始化 4G 模組 UART
 */
esp_err_t drv_4g_init(void);

/**
 * @brief 發送 AT 指令並等待預期的字串回覆
 * @param cmd 完整的 AT 指令 (例如 "AT\r\n")
 * @param expected_response 預期收到的字串 (例如 "OK" 或 ">")
 * @param timeout_ms 超時時間 (毫秒)
 * @param out_response (可選) 存放模組完整回傳字串的 Buffer
 * @param out_max_len out_response 的最大長度
 * @return esp_err_t ESP_OK 表示成功匹配，ESP_ERR_TIMEOUT 表示超時
 */
esp_err_t drv_4g_send_at_cmd(const char *cmd, const char *expected_response, uint32_t timeout_ms, char *out_response, size_t out_max_len);

/**
 * @brief 開啟或關閉 GPS 模組電源
 */
esp_err_t drv_4g_gnss_power(bool enable);

/**
 * @brief 設定 APN 註冊網路 (預設 internet)
 */
esp_err_t drv_4g_set_apn(void);

/**
 * @brief 建立 TCP 單一連線 (對應 AT+TCPNUM=1)
 */
esp_err_t drv_4g_tcp_connect(const char *ip, int port);

/**
 * @brief 透過 TCP 發送定長資料 (對應 AT+CIPSEND=1)
 */
esp_err_t drv_4g_tcp_send(const uint8_t *data, size_t len);

/**
 * @brief 關閉 TCP 連線 (對應 AT+CIPCLOSE=1)
 */
esp_err_t drv_4g_tcp_close(void);

/**
 * @brief 開啟 NMEA 持續更新 (AT+MGPSGET=ALL,1)
 */
esp_err_t drv_4g_start_nmea_stream(void);

/**
 * @brief 關閉 NMEA 持續更新 (AT+MGPSGET=ALL,0)
 */
esp_err_t drv_4g_stop_nmea_stream(void);

/**
 * @brief 純粹監聽 UART 並將收到的資料原封不動印出 (不會報錯、不會提早跳出)
 * @param duration_ms 監聽的時間 (毫秒)
 */

void drv_4g_listen_and_print(uint32_t duration_ms);

#endif // DRV_4G_MODULE_H 
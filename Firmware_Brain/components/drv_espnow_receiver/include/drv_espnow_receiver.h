#ifndef DRV_ESPNOW_RECEIVER_H
#define DRV_ESPNOW_RECEIVER_H

#include "esp_err.h"
#include "sys_defs.h" // 引入共用的 Payload 結構

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 ESP-NOW 接收端 (Master 使用)
 * 內建 Wi-Fi Station 模式與 Long Range (LR) 協定開啟
 * @return esp_err_t ESP_OK 代表成功
 */
esp_err_t espnow_receiver_init(void);

#ifdef __cplusplus
}
#endif

#endif // ESPNOW_RECEIVER_H
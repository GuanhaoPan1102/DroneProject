#ifndef DRV_ESPNOW_SENDER_H
#define DRV_ESPNOW_SENDER_H

#include "esp_err.h"
#include "sys_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 ESP-NOW 發送端 (Slave 使用)
 * * @param master_mac 目標 Master 節點的 6-byte MAC Address
 * @return esp_err_t ESP_OK 代表成功
 */
esp_err_t espnow_sender_init(const uint8_t *master_mac);

/**
 * @brief 發送 ESP-NOW 封包給 Master
 * * @param payload 要發送的資料結構 (包含 msg_type 與對應的 data)
 * @return esp_err_t ESP_OK 代表成功送進發送佇列
 */
esp_err_t espnow_sender_send(espnow_payload_t *payload);

#ifdef __cplusplus
}
#endif

#endif // ESPNOW_SENDER_H
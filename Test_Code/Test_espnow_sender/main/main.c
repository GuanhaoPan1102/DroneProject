#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "sys/time.h"

// 引入你剛剛寫好的發送元件
#include "drv_espnow_sender.h"

static const char *TAG = "SLAVE_TEST";

// 定義這個 Slave 的名稱代號
#define MY_NODE_ID 1

// ⚠️ 【請填寫這裡】 你的 Master 節點 MAC Address
static const uint8_t MASTER_MAC[6] = {0x2C, 0xBC, 0xBB, 0xA8, 0x49, 0x78}; 

void app_main(void)
{
    ESP_LOGI(TAG, "Slave 節點開機，準備初始化...");

    // 1. 初始化 NVS (Wi-Fi 和 ESP-NOW 啟動的先決條件)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 2. 初始化 ESP-NOW 發送端 (記得確認裡面已經加了 LR 模式的程式碼)
    ESP_ERROR_CHECK(espnow_sender_init(MASTER_MAC));

    // ==========================================
    // 步驟一：等待 5 秒後，發送 [註冊通知]
    // ==========================================
    ESP_LOGI(TAG, "等待 5 秒後發送註冊封包...");
    vTaskDelay(pdMS_TO_TICKS(5000));

    espnow_payload_t reg_msg = {0};
    reg_msg.msg_type = MSG_TYPE_REGISTER;
    reg_msg.node_id  = MY_NODE_ID;
    
    // 塞入假的 GPS 座標，讓 Master 畫面印出來好看一點
    // 假設位於嘉義民雄附近 (23.55°N, 120.43°E)
    reg_msg.data.reg.lat = 235500000;  
    reg_msg.data.reg.lon = 1204300000;
    reg_msg.data.reg.alt = 50.5;

    esp_err_t send_res = espnow_sender_send(&reg_msg);
    if (send_res == ESP_OK) {
        ESP_LOGI(TAG, "-> 註冊封包發送成功！");
    } else {
        ESP_LOGE(TAG, "-> 註冊封包發送失敗！");
    }

    // ==========================================
    // 步驟二：再等待 5 秒後，發送 [存檔回報]
    // ==========================================
    ESP_LOGI(TAG, "等待 5 秒後發送存檔回報封包...");
    vTaskDelay(pdMS_TO_TICKS(5000));

    espnow_payload_t file_msg = {0};
    file_msg.msg_type = MSG_TYPE_FILE_SAVED;
    file_msg.node_id  = MY_NODE_ID;
    
    // 塞入假的 UTC 時間戳與你指定的檔名
    file_msg.data.file.timestamp = 1772000000000; // 隨便給個假時間戳
    snprintf(file_msg.data.file.filename, sizeof(file_msg.data.file.filename), "Data1.txt");

    send_res = espnow_sender_send(&file_msg);
    if (send_res == ESP_OK) {
        ESP_LOGI(TAG, "-> 存檔回報發送成功！檔名: %s", file_msg.data.file.filename);
    } else {
        ESP_LOGE(TAG, "-> 存檔回報發送失敗！");
    }

    // ==========================================
    // 結束測試，進入休眠或閒置
    // ==========================================
    ESP_LOGI(TAG, "測試發送流程完畢，進入閒置狀態。");
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
#include "drv_espnow_sender.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include <string.h>

static const char *TAG = "ESPNOW_TX";

// 儲存 Master 的 MAC Address，供發送時使用
static uint8_t s_master_mac[6];

// 發送結果的 Callback 函式
static void espnow_send_cb(const uint8_t *mac_addr, esp_now_send_status_t status) 
{
    if (status == ESP_NOW_SEND_SUCCESS) {
        ESP_LOGD(TAG, "Send Success"); // 設為 Debug 層級，避免洗版
    } else {
        ESP_LOGE(TAG, "Send Failed");
    }
}

esp_err_t espnow_sender_init(const uint8_t *master_mac) 
{
    if (master_mac == NULL) {
        ESP_LOGE(TAG, "Master MAC address is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    // 複製 Master MAC 到本地靜態變數
    memcpy(s_master_mac, master_mac, 6);

    // 1. 初始化底層網路與 Wi-Fi
    // 必須將 Wi-Fi 設為 Station 模式才能使用 ESP-NOW
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    
    // 將儲存模式設為 RAM，避免頻繁寫入 Flash 導致壽命減損
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    // 2. 初始化 ESP-NOW
    esp_err_t ret = esp_now_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error initializing ESP-NOW");
        return ret;
    }

    // 3. 註冊發送 Callback
    ESP_ERROR_CHECK(esp_now_register_send_cb((esp_now_send_cb_t)espnow_send_cb));

    // 4. 加入 Master 節點作為 Peer
    esp_now_peer_info_t peerInfo = {0};
    memcpy(peerInfo.peer_addr, s_master_mac, 6);
    peerInfo.channel = 0;      // 0 代表跟隨當前 Wi-Fi 的頻道
    peerInfo.encrypt = false;  // 為了傳輸速度與降低延遲，先不加密
    
    // 如果 Peer 不存在才加入
    if (!esp_now_is_peer_exist(s_master_mac)) {
        ret = esp_now_add_peer(&peerInfo);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to add peer");
            return ret;
        }
    }

    ESP_LOGI(TAG, "ESP-NOW Sender Initialized. Target MAC: %02x:%02x:%02x:%02x:%02x:%02x",
             s_master_mac[0], s_master_mac[1], s_master_mac[2], 
             s_master_mac[3], s_master_mac[4], s_master_mac[5]);

    return ESP_OK;
}

esp_err_t espnow_sender_send(espnow_payload_t *payload) 
{
    if (payload == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    // 將自定義的 payload 結構轉型為 uint8_t 陣列並發送
    esp_err_t ret = esp_now_send(s_master_mac, (const uint8_t *)payload, sizeof(espnow_payload_t));
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error sending data: %s", esp_err_to_name(ret));
    }
    
    return ret;
}
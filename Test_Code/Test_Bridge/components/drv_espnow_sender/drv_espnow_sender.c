#include "drv_espnow_sender.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include <string.h>

// 引入 FreeRTOS 同步元件
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

static const char *TAG = "ESPNOW_TX";

static uint8_t s_master_mac[6];

// 定義事件群組與旗標
static EventGroupHandle_t s_evt_group;
#define SEND_SUCCESS_BIT BIT0
#define SEND_FAIL_BIT    BIT1

// 發送結果的 Callback 函式
static void espnow_send_cb(const uint8_t *mac_addr, esp_now_send_status_t status) 
{
    if (status == ESP_NOW_SEND_SUCCESS) {
        xEventGroupSetBits(s_evt_group, SEND_SUCCESS_BIT); // 通知主程式：發送成功
        ESP_LOGD(TAG, "底層 ACK: 收到"); 
    } else {
        xEventGroupSetBits(s_evt_group, SEND_FAIL_BIT);    // 通知主程式：發送失敗
        ESP_LOGE(TAG, "底層 ACK: 遺失 (目標未開機或距離太遠)");
    }
}

esp_err_t espnow_sender_init(const uint8_t *master_mac) 
{
    if (master_mac == NULL) {
        ESP_LOGE(TAG, "Master MAC address is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    memcpy(s_master_mac, master_mac, 6);

    // 建立 FreeRTOS 事件群組
    s_evt_group = xEventGroupCreate();

    // 1. 初始化底層網路與 Wi-Fi
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_LR)); // 確保開了 LR
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE));

    // 取得並印出本機 (Slave) 的 MAC Address
    uint8_t my_mac[6];
    esp_read_mac(my_mac, ESP_MAC_WIFI_STA);
    ESP_LOGI(TAG, "==================================================");
    ESP_LOGI(TAG, "[Sender/Slave] 本機 MAC Address: 0x%02X, 0x%02X, 0x%02X, 0x%02X, 0x%02X, 0x%02X", 
             my_mac[0], my_mac[1], my_mac[2], my_mac[3], my_mac[4], my_mac[5]);
    ESP_LOGI(TAG, "==================================================");

    // 2. 初始化 ESP-NOW
    esp_err_t ret = esp_now_init();
    if (ret != ESP_OK) return ret;

    // 3. 註冊發送 Callback
    ESP_ERROR_CHECK(esp_now_register_send_cb((esp_now_send_cb_t)espnow_send_cb));

    // 4. 加入 Master 節點作為 Peer
    esp_now_peer_info_t peerInfo = {0};
    memcpy(peerInfo.peer_addr, s_master_mac, 6);
    peerInfo.channel = 0;      
    peerInfo.encrypt = false;  
    
    if (!esp_now_is_peer_exist(s_master_mac)) {
        ESP_ERROR_CHECK(esp_now_add_peer(&peerInfo));
    }

    return ESP_OK;
}

esp_err_t espnow_sender_send(espnow_payload_t *payload) 
{
    if (payload == NULL) return ESP_ERR_INVALID_ARG;

    // 發送前，先清空舊的事件旗標
    xEventGroupClearBits(s_evt_group, SEND_SUCCESS_BIT | SEND_FAIL_BIT);

    // 把資料塞進 Wi-Fi 佇列
    esp_err_t ret = esp_now_send(s_master_mac, (const uint8_t *)payload, sizeof(espnow_payload_t));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "無法塞入發送佇列: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // ==========================================
    // 關鍵：讓程式停在這裡等！
    // 等待 Callback 告訴我們到底有沒有成功，最多等 500 毫秒
    // ==========================================
    EventBits_t bits = xEventGroupWaitBits(
        s_evt_group, 
        SEND_SUCCESS_BIT | SEND_FAIL_BIT, 
        pdTRUE,  // 等到之後自動清除旗標
        pdFALSE, // 滿足其中一個條件就放行
        pdMS_TO_TICKS(500) // Timeout 500ms
    );

    // 根據 Callback 的結果回傳真正的狀態
    if (bits & SEND_SUCCESS_BIT) {
        return ESP_OK;   // 對方真的收到了
    } else {
        return ESP_FAIL; // 對方沒收到，或超時
    }
}
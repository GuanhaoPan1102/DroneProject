#include "drv_espnow_receiver.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "esp_netif.h"
#include <string.h>
#include <time.h>

static const char *TAG = "ESPNOW_RX";

// ESP-IDF v5.x 專屬的接收 Callback 簽名
// 第一個參數變成 esp_now_recv_info_t 結構，裡面包含了發送者的 MAC 與頻道資訊
static void espnow_recv_cb(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len) 
{
    if (data == NULL || len != sizeof(espnow_payload_t)) {
        ESP_LOGW(TAG, "收到未知長度的封包 (Len: %d)，已丟棄。", len);
        return;
    }

    // 將收到的 Byte 陣列強制轉型為我們的結構體
    espnow_payload_t *payload = (espnow_payload_t *)data;

    // 擷取發送者 (Slave) 的 MAC Address 方便辨識
    const uint8_t *mac = recv_info->src_addr;
    
    // 依照訊息類型進行解析
    switch (payload->msg_type) {
        
        case MSG_TYPE_REGISTER:
            ESP_LOGI(TAG, "=========================================");
            ESP_LOGI(TAG, "[註冊通知] 來自 Slave 節點 ID: %d (MAC: %02X:%02X:%02X:%02X:%02X:%02X)", 
                     payload->node_id, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
            // 將整數縮放的經緯度還原為浮點數印出 (除以 10^7)
            ESP_LOGI(TAG, " -> 鎖定座標: Lat %.7f, Lon %.7f", 
                     payload->data.reg.lat / 10000000.0, 
                     payload->data.reg.lon / 10000000.0);
            ESP_LOGI(TAG, " -> 海拔高度: %.2f m", payload->data.reg.alt);

            time_t slave_sec = payload->data.reg.timestamp / 1000;
            time_t slave_tw_sec = slave_sec + (8 * 3600); // 加上台灣時區 (+8) 以方便閱讀
            struct tm timeinfo;
            gmtime_r(&slave_tw_sec, &timeinfo);
            char time_str[64];
            strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &timeinfo);
            
            ESP_LOGI(TAG, " -> 系統時間: %lld ms [%s]", payload->data.reg.timestamp, time_str);
            ESP_LOGI(TAG, "=========================================");
            break;

        case MSG_TYPE_FILE_SAVED:
            ESP_LOGI(TAG, "-----------------------------------------");
            ESP_LOGI(TAG, "[存檔回報] 來自 Slave 節點 ID: %d", payload->node_id);
            ESP_LOGI(TAG, " -> 儲存檔名: %s", payload->data.file.filename);
            ESP_LOGI(TAG, " -> 寫入時間戳 (UTC): %lld", payload->data.file.timestamp);
            ESP_LOGI(TAG, "-----------------------------------------");
            break;

        case MSG_TYPE_BLE_DATA:
            ESP_LOGI(TAG, "[即時資料] 收到 Slave %d 傳來的 BLE 數據 (功能預留)", payload->node_id);
            break;

        default:
            ESP_LOGW(TAG, "收到未知的訊息類型: %d", payload->msg_type);
            break;
    }
}

esp_err_t espnow_receiver_init(void) 
{
    ESP_LOGI(TAG, "正在初始化 ESP-NOW Master 接收端...");

    // 1. 底層網路初始化
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    
    // ==========================================
    // 關鍵：開啟 Wi-Fi Long Range (LR) 模式
    // 這樣才能聽得懂 Slave 用 LR 模式打過來的超長距離封包
    // ==========================================
    ESP_ERROR_CHECK(esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_LR));

    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE));

    uint8_t my_mac[6];
    esp_read_mac(my_mac, ESP_MAC_WIFI_STA);
    ESP_LOGI(TAG, "==================================================");
    ESP_LOGI(TAG, "[Receiver/Master] 本機 MAC Address: 0x%02X, 0x%02X, 0x%02X, 0x%02X, 0x%02X, 0x%02X", 
             my_mac[0], my_mac[1], my_mac[2], my_mac[3], my_mac[4], my_mac[5]);
    ESP_LOGI(TAG, " 請將上方這組 MAC 填入所有 Sender 的 MASTER_MAC 中！");
    ESP_LOGI(TAG, "==================================================");

    // 2. 初始化 ESP-NOW
    esp_err_t ret = esp_now_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ESP-NOW 初始化失敗");
        return ret;
    }

    // 3. 註冊接收 Callback
    // 在 v5.x 中，esp_now_register_recv_cb 要求的參數就是我們寫的那個簽名，直接放入即可
    ESP_ERROR_CHECK(esp_now_register_recv_cb(espnow_recv_cb));

    ESP_LOGI(TAG, "Master 接收端初始化完成，正在監聽 LR 頻道...");

    return ESP_OK;
}
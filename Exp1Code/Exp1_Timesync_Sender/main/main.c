#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_now.h"

static const char *TAG = "MASTER_TX";

// ==========================================
// 🚀 燒錄前必改：設定這塊板子的專屬名稱！
// ==========================================
// 燒錄第一塊請用 "S1"，第二塊請改 "S2"，第三塊改 "S3"
#define NODE_ID "S0"  

// 雙方必須完全一致的通訊結構體
typedef struct {
    char     node_name[4]; // 存放 "S1", "S2", "S3" (包含字尾 \0)
    uint32_t seq_num;
} __attribute__((packed)) sync_test_packet_t;

// 目標 MAC 地址 (目前維持廣播，若要單播扛壓測試，請換成 Receiver 的 MAC)
static const uint8_t target_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

void app_main(void)
{
    ESP_LOGI(TAG, "初始化發射端節點: %s ...", NODE_ID);

    // 1. 基礎初始化
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    
    // 開啟 LR 模式以增加傳輸穩定性
    ESP_ERROR_CHECK(esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_LR));
    ESP_ERROR_CHECK(esp_wifi_start());

    // 強制將 Wi-Fi 鎖定在頻道 1
    ESP_ERROR_CHECK(esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE));

    // 2. 初始化 ESP-NOW 並加入 Peer
    ESP_ERROR_CHECK(esp_now_init());
    
    esp_now_peer_info_t peerInfo = {0};
    memcpy(peerInfo.peer_addr, target_mac, 6);
    peerInfo.channel = 0;      
    peerInfo.encrypt = false;  
    
    if (!esp_now_is_peer_exist(target_mac)) {
        ESP_ERROR_CHECK(esp_now_add_peer(&peerInfo));
    }

    ESP_LOGI(TAG, "[%s] 準備就緒, 30秒後開始以 10Hz 頻率發送測試封包...", NODE_ID);
    vTaskDelay(pdMS_TO_TICKS(30000)); 

    ESP_LOGI(TAG, ">>> [%s] 開始發送, 實驗時長: 10 分鐘 (預計發送 6000 筆) <<<", NODE_ID);

    // 3. 廣播迴圈 (初始化封包並自動寫入 NODE_ID)
    sync_test_packet_t pkt;
    memset(&pkt, 0, sizeof(pkt)); // 先清空記憶體確保安全
    strncpy(pkt.node_name, NODE_ID, 3); // 寫入 "S1", "S2" 或 "S3"
    pkt.seq_num = 1;
    
    // 設定終點：當序號小於等於 6000 時持續發送
    while (pkt.seq_num <= 6000) {
        esp_err_t ret = esp_now_send(target_mac, (const uint8_t *)&pkt, sizeof(pkt));
        
        if (ret == ESP_OK) {
            // 每 100 筆印一次 Log，避免洗畫面
            if (pkt.seq_num % 100 == 0) {
                //ESP_LOGI(TAG, "[%s] 已發送 Seq: %lu / 6000", pkt.node_name, pkt.seq_num);
            }
            pkt.seq_num++;
        } else {
            ESP_LOGE(TAG, "第一周期 : [%s] 發送失敗: %s", pkt.node_name, esp_err_to_name(ret));
        }

        // 發送間隔：100ms (10Hz)
        vTaskDelay(pdMS_TO_TICKS(100)); 
    }

    ESP_LOGI(TAG, "[%s] 準備就緒, 10秒後開始以 5Hz 頻率發送測試封包...", NODE_ID);
    vTaskDelay(pdMS_TO_TICKS(10000)); 

    ESP_LOGI(TAG, ">>> [%s] 開始發送, 實驗時長: 10 分鐘 (預計發送 3000 筆) <<<", NODE_ID);

    memset(&pkt, 0, sizeof(pkt)); // 先清空記憶體確保安全
    strncpy(pkt.node_name, NODE_ID, 3); // 寫入 "S1", "S2" 或 "S3"
    pkt.seq_num = 1;
    
    // 設定終點：當序號小於等於 6000 時持續發送
    while (pkt.seq_num <= 3000) {
        esp_err_t ret = esp_now_send(target_mac, (const uint8_t *)&pkt, sizeof(pkt));
        
        if (ret == ESP_OK) {
            // 每 100 筆印一次 Log，避免洗畫面
            if (pkt.seq_num % 100 == 0) {
                //ESP_LOGI(TAG, "[%s] 已發送 Seq: %lu / 6000", pkt.node_name, pkt.seq_num);
            }
            pkt.seq_num++;
        } else {
            ESP_LOGE(TAG, "第二周期 : [%s] 發送失敗: %s", pkt.node_name, esp_err_to_name(ret));
        }

        // 發送間隔：200ms (5Hz)
        vTaskDelay(pdMS_TO_TICKS(200)); 
    }

    ESP_LOGI(TAG, "[%s] 準備就緒, 10秒後開始以 2Hz 頻率發送測試封包...", NODE_ID);
    vTaskDelay(pdMS_TO_TICKS(10000)); 

    ESP_LOGI(TAG, ">>> [%s] 開始發送, 實驗時長: 10 分鐘 (預計發送 1200 筆) <<<", NODE_ID);

    memset(&pkt, 0, sizeof(pkt)); // 先清空記憶體確保安全
    strncpy(pkt.node_name, NODE_ID, 3); // 寫入 "S1", "S2" 或 "S3"
    pkt.seq_num = 1;
    
    // 設定終點：當序號小於等於 6000 時持續發送
    while (pkt.seq_num <= 1200) {
        esp_err_t ret = esp_now_send(target_mac, (const uint8_t *)&pkt, sizeof(pkt));
        
        if (ret == ESP_OK) {
            // 每 100 筆印一次 Log，避免洗畫面
            if (pkt.seq_num % 100 == 0) {
                //ESP_LOGI(TAG, "[%s] 已發送 Seq: %lu / 6000", pkt.node_name, pkt.seq_num);
            }
            pkt.seq_num++;
        } else {
            ESP_LOGE(TAG, "第三周期 : [%s] 發送失敗: %s", pkt.node_name, esp_err_to_name(ret));
        }

        // 發送間隔：500ms (2Hz)
        vTaskDelay(pdMS_TO_TICKS(500)); 
    }

    // ==========================================
    // 實驗結束，進入閒置狀態
    // ==========================================
    ESP_LOGI(TAG, "====================================");
    ESP_LOGI(TAG, " [%s] 發送實驗已結束！", NODE_ID);
    ESP_LOGI(TAG, " 總共發送封包數：6000 / 3000 / 1200 筆");
    ESP_LOGI(TAG, "====================================");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000)); // 無窮休眠
    }
}
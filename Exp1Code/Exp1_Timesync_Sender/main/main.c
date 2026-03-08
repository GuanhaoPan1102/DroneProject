#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_now.h"

static const char *TAG = "MASTER_TX";

// 獨立專案專用的簡單廣播結構體
typedef struct {
    uint32_t seq_num;
} __attribute__((packed)) sync_test_packet_t;

// 廣播 MAC 地址
static const uint8_t broadcast_mac[6] = {0x2C, 0xBC, 0xBB, 0xA8, 0x48, 0xF0};

void app_main(void)
{
    ESP_LOGI(TAG, "初始化 Master 廣播發射端...");

    // 1. 基礎初始化
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    
    // 為了 20 公尺的穩定性，開啟 LR 模式
    ESP_ERROR_CHECK(esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_LR));
    ESP_ERROR_CHECK(esp_wifi_start());

    // 【安全防護】強制將 Wi-Fi 鎖定在頻道 1，避免亂跳頻道導致漏包
    ESP_ERROR_CHECK(esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE));

    // 2. 初始化 ESP-NOW 並加入廣播 Peer
    ESP_ERROR_CHECK(esp_now_init());
    
    esp_now_peer_info_t peerInfo = {0};
    memcpy(peerInfo.peer_addr, broadcast_mac, 6);
    peerInfo.channel = 0;      // 設為 0，讓它自動跟隨剛剛鎖定的頻道 1
    peerInfo.encrypt = false;  
    
    // 檢查是否已存在，再加入 peer
    if (!esp_now_is_peer_exist(broadcast_mac)) {
        ESP_ERROR_CHECK(esp_now_add_peer(&peerInfo));
    }

    ESP_LOGI(TAG, "Master 準備就緒，30秒後開始以 10Hz 頻率發送廣播測試封包...");
    vTaskDelay(pdMS_TO_TICKS(30000)); 

    ESP_LOGI(TAG, ">>> 開始廣播，實驗時長：10 分鐘 (預計發送 6000 筆) <<<");

    // 3. 廣播迴圈 (限制 6000 發)
    sync_test_packet_t pkt = { .seq_num = 1 };
    
    // 設定終點：當序號小於等於 6000 時持續發送
    while (pkt.seq_num <= 6000) {
        esp_err_t ret = esp_now_send(broadcast_mac, (const uint8_t *)&pkt, sizeof(pkt));
        
        if (ret == ESP_OK) {
            // 每 100 筆印一次 Log 就好，避免 Console 洗畫面洗太快導致 Master 稍微卡頓
            if (pkt.seq_num % 100 == 0) {
                ESP_LOGI(TAG, "已發送 Seq: %lu / 6000", pkt.seq_num);
            }
            pkt.seq_num++;
        } else {
            ESP_LOGE(TAG, "廣播失敗: %s", esp_err_to_name(ret));
        }

        // 發送間隔：100ms (10Hz)
        vTaskDelay(pdMS_TO_TICKS(100)); 
    }

    // ==========================================
    // 實驗結束，進入閒置狀態
    // ==========================================
    ESP_LOGI(TAG, "====================================");
    ESP_LOGI(TAG, " 10 分鐘廣播實驗已結束！");
    ESP_LOGI(TAG, " 總共發送封包數：6000 筆");
    ESP_LOGI(TAG, " 現在可以拔除所有節點的電源並回收 SD 卡了。");
    ESP_LOGI(TAG, "====================================");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000)); // 無窮休眠，不再發送
    }
}
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "drv_4G_module.h"

static const char *TAG = "TEST_MAIN";

// 為了接收 ATI 的模組資訊，我們還是保留一個全域 Buffer
static char rx_buf[2048];

void app_main(void)
{
    esp_err_t err;

    ESP_LOGI(TAG, "========== 4G 模組「精準預期」自動化測試啟動 ==========");

    // ==========================================
    // 闖關 1：初始化 UART
    // ==========================================
    ESP_LOGI(TAG, "\n--> Step 1: 初始化 4G UART...");
    err = drv_4g_init();
    if (err == ESP_OK) ESP_LOGI(TAG, "[PASS] UART 初始化成功");
    else ESP_LOGE(TAG, "[FAIL] UART 初始化失敗");
    vTaskDelay(pdMS_TO_TICKS(1000));

    // ==========================================
    // 闖關 2：基本通訊測試 (AT & ATI)
    // ==========================================
    ESP_LOGI(TAG, "\n--> Step 2: 測試 AT 指令與讀取模組資訊...");
    err = drv_4g_send_at_cmd("AT\r\n", "OK", 2000, NULL, 0);
    if (err == ESP_OK) ESP_LOGI(TAG, "[PASS] AT 握手成功");
    
    memset(rx_buf, 0, sizeof(rx_buf));
    err = drv_4g_send_at_cmd("ATI\r\n", "OK", 2000, rx_buf, sizeof(rx_buf));
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "[PASS] 取得模組資訊:\n%s", rx_buf);
    }

    // ==========================================
    // 闖關 3：開啟 GPS
    // ==========================================
    ESP_LOGI(TAG, "\n--> Step 3: 開啟 GPS 電源...");
    err = drv_4g_gnss_power(true);
    if (err == ESP_OK) ESP_LOGI(TAG, "[PASS] GPS 已成功開啟");
    else ESP_LOGE(TAG, "[FAIL] GPS 開啟超時");
    vTaskDelay(pdMS_TO_TICKS(1000));

    // ==========================================
    // 闖關 4：設定 APN 並註冊網路
    // ==========================================
    ESP_LOGI(TAG, "\n--> Step 4: 設定 APN (internet)...");
    err = drv_4g_set_apn();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "[PASS] APN 設定成功");
    } else {
        ESP_LOGE(TAG, "[FAIL] APN 設定失敗");
    }
    
    ESP_LOGW(TAG, "等待 3 秒讓模組穩穩地註冊上基地台...");
    vTaskDelay(pdMS_TO_TICKS(3000));

    // ==========================================
    // 闖關 5：建立 TCP 連線
    // ==========================================
    // 使用你 Log 中最新測試成功的 Port 32702
    const char *target_ip = "112.125.89.8"; 
    int target_port = 34093;
    
    ESP_LOGI(TAG, "\n--> Step 5: 嘗試 TCP 連線至 %s:%d ...", target_ip, target_port);
    err = drv_4g_tcp_connect(target_ip, target_port);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "[PASS] TCP 連線成功！(+CIPOPEN: SUCCESS,1)");
        vTaskDelay(pdMS_TO_TICKS(1000));

        // ==========================================
        // 闖關 6：透過 TCP 發送資料
        // ==========================================
        ESP_LOGI(TAG, "\n--> Step 6: 透過 TCP 發送推論測試字串...");
        const char *payload = "Hello from Master TTGO!";
        err = drv_4g_tcp_send((const uint8_t *)payload, strlen(payload));
        
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "[PASS] 資料發送成功！(+CIPSEND:SUCCESS)");
        } else {
            ESP_LOGE(TAG, "[FAIL] 資料發送失敗或超時");
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
        
        // ==========================================
        // 闖關 7：關閉 TCP 連線
        // ==========================================
        ESP_LOGI(TAG, "\n--> Step 7: 關閉 TCP 連線...");
        err = drv_4g_tcp_close();
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "[PASS] TCP 已安全關閉！(+CIPCLOSE: SUCCESS,1)");
        } else {
            ESP_LOGE(TAG, "[FAIL] TCP 關閉失敗");
        }
    } else {
        ESP_LOGE(TAG, "[FAIL] TCP 連線失敗，請檢查伺服器是否開啟或 SIM 卡網路狀態。");
    }
    vTaskDelay(pdMS_TO_TICKS(2000));
    err = drv_4g_send_at_cmd("AT\r\n", "OK", 2000, NULL, 0);
    if (err == ESP_OK) ESP_LOGI(TAG, "[PASS] AT 握手成功");
    // ==========================================
    // 闖關 8：測試 NMEA 持續更新廣播
    // ==========================================
    ESP_LOGI(TAG, "\n--> Step 8: 開啟 NMEA 持續更新 (監聽 5 秒)...");
    err = drv_4g_start_nmea_stream();
    
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "[PASS] NMEA 更新已開啟！準備接收資料：\n");
        printf("================ NMEA STREAM START ================\n");
        
        // 呼叫我們新寫好的監聽函式，卡在這裡靜靜聽 5 秒鐘
        drv_4g_listen_and_print(15000); 
        
        printf("================ NMEA STREAM END ==================\n");

        // 5 秒結束後，立刻關閉廣播
        ESP_LOGI(TAG, "\n--> Step 9: 關閉 NMEA 持續更新...");
        err = drv_4g_stop_nmea_stream();
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "[PASS] NMEA 更新已成功關閉！");
        } else {
            ESP_LOGE(TAG, "[FAIL] NMEA 關閉失敗");
        }
    } else {
        ESP_LOGE(TAG, "[FAIL] 無法開啟 NMEA 更新");
    }

    ESP_LOGI(TAG, "\n========== 測試流程結束，板子安全閒置中 ==========");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
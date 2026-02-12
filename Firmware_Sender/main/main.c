#include <stdio.h>
#include "esp_log.h"
#include "driver/uart.h"
#include "sys_defs.h"
#include "drv_gnss.h"
#include "drv_ble_adv.h"

#define GPS_UART_NUM UART_NUM_1
#define GPS_TX_PIN   5
#define GPS_RX_PIN   4
#define GPS_BAUD     9600

static const char *TAG = "MAIN";

void _test_gps(void)
{
    gps_data_t gps = gnss_get_data();

    if (gps.is_valid) {
        // --- 定位成功 ---
            
        ESP_LOGI(TAG, " GPS FIX! --------------------------------");
        ESP_LOGI(TAG, " Lat : %ld.%07ld", 
                gps.latitude_scaled / 10000000, 
                abs(gps.latitude_scaled % 10000000));
            
        ESP_LOGI(TAG, " Lon : %ld.%07ld", 
                gps.longitude_scaled / 10000000, 
                abs(gps.longitude_scaled % 10000000));
            
        ESP_LOGI(TAG, " Spd : %d.%02d km/h", 
                gps.speed_kph_scaled / 100, 
                abs(gps.speed_kph_scaled % 100));
            
        ESP_LOGI(TAG, " Alt : %d m", gps.altitude_m);
        ESP_LOGI(TAG, "------------------------------------------");

    } else {
        // 此時經緯度通常被清零，高度也應該是 0
        ESP_LOGW(TAG, " Searching... (Lat: %ld, Lon: %ld)", 
                gps.latitude_scaled, gps.longitude_scaled);
    }
}

void app_main(void)
{
	//Set UART log level
    esp_log_level_set("GNSS", ESP_LOG_INFO);
    esp_log_level_set("BLE_BROADCAST", ESP_LOG_INFO);
    
	//Initialize GNSS module on UART1, TX pin 5, RX pin 4, baud rate 9600
	gnss_init(GPS_UART_NUM, GPS_TX_PIN, GPS_RX_PIN, GPS_BAUD);
    ble_adv_init();

    ESP_LOGI(TAG, "-----------------------------");
    ESP_LOGI(TAG, " System Running.");
    ESP_LOGI(TAG, " GNSS parsing in background.");
    ESP_LOGI(TAG, " BLE advertising at 10Hz.");
    ESP_LOGI(TAG, "-----------------------------");

    while (1) {
        
        // 每 5 秒印一次心跳包
        ESP_LOGI(TAG, "System Alive... (Broadcasting in progress)");
        
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
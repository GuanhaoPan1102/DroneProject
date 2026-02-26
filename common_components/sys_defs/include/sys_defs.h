#ifndef COMMON_SYS_DEFS_H // Include guard
#define COMMON_SYS_DEFS_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Fixed header byte for BLE telemetry packets (0xAA).
/// @note [Protocol Identifier]
/// This header is used to distinguish UAV telemetry packets from other ambient BLE noise 
/// (e.g., smartwatches, headphones).
/// In a standardized commercial deployment (e.g., Remote ID), this custom header 
/// would be replaced by a standard Service UUID
#define BLE_PACKET_IDENTIFIER   0XAA

#define BLE_PAYLOAD_SIZE        sizeof(uav_ble_payload_t)

#define UART_HEADER_0  0xEB
#define UART_HEADER_1  0x90

// UAV GPS 資料結構
typedef struct {
    // 經緯度：單位為 度 * 10^7
    // 例如：25.0339640 度 -> 儲存為 250339640
    int32_t latitude_scaled;
    int32_t longitude_scaled;

    // 高度：單位為 公尺 (m)
    // 來自 GPGGA ，若未定位則為 0
    int16_t altitude_m;

    // 地面速度：單位為 0.01 km/h
    // 例如：12.34 km/h -> 儲存為 1234
    // 來自 GPRMC
    int16_t speed_kph_scaled;

    // 定位狀態旗標
    // true = 定位有效 (Fix), false = 搜尋中或無效
    bool is_valid;

} gps_data_t;

// BLE 封包結構
typedef struct __attribute__((packed)) {
    uint8_t identifier;        // 標示碼
    uint8_t seq_num;           // 封包序列碼
    int32_t lat;               // 經度 (單位為 度 * 10^7)
    int32_t lon;               // 緯度 (單位為 度 * 10^7)
    int16_t alt;               // 高度 (單位為 公尺 (m))
    int16_t spd;               // 速度 (單位為 0.01 km/h)
    uint8_t checksum;          // 校驗碼
} uav_ble_payload_t;

// BLE to Queue 資料結構
typedef struct {
    uav_ble_payload_t payload; // 原本的 UAV 資料 (15 bytes)
    int rssi;                  // 訊號強度
} ble_packet_queue_item_t;

// 地面站GPS 資料結構
typedef struct {
    int32_t latitude;   // 緯度 (例如 221234567 代表 22.1234567 度)
    int32_t longitude;  // 經度
    float altitude;     // 高度 (單位: 公尺)
    bool is_fixed;      // 是否已經完成 60 次平均並鎖定
    bool is_time_synced;// 是否已經完成時間同步
} gps_fix_t;

// ESP-NOW 訊息類型
typedef enum {
    MSG_TYPE_REGISTER = 0,    // 節點註冊 (傳送地面站 GPS 位置)
    MSG_TYPE_FILE_SAVED = 1,  // 檔案儲存完畢通知
    MSG_TYPE_BLE_DATA = 2     // 預留給未來即時傳輸 BLE RSSI 用
} espnow_msg_type_t;

// ESP-NOW 傳輸 Payload (使用 packed 避免記憶體對齊造成的長度誤差)
typedef struct __attribute__((packed)) {
    uint8_t  msg_type;   // 對應 espnow_msg_type_t
    uint8_t  node_id;    // 節點編號 (例如 Slave 1, 2, 3)
    
    // 使用 union 讓不同種類的訊息共用這塊記憶體空間，節省傳輸頻寬
    union {
        // [MSG_TYPE_REGISTER] 註冊用的資料
        struct {
            int32_t lat;
            int32_t lon;
            float   alt;
        } reg;

        // [MSG_TYPE_FILE_SAVED] 檔案儲存回報用的資料
        struct {
            int64_t timestamp;     // 儲存當下的時間戳 (已校正為純 UTC)
            char    filename[16];  // 檔名，例如 "DATA1.csv"
        } file;
        
        // [MSG_TYPE_BLE_DATA] 即時 BLE 資料 (未來可直接把你的 Queue Item 傳過來)
        struct {
            int64_t timestamp;
            ble_packet_queue_item_t ble_data; 
        } live_data;

    } data;
} espnow_payload_t;

#ifdef __cplusplus
}
#endif

#endif // COMMON_SYS_DEFS_H
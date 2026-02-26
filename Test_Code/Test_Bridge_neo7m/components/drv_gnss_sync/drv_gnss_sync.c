#include <stdio.h>
#include <stdlib.h>
#include "drv_gnss_sync.h"
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "GNSS";
#define BUF_SIZE 1024
#define RD_BUF_SIZE (BUF_SIZE)

static int g_gps_uart_num = UART_NUM_1; 
static QueueHandle_t uart_queue;

// --- PPS 時間同步變數 ---
static volatile time_t _next_pps_timestamp = 0; // 下一秒的時間戳

// --- Survey-In 設定 ---
#define SURVEY_SAMPLES 60   // 目標：平均 60 次
static int _rmc_count = 0; // 計算 RMC (經緯度) 收到幾包
static int _gga_count = 0; // 計算 GGA (高度) 收到幾包

static int64_t _sum_lat = 0;
static int64_t _sum_lon = 0;
static float   _sum_alt = 0.0f;

// 儲存最終結果
static gps_fix_t _final_fix = {0};

static int32_t _convert_nmea_to_scaled(float nmea_val) // DDMM.MMMM
{
    int degrees = (int)(nmea_val / 100);             // 取出度 (D)
    float minutes = nmea_val - (degrees * 100);      // 取出分 (M)
    float decimal_deg = degrees + (minutes / 60.0f); // 換算成小數度 (D + M/60)
    return (int32_t)(decimal_deg * 10000000);        // 轉成整數 (D + M/60) * 10^7
}

// --- [核心] PPS 中斷處理 (ISR) ---
static void IRAM_ATTR pps_gpio_isr_handler(void* arg)
{
    // 如果我們已經算出下一秒的時間，且 PPS 訊號來了 (代表整點瞬間)
    if (_next_pps_timestamp > 0) {
        struct timeval tv;
        tv.tv_sec = _next_pps_timestamp; // 設定秒數
        tv.tv_usec = 0;                  // 微秒歸零 (這是精度的關鍵)
        
        settimeofday(&tv, NULL);         // 寫入 ESP32 系統時間

        _next_pps_timestamp++;           // 自動推進一秒，預防下一次 NMEA 掉包
        
        // (選擇性) 更新結構體標記，讓 Main 知道時間已同步
        _final_fix.is_time_synced = true;
    }
}

static void _parse_rmc(char *line)
{
    char *rest = line;
    char *token;
    int field = 0;

    // 暫存變數
    int raw_time = 0; // HHMMSS
    int raw_date = 0; // DDMMYY
    
    int32_t curr_lat = 0;
    int32_t curr_lon = 0;
    bool valid = false;

    // 使用 strsep 切割字串
    while ((token = strsep(&rest, ",")) != NULL) {
        size_t len = strlen(token);
        switch (field) {
            // [Field 1] 時間 (永遠解析)
            case 1: 
                if (len > 0) raw_time = atoi(token); 
                break; 

            // [Field 2] 狀態 (A=有效, V=無效)
            case 2: 
                if (len > 0 && token[0] == 'A') valid = true; 
                break;
            
            // [Field 3-6] 經緯度 (只有在 "未鎖定" 時才解析，節省資源)
            case 3: 
                if (!_final_fix.is_fixed && len > 0) 
                    curr_lat = _convert_nmea_to_scaled(strtof(token, NULL)); 
                break;
            case 4: 
                if (!_final_fix.is_fixed && len > 0 && token[0] == 'S') 
                    curr_lat *= -1; 
                break;
            case 5: 
                if (!_final_fix.is_fixed && len > 0) 
                    curr_lon = _convert_nmea_to_scaled(strtof(token, NULL)); 
                break;
            case 6: 
                if (!_final_fix.is_fixed && len > 0 && token[0] == 'W') 
                    curr_lon *= -1; 
                break;

            // [Field 9] 日期 (永遠解析，配合時間做同步)
            case 9: 
                if (len > 0) raw_date = atoi(token); 
                break; 
        }
        field++;
    }

    // === 任務 A: 時間同步 (永遠執行) ===
    // 只要有日期跟時間，就計算下一秒的 Timestamp 預備給 PPS 使用
    if (raw_date > 0 && raw_time > 0) {
        struct tm tm_struct = {0};
        
        // 解析 HHMMSS
        tm_struct.tm_hour = raw_time / 10000;
        tm_struct.tm_min  = (raw_time % 10000) / 100;
        tm_struct.tm_sec  = raw_time % 100;
        
        // 解析 DDMMYY
        tm_struct.tm_mday = raw_date / 10000;
        tm_struct.tm_mon  = ((raw_date % 10000) / 100) - 1; // 月份要 -1 (0-11)
        tm_struct.tm_year = (raw_date % 100) + 100;         // 年份要 +100 (100 = 2000年)

        // 計算 Unix Timestamp
        time_t current_gps_time = mktime(&tm_struct);
        
        // NMEA 報的是「刚刚」那個 PPS 的時間
        // 所以下一個 PPS 來臨時，時間應該是 +1 秒
        _next_pps_timestamp = current_gps_time + 1;
    }

    // === 任務 B: 位置平均 (鎖定後就不執行) ===
    // 只有在 (1) 還沒鎖定 且 (2) 資料有效 時，才進行累加
    if (!_final_fix.is_fixed && valid) {
        _sum_lat += curr_lat;
        _sum_lon += curr_lon;
        _rmc_count++;
        // ESP_LOGI(TAG, "RMC data count: %d", _rmc_count);
        // 注意：這裡只負責累加，鎖定判斷邏輯 (is_fixed = true) 
        // 會在 _process_nmea_line 裡面統一檢查 (因為還要等 GGA 的高度)
    }
}

static void _parse_gga(char *line)
{
    char *rest = line;
    char *token;
    int field = 0;
    
    // 暫存變數
    bool quality_valid = false;
    float current_alt = 0.0f;

    // 使用 strsep 切割逗號
    while ((token = strsep(&rest, ",")) != NULL) {
        size_t len = strlen(token);

        switch (field) {
            // [Field 6] 定位品質 Indicator
            // 0 = 無效 (Fix not available)
            // 1 = GPS fix
            // 2 = DGPS fix (誤差更小)
            // ... 其他數字也代表有效
            case 6: 
                // 只要字元不是 '0'，我們就認為這包數據是有效的
                if (len > 0 && token[0] != '0') {
                    quality_valid = true;
                }
                break;

            // [Field 9] 海拔高度 (Antenna Altitude above/below mean sea level)
            // 單位是公尺 (Meters)
            case 9: 
                // 只有在還沒鎖定時才需要轉換 float，節省 CPU
                if (!_final_fix.is_fixed && len > 0) {
                    current_alt = strtof(token, NULL);
                }
                break;
        }
        field++;
    }

    // === 任務: 高度平均累加 ===
    // 只有在 (1) 定位品質有效 且 (2) 地面站還沒鎖定 時，才進行累加
    if (quality_valid && !_final_fix.is_fixed) {
        _sum_alt += current_alt;
        _gga_count++;
        
        // 注意：這裡只負責累加，鎖定判斷邏輯 (is_fixed = true) 
        // 統一在 _process_nmea_line 或 _parse_rmc 裡面做檢查
    }
}

static void _process_nmea_line(char *line)
{
    // [情況 1] 如果是 RMC 封包 (包含經緯度與時間)
    // 注意：即使 is_fixed = true，我們還是要進去，因為裡面有「時間同步」的邏輯
    if (strncmp(line, "$GNRMC", 6) == 0 || strncmp(line, "$GPRMC", 6) == 0) {
        //ESP_LOGI(TAG, "read data: %s", (char *)line);
        _parse_rmc(line); // 解析！(內部會自己判斷要不要累加經緯度)
        
        // --- 檢查是否可以鎖定位置 (Survey-In 完成?) ---
        // 條件：
        // 1. 目前還沒鎖定
        // 2. RMC (水平) 樣本數夠了
        // 3. GGA (垂直) 樣本數也夠了
        if (!_final_fix.is_fixed && 
            _rmc_count >= SURVEY_SAMPLES && 
            _gga_count >= SURVEY_SAMPLES) {
            
            // 進行最終平均計算
            _final_fix.latitude = (int32_t)(_sum_lat / _rmc_count);
            _final_fix.longitude = (int32_t)(_sum_lon / _rmc_count);
            _final_fix.altitude = _sum_alt / _gga_count;
            
            // 正式鎖定！
            _final_fix.is_fixed = true;

            ESP_LOGI(TAG, "=== Ground Station Position LOCKED ===");
            ESP_LOGI(TAG, "Lat: %d, Lon: %d, Alt: %.2f m", 
                     _final_fix.latitude, _final_fix.longitude, _final_fix.altitude);
        }
    } 
    // [情況 2] 如果是 GGA 封包 (包含高度)
    // 優化：只有在「還沒鎖定」的時候才解析，鎖定後直接忽略，省 CPU
    else if (!_final_fix.is_fixed) {
        if (strncmp(line, "$GNGGA", 6) == 0 || strncmp(line, "$GPGGA", 6) == 0) {
            _parse_gga(line);
        }
    }
}

static void gps_event_task(void *pvParameters)
{
	uart_event_t event;
    //uint8_t* dtmp = (uint8_t*) malloc(RD_BUF_SIZE);
    //assert(dtmp);
    uint8_t* dtmp = NULL;
    while (dtmp == NULL) {
        dtmp = (uint8_t*) malloc(RD_BUF_SIZE);
        
        if (dtmp == NULL) {
            ESP_LOGE(TAG, "Malloc failed! Retrying in 1 sec...");
            // 休息 1 秒鐘，避免佔用 CPU 資源 (Busy Waiting)
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    uart_flush_input(g_gps_uart_num); 
    xQueueReset(uart_queue);

    for (;;) {
        //Waiting for UART event.
        if (xQueueReceive(uart_queue, (void *)&event, (TickType_t)portMAX_DELAY)) {

            size_t buffered_size;
            uart_get_buffered_data_len(g_gps_uart_num, &buffered_size);

            // 9600 baud 之下，一秒鐘的資料量大約是 300~500 bytes。
            // 如果緩衝區超過 600 bytes，代表裡面積了超過 1~2 秒的舊資料。
            // 這時候不要讀了，直接清空，強迫系統去抓下一秒的「最新」資料。
            if (buffered_size > 600) {
                ESP_LOGW(TAG, "Buffer full (%d bytes), flushing old data to sync real-time!", buffered_size);
                uart_flush_input(g_gps_uart_num); // 清空硬體 FIFO 與 Ring Buffer
                xQueueReset(uart_queue);          // 清空事件 Queue
                continue;                         // 跳過這次迴圈，重新等待最新的 Pattern
            }

            memset(dtmp, 0, RD_BUF_SIZE);
            switch (event.type) {

            //UART_PATTERN_DET
            case UART_PATTERN_DET:
                int pos = uart_pattern_pop_pos(g_gps_uart_num);
                if (pos != -1) {
                    int len = uart_read_bytes(g_gps_uart_num, dtmp, pos+1, 100 / portTICK_PERIOD_MS);
					dtmp[len] = '\0';
                    //ESP_LOGI(TAG, "read data: %s", (char *)dtmp);
                    char *start = strchr((char *)dtmp, '$');
                    if (start != NULL) {
                        _process_nmea_line(start);
                    }
                } else {
                    uart_flush_input(g_gps_uart_num);
                }
                break;

			//HW FIFO overflow detected or UART ring buffer full
            case UART_FIFO_OVF:
            case UART_BUFFER_FULL:
                uart_flush_input(g_gps_uart_num);
                xQueueReset(uart_queue);
                break;
            //Others
            default:
                break;
            }
        }
    }
    free(dtmp);
    dtmp = NULL;
    vTaskDelete(NULL);
}

void gnss_sync_init(int uart_num, int tx_pin, int rx_pin, int pps_pin, int baud_rate)
{
    ESP_LOGI(TAG, "Initializing GNSS Parser...");
	g_gps_uart_num = uart_num;
	/* Configure parameters of an UART driver,
     * communication pins and install the driver */
    uart_config_t uart_config = {
        .baud_rate = baud_rate,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    //Install UART driver, and get the queue.
    ESP_ERROR_CHECK(uart_driver_install(uart_num, BUF_SIZE * 2, BUF_SIZE * 2, 20, &uart_queue, 0));
    ESP_ERROR_CHECK(uart_param_config(uart_num, &uart_config));

    //Set UART pins (using UART0 default pins ie no changes.)
    ESP_ERROR_CHECK(uart_set_pin(uart_num, tx_pin, rx_pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    //Set uart pattern detect function.
    uart_enable_pattern_det_baud_intr(uart_num, '\n', 1, 9, 0, 0);
    //Reset the pattern queue length to record at most 20 pattern positions.
    uart_pattern_queue_reset(uart_num, 20);

    // Set PPS GPIO
    if (pps_pin >= 0) { // 加個判斷，允許傳入 -1 來停用 PPS
        gpio_config_t io_conf = {};
        io_conf.intr_type = GPIO_INTR_POSEDGE; // 上升緣觸發
        io_conf.pin_bit_mask = (1ULL << pps_pin);
        io_conf.mode = GPIO_MODE_INPUT;
        io_conf.pull_up_en = 0;
        io_conf.pull_down_en = 0;
        gpio_config(&io_conf);

        // 安裝 ISR 服務 (使用 ESP_OK 檢查避免重複安裝報錯)
        if (gpio_install_isr_service(0) != ESP_OK) {
            ESP_LOGW(TAG, "ISR Service already installed, skipping init.");
        }
        gpio_isr_handler_add(pps_pin, pps_gpio_isr_handler, NULL);
        ESP_LOGI(TAG, "PPS enabled on GPIO %d", pps_pin);
    } else {
        ESP_LOGW(TAG, "PPS pin is set to %d, skipping PPS init.", pps_pin);
    }

    //Create a task to handler UART event from ISR
    xTaskCreate(gps_event_task, "gps_task", 4096, NULL, 12, NULL);

	ESP_LOGI(TAG, "GPS Init on UART%d (TX:%d, RX:%d, Baud:%d)", uart_num, tx_pin, rx_pin, baud_rate);
}

void gnss_reset_fix(void) {
    _rmc_count = 0;
    _gga_count = 0;
    _sum_lat = 0;
    _sum_lon = 0;
    _sum_alt = 0.0f; // 清空高度
    _final_fix.is_fixed = false;
    ESP_LOGW(TAG, "Fix Reset. Surveying started...");
}

gps_fix_t gnss_get_fix(void)
{
    return _final_fix;
}

void configure_neo7m(int uart_num)
{
    ESP_LOGI(TAG, "Configuring u-blox NEO-7M GPS...");

    // 1. 設定 Update Rate 為 10Hz (100ms)
    // 協定: UBX-CFG-RATE (Class 0x06, ID 0x08)
    const uint8_t ubx_cfg_rate[] = {
        0xB5, 0x62, 0x06, 0x08, 0x06, 0x00, 
        0x64, 0x00, 0x01, 0x00, 0x00, 0x00, 
        0x79, 0x10  // Checksum
    };
    uart_write_bytes(uart_num, (const char*)ubx_cfg_rate, sizeof(ubx_cfg_rate));
    ESP_LOGI(TAG, "Update Rate set to 10Hz (UBX protocol)");
    
    // 給予模組一點時間消化速率變更
    vTaskDelay(pdMS_TO_TICKS(100)); 

    // 2. 設定 Baud Rate 為 115200 (針對 UART1)
    // 協定: UBX-CFG-PRT (Class 0x06, ID 0x00)
    const uint8_t ubx_cfg_prt[] = {
        0xB5, 0x62, 0x06, 0x00, 0x14, 0x00, 
        0x01, 0x00, 0x00, 0x00, 0xD0, 0x08, 0x00, 0x00, 
        0x00, 0xC2, 0x01, 0x00, 0x07, 0x00, 0x03, 0x00, 
        0x00, 0x00, 0x00, 0x00, 
        0xC0, 0x7E  // Checksum
    };
    uart_write_bytes(uart_num, (const char*)ubx_cfg_prt, sizeof(ubx_cfg_prt));
    ESP_LOGI(TAG, "Baud Rate set command sent. Switching ESP32 UART...");

    // 3. 立刻切換 ESP32 的 UART 速度來追上它
    // 等待 200ms 確保模組硬體已經切換完畢
    vTaskDelay(pdMS_TO_TICKS(200)); 
    uart_set_baudrate(uart_num, 115200);
    ESP_LOGI(TAG, "ESP32 UART switched to 115200");

    // 4. 儲存設定 (Save to Flash/BBR)
    // 協定: UBX-CFG-CFG (Class 0x06, ID 0x09)
    // 這一步非常重要，否則 NEO-7M 斷電後會恢復成 9600 baud / 1Hz
    const uint8_t ubx_cfg_save[] = {
        0xB5, 0x62, 0x06, 0x09, 0x0D, 0x00, 
        0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0x00, 0x00, 
        0x00, 0x00, 0x00, 0x00, 0x07, 
        0x21, 0xAF  // Checksum
    };
    uart_write_bytes(uart_num, (const char*)ubx_cfg_save, sizeof(ubx_cfg_save));
    ESP_LOGI(TAG, "Configuration Saved to NEO-7M Flash/BBR");
}

// 用來發送指令的輔助函式
void send_cmd(int uart_num, const char* cmd) {
    uart_write_bytes(uart_num, cmd, strlen(cmd));
    // 等待一點時間讓模組處理
    vTaskDelay(pdMS_TO_TICKS(100)); 
}

void configure_atgm336h(int uart_num)
{
    ESP_LOGI(TAG, "Configuring ATGM336H GPS...");

    // 1. 先設定 Update Rate 為 10Hz (此時還是 9600 baud)
    //    注意：如果您不需要這麼快，可以改用 5Hz ($PCAS02,200*1D)
    send_cmd(uart_num, "$PCAS02,100*1E\r\n");
    ESP_LOGI(TAG, "Update Rate set to 10Hz");

    // 2. 設定 Baud Rate 為 115200
    //    警告：送出這行後，模組會立刻變心，我們只有幾毫秒的時間可以切換
    send_cmd(uart_num, "$PCAS01,5*19\r\n");
    ESP_LOGI(TAG, "Baud Rate set command sent. Switching ESP32 UART...");

    // 3. 立刻切換 ESP32 的 UART 速度來追上它
    //    等待 200ms 確保模組已經切換完畢
    vTaskDelay(pdMS_TO_TICKS(200)); 
    uart_set_baudrate(uart_num, 115200);
    ESP_LOGI(TAG, "ESP32 UART switched to 115200");

    // 4. 儲存設定 (Save to Flash)
    //    這時候我們已經是用 115200 在溝通了
    //    如果這行成功，代表通訊握手成功
    send_cmd(uart_num, "$PCAS00*01\r\n");
    ESP_LOGI(TAG, "Configuration Saved to Flash");
}
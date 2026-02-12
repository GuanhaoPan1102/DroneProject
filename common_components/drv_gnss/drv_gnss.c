#include <stdio.h>
#include <stdlib.h>
#include "drv_gnss.h"
#include <string.h>
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

// 儲存最新的解析結果
static gps_data_t _latest_data = {0};

static int32_t _convert_nmea_to_scaled(float nmea_val) // DDMM.MMMM
{
    int degrees = (int)(nmea_val / 100);             // 取出度 (D)
    float minutes = nmea_val - (degrees * 100);      // 取出分 (M)
    float decimal_deg = degrees + (minutes / 60.0f); // 換算成小數度 (D + M/60)
    return (int32_t)(decimal_deg * 10000000);        // 轉成整數 (D + M/60) * 10^7
}

static void _parse_rmc(char *line)
{
    char *rest = line;
    char *token;
    int field_index = 0;

    while((token = strsep(&rest, ",")) != NULL ) {
        size_t len = strlen(token);

        switch (field_index) {
        case 0:
            break;

        case 1:
            break;

        case 2:
            if (len>0) {
                if (token[0] == 'A') {
                    _latest_data.is_valid = true;
                } else if (token[0] == 'V') {
                    _latest_data.is_valid = false;
                    _latest_data.latitude_scaled = 0;
                    _latest_data.longitude_scaled = 0;
                    _latest_data.altitude_m = 0;
                    _latest_data.speed_kph_scaled = 0;
                    return;
                }
            }
            break;

        case 3:
            if (len>0) {
                float lat_raw = strtof(token, NULL);
                _latest_data.latitude_scaled = _convert_nmea_to_scaled(lat_raw);
            }
            break;
        
        case 4:
            if (len>0 && token[0] == 'S') {
                _latest_data.latitude_scaled *= -1;
            }
            break;

        case 5:
            if (len>0) {
                float lon_raw = strtof(token, NULL);
                _latest_data.longitude_scaled = _convert_nmea_to_scaled(lon_raw);
            }
            break;

        case 6:
            if (len>0 && token[0] == 'W') {
                _latest_data.longitude_scaled *= -1;
            }
            break;
        
        case 7:
            if (len>0) {
                float speed_knots = strtof(token, NULL);
                _latest_data.speed_kph_scaled = (int16_t)(speed_knots * 185.2f); // 1 knot = 1.852 km/h
            }

        default:
            break;
        }

        field_index++;
    }
}

static void _parse_gga(char *line)
{
    char *rest = line;
    char *token;
    int field_index = 0;

    while((token = strsep(&rest, ",")) != NULL ) {
        size_t len = strlen(token);

        switch (field_index) {
        case 6: // Fix Quality (0=Invalid, 1=GPS fix, 2=DGPS fix...)
            if (len > 0) {
                int quality = atoi(token);
                if (quality == 0) {
                    _latest_data.altitude_m = 0;
                }
            }
            break;

        case 9: 
            if (len > 0) {
                float alt = strtof(token, NULL);
                _latest_data.altitude_m = (int16_t)alt;
            }
            break;
        }
        field_index++;
    }
}

static void _process_nmea_line(char *line)
{
    // 判斷是否為 RMC (定位資料)
    if (strncmp(line, "$GNRMC", 6) == 0 || strncmp(line, "$GPRMC", 6) == 0) {
        _parse_rmc(line);
    }
    // 判斷是否為 GGA (高度資料)
    else if (strncmp(line, "$GNGGA", 6) == 0 || strncmp(line, "$GPGGA", 6) == 0) {
        _parse_gga(line);
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

    for (;;) {
        //Waiting for UART event.
        if (xQueueReceive(uart_queue, (void *)&event, (TickType_t)portMAX_DELAY)) {
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

void gnss_init(int uart_num, int tx_pin, int rx_pin, int baud_rate)
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

    //Create a task to handler UART event from ISR
    xTaskCreate(gps_event_task, "gps_task", 4096, NULL, 12, NULL);

	ESP_LOGI(TAG, "GPS Init on UART%d (TX:%d, RX:%d, Baud:%d)", uart_num, tx_pin, rx_pin, baud_rate);
}

gps_data_t gnss_get_data(void)
{
    return _latest_data;
}

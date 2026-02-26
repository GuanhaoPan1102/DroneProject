#ifndef COMMON_DRV_GNSS_SYNC_H  // Include Guard
#define COMMON_DRV_GNSS_SYNC_H

#include <stdint.h>
#include <stdbool.h>
#include "sys_defs.h"

void gnss_sync_init(int uart_num, int tx_pin, int rx_pin, int pps_pin, int baud_rate);

// 取得目前的定位狀態
gps_fix_t gnss_get_fix(void);

// 重新定位
void gnss_reset_fix(void);

void configure_neo7m(int uart_num);

#endif // COMMON_DRV_GNSS_H
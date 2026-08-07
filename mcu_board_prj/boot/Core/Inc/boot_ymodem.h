/**
 * Bootloader Ymodem OTA: dual UART (USART1/2), idle+DMA RX, OTA module flash write.
 */
#ifndef BOOT_YMODEM_H
#define BOOT_YMODEM_H

#include "main.h"
#include "usart.h"

#ifdef __cplusplus
extern "C" {
#endif

/* printf/diagnostic output target: Ymodem selects the non-upgrade UART. */
extern UART_HandleTypeDef *g_boot_debug_uart;

void boot_ymodem_init(void);
void boot_ymodem_poll(void);

/**
 * @brief Tentative Ymodem discovery: send 'C' on listening UART(s), up to @p discovery_rounds times.
 *        Each round waits BOOT_YM_PROBE_WINDOW_MS (default 300 ms, overridable before include) and
 *        drains RX; if the host starts Ymodem (link locked or stream has bytes), returns 0.
 * @return 0 host responded — keep calling boot_ymodem_poll() to complete transfer.
 * @return -1 no response in all rounds — caller may run ota_module_boot_preprocess() to enter APP.
 */
#ifndef BOOT_YM_PROBE_WINDOW_MS
#define BOOT_YM_PROBE_WINDOW_MS 100u
#endif
int boot_ymodem_probe(uint32_t discovery_rounds);

#ifdef __cplusplus
}
#endif

#endif /* BOOT_YMODEM_H */

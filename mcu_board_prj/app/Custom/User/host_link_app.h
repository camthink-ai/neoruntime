#ifndef HOST_LINK_APP_H
#define HOST_LINK_APP_H

#ifdef __cplusplus
extern "C" {
#endif

#include "sys_config.h"
#include "host_link.h"

/** USART2 + DMA RX/TX glue for host_link (Linux host serial). */
#define HOST_LINK_APP_TASK_UART_NAME        "hl_uart2"
#define HOST_LINK_APP_TASK_UART_STACK       (2048u)
#define HOST_LINK_APP_TASK_UART_PRIORITY    (4)

#define HOST_LINK_APP_TASK_POLL_NAME        "hl_poll"
#define HOST_LINK_APP_TASK_POLL_STACK       (1536u)
#define HOST_LINK_APP_TASK_POLL_PRIORITY    (4)

/** RX DMA buffer (binary stream; not null-terminated). >= max host_link frame (14+512+2). */
#define HOST_LINK_APP_RX_BUF_SIZE           (1024u)

int host_link_app_init(void);
void host_link_app_deinit(void);

/** NULL before host_link_app_init() or after host_link_app_deinit(). */
host_link_handler_t *host_link_app_handler(void);

/**
 * Called from HAL (ISR): USART2 RxEvent (legacy hook; RX uses circular DMA + task drain).
 * Implemented in host_link_app.c; referenced from Core/Src/usart.c.
 */
void host_link_app_on_uart2_rx_event(UART_HandleTypeDef *huart, uint16_t size);

/**
 * Called from HAL (ISR): USART2 TX DMA complete.
 */
void host_link_app_on_uart2_tx_done(UART_HandleTypeDef *huart);

/**
 * Called from HAL (ISR): USART2 error.
 */
void host_link_app_on_uart2_error(UART_HandleTypeDef *huart);

#ifdef __cplusplus
}
#endif

#endif /* HOST_LINK_APP_H */

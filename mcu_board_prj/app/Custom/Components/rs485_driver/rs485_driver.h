#ifndef RS485_DRIVER_H
#define RS485_DRIVER_H

#ifdef __cplusplus
extern "C" {
#endif

#include "sys_config.h"
#include "usart.h"

#define RS485_RX_BUFFER_SIZE 512u
#define RS485_TX_BUFFER_SIZE 512u
#define RS485_DEFAULT_TIMEOUT_MS 200u

typedef void (*rs485_rx_callback_t)(const uint8_t *data, uint16_t len, void *user);

int rs485_driver_init(uint32_t baudrate, const char *config);
int rs485_driver_deinit(void);
int rs485_driver_send(const uint8_t *data, uint16_t len, uint32_t timeout_ms);
int rs485_driver_recv(uint8_t *data, uint16_t data_len, uint16_t *out_len, uint32_t timeout_ms);
int rs485_driver_txrx(const uint8_t *tx_data, uint16_t tx_len, uint8_t *rx_data, uint16_t rx_len,
                      uint16_t *out_len, uint32_t timeout_ms);
int rs485_driver_is_inited(void);
int rs485_driver_set_rx_callback(rs485_rx_callback_t cb, void *user);

void rs485_driver_on_uart3_rx_event(UART_HandleTypeDef *huart, uint16_t size);
void rs485_driver_on_uart3_tx_done(UART_HandleTypeDef *huart);
void rs485_driver_on_uart3_error(UART_HandleTypeDef *huart);

#ifdef __cplusplus
}
#endif

#endif /* RS485_DRIVER_H */

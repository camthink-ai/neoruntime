#include "rs485_driver.h"
#include <string.h>

#define RS485_EV_RX_COMPLETE ((EventBits_t)(1u << 0))
#define RS485_EV_RX_ERROR    ((EventBits_t)(1u << 1))
#define RS485_EV_TX_COMPLETE ((EventBits_t)(1u << 2))
#define RS485_EV_RX_NOTIFY   ((EventBits_t)(1u << 3))
#define RS485_EV_RX_FRAME    ((EventBits_t)(1u << 4))
#define RS485_EV_EXIT_REQ    ((EventBits_t)(1u << 5))
#define RS485_EV_EXIT_ACK    ((EventBits_t)(1u << 6))

static EventGroupHandle_t s_events;
static SemaphoreHandle_t s_tx_mutex;
static SemaphoreHandle_t s_rx_frame_mutex;
static TaskHandle_t s_rx_task;
static uint8_t s_inited;
static uint8_t s_rx_buf[RS485_RX_BUFFER_SIZE];
static uint8_t s_tx_buf[RS485_TX_BUFFER_SIZE];
static uint8_t s_rx_frame[RS485_RX_BUFFER_SIZE];
static volatile uint16_t s_rx_len;
static uint16_t s_rx_frame_len;
static uint8_t s_rx_frame_ready;
static rs485_rx_callback_t s_rx_cb;
static void *s_rx_cb_user;

static void rs485_rx_task(void *arg)
{
    EventBits_t ev;
    (void)arg;

    if (HAL_UARTEx_ReceiveToIdle_DMA(&huart3, s_rx_buf, RS485_RX_BUFFER_SIZE) != HAL_OK) {
        (void)xEventGroupSetBits(s_events, RS485_EV_RX_ERROR);
    }

    for (;;) {
        ev = xEventGroupWaitBits(s_events, RS485_EV_RX_NOTIFY | RS485_EV_RX_ERROR | RS485_EV_EXIT_REQ,
                                 pdTRUE, pdFALSE, portMAX_DELAY);
        if ((ev & RS485_EV_EXIT_REQ) != 0u) {
            break;
        }
        if ((ev & RS485_EV_RX_ERROR) != 0u) {
            (void)HAL_UART_AbortReceive(&huart3);
            (void)HAL_UARTEx_ReceiveToIdle_DMA(&huart3, s_rx_buf, RS485_RX_BUFFER_SIZE);
            continue;
        }
        if ((ev & RS485_EV_RX_NOTIFY) != 0u) {
            uint16_t n = s_rx_len;
            if (n > RS485_RX_BUFFER_SIZE) {
                n = RS485_RX_BUFFER_SIZE;
            }

            if (xSemaphoreTake(s_rx_frame_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                if (n > 0u) {
                    memcpy(s_rx_frame, s_rx_buf, n);
                }
                s_rx_frame_len = n;
                s_rx_frame_ready = 1u;
                (void)xSemaphoreGive(s_rx_frame_mutex);
                (void)xEventGroupSetBits(s_events, RS485_EV_RX_FRAME | RS485_EV_RX_COMPLETE);
            }

            if (s_rx_cb != NULL && n > 0u) {
                s_rx_cb(s_rx_buf, n, s_rx_cb_user);
            }
            (void)HAL_UARTEx_ReceiveToIdle_DMA(&huart3, s_rx_buf, RS485_RX_BUFFER_SIZE);
        }
    }

    (void)xEventGroupSetBits(s_events, RS485_EV_EXIT_ACK);
    vTaskDelete(NULL);
}

static int rs485_parse_cfg(const char *cfg, UART_InitTypeDef *init)
{
    if (cfg == NULL || init == NULL) {
        return SYS_ERR_INVALID_ARG;
    }
    if (strlen(cfg) != 3u) {
        return SYS_ERR_INVALID_FMT;
    }

    if (cfg[0] == '7') {
        init->WordLength = UART_WORDLENGTH_7B;
    } else if (cfg[0] == '8') {
        init->WordLength = UART_WORDLENGTH_8B;
    } else if (cfg[0] == '9') {
        init->WordLength = UART_WORDLENGTH_9B;
    } else {
        return SYS_ERR_INVALID_FMT;
    }

    if (cfg[1] == 'N' || cfg[1] == 'n') {
        init->Parity = UART_PARITY_NONE;
    } else if (cfg[1] == 'E' || cfg[1] == 'e') {
        init->Parity = UART_PARITY_EVEN;
    } else if (cfg[1] == 'O' || cfg[1] == 'o') {
        init->Parity = UART_PARITY_ODD;
    } else {
        return SYS_ERR_INVALID_FMT;
    }

    if (cfg[2] == '1') {
        init->StopBits = UART_STOPBITS_1;
    } else if (cfg[2] == '2') {
        init->StopBits = UART_STOPBITS_2;
    } else {
        return SYS_ERR_INVALID_FMT;
    }

    return SYS_OK;
}

int rs485_driver_init(uint32_t baudrate, const char *config)
{
    UART_InitTypeDef cfg;
    int ret;

    if (s_inited != 0u) {
        return SYS_ERR_INVALID_STATE;
    }
    if (baudrate < 1200u || baudrate > 1000000u) {
        return SYS_ERR_OUT_OF_RANGE;
    }

    s_events = xEventGroupCreate();
    if (s_events == NULL) {
        return SYS_ERR_NO_MEM;
    }
    s_tx_mutex = xSemaphoreCreateMutex();
    if (s_tx_mutex == NULL) {
        vEventGroupDelete(s_events);
        s_events = NULL;
        return SYS_ERR_NO_MEM;
    }
    s_rx_frame_mutex = xSemaphoreCreateMutex();
    if (s_rx_frame_mutex == NULL) {
        vSemaphoreDelete(s_tx_mutex);
        s_tx_mutex = NULL;
        vEventGroupDelete(s_events);
        s_events = NULL;
        return SYS_ERR_NO_MEM;
    }

    cfg = huart3.Init;
    cfg.BaudRate = baudrate;
    if (config != NULL) {
        ret = rs485_parse_cfg(config, &cfg);
        if (ret != SYS_OK) {
            rs485_driver_deinit();
            return ret;
        }
    }

    (void)HAL_UART_Abort(&huart3);
    (void)HAL_UART_DeInit(&huart3);
    huart3.Init = cfg;
    if (HAL_RS485Ex_Init(&huart3, UART_DE_POLARITY_HIGH, 0, 0) != HAL_OK) {
        rs485_driver_deinit();
        return SYS_ERR_HAL;
    }
    if (HAL_UARTEx_SetTxFifoThreshold(&huart3, UART_TXFIFO_THRESHOLD_1_8) != HAL_OK ||
        HAL_UARTEx_SetRxFifoThreshold(&huart3, UART_RXFIFO_THRESHOLD_1_8) != HAL_OK ||
        HAL_UARTEx_DisableFifoMode(&huart3) != HAL_OK) {
        rs485_driver_deinit();
        return SYS_ERR_HAL;
    }

    memset(s_rx_buf, 0, sizeof(s_rx_buf));
    memset(s_tx_buf, 0, sizeof(s_tx_buf));
    memset(s_rx_frame, 0, sizeof(s_rx_frame));
    s_rx_len = 0u;
    s_rx_frame_len = 0u;
    s_rx_frame_ready = 0u;
    s_rx_cb = NULL;
    s_rx_cb_user = NULL;

    if (xTaskCreate(rs485_rx_task, "rs485_rx", 1024, NULL, tskIDLE_PRIORITY + 2u, &s_rx_task) != pdPASS) {
        rs485_driver_deinit();
        return SYS_ERR_NO_MEM;
    }
    s_inited = 1u;
    return SYS_OK;
}

int rs485_driver_deinit(void)
{
    if (s_rx_task != NULL && s_events != NULL) {
        (void)xEventGroupSetBits(s_events, RS485_EV_EXIT_REQ);
        (void)xEventGroupWaitBits(s_events, RS485_EV_EXIT_ACK, pdTRUE, pdFALSE, pdMS_TO_TICKS(100));
        s_rx_task = NULL;
    }

    (void)HAL_UART_Abort(&huart3);
    if (s_inited != 0u) {
        (void)HAL_UART_DeInit(&huart3);
        MX_USART3_UART_Init();
    }
    s_inited = 0u;
    s_rx_len = 0u;
    s_rx_frame_ready = 0u;
    if (s_rx_frame_mutex != NULL) {
        vSemaphoreDelete(s_rx_frame_mutex);
        s_rx_frame_mutex = NULL;
    }
    if (s_tx_mutex != NULL) {
        vSemaphoreDelete(s_tx_mutex);
        s_tx_mutex = NULL;
    }
    if (s_events != NULL) {
        vEventGroupDelete(s_events);
        s_events = NULL;
    }
    return SYS_OK;
}

int rs485_driver_send(const uint8_t *data, uint16_t len, uint32_t timeout_ms)
{
    EventBits_t ev;

    if (s_inited == 0u) {
        return SYS_ERR_INVALID_STATE;
    }
    if (data == NULL || len == 0u || len > RS485_TX_BUFFER_SIZE) {
        return SYS_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(s_tx_mutex, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return SYS_ERR_TIMEOUT;
    }

    memcpy(s_tx_buf, data, len);
    (void)xEventGroupClearBits(s_events, RS485_EV_TX_COMPLETE);
    if (HAL_UART_Transmit_DMA(&huart3, s_tx_buf, len) != HAL_OK) {
        (void)xSemaphoreGive(s_tx_mutex);
        return SYS_ERR_HAL;
    }
    ev = xEventGroupWaitBits(s_events, RS485_EV_TX_COMPLETE, pdTRUE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    (void)xSemaphoreGive(s_tx_mutex);
    if ((ev & RS485_EV_TX_COMPLETE) == 0u) {
        (void)HAL_UART_AbortTransmit(&huart3);
        return SYS_ERR_TIMEOUT;
    }
    return SYS_OK;
}

int rs485_driver_recv(uint8_t *data, uint16_t data_len, uint16_t *out_len, uint32_t timeout_ms)
{
    EventBits_t ev;
    uint16_t rx_len = 0u;

    if (s_inited == 0u) {
        return SYS_ERR_INVALID_STATE;
    }
    if (data == NULL || out_len == NULL || data_len == 0u) {
        return SYS_ERR_INVALID_ARG;
    }
    *out_len = 0u;

    /* First consume already buffered frame to avoid clearing a valid pending RX event. */
    if (xSemaphoreTake(s_rx_frame_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        if (s_rx_frame_ready != 0u) {
            rx_len = s_rx_frame_len;
            if (rx_len > data_len) {
                rx_len = data_len;
            }
            if (rx_len > 0u) {
                memcpy(data, s_rx_frame, rx_len);
            }
            s_rx_frame_ready = 0u;
            (void)xSemaphoreGive(s_rx_frame_mutex);
            *out_len = rx_len;
            return SYS_OK;
        }
        (void)xSemaphoreGive(s_rx_frame_mutex);
    }

    ev = xEventGroupWaitBits(s_events, RS485_EV_RX_FRAME | RS485_EV_RX_ERROR, pdTRUE, pdFALSE, pdMS_TO_TICKS(timeout_ms));

    if ((ev & RS485_EV_RX_ERROR) != 0u) {
        return SYS_ERR_HAL;
    }
    if ((ev & RS485_EV_RX_FRAME) == 0u) {
        return SYS_ERR_TIMEOUT;
    }

    if (xSemaphoreTake(s_rx_frame_mutex, pdMS_TO_TICKS(20)) != pdTRUE) {
        return SYS_ERR_TIMEOUT;
    }
    if (s_rx_frame_ready == 0u) {
        (void)xSemaphoreGive(s_rx_frame_mutex);
        return SYS_ERR_TIMEOUT;
    }
    rx_len = s_rx_frame_len;
    if (rx_len > data_len) {
        rx_len = data_len;
    }
    if (rx_len > 0u) {
        memcpy(data, s_rx_frame, rx_len);
    }
    s_rx_frame_ready = 0u;
    (void)xSemaphoreGive(s_rx_frame_mutex);
    *out_len = rx_len;
    return SYS_OK;
}

int rs485_driver_txrx(const uint8_t *tx_data, uint16_t tx_len, uint8_t *rx_data, uint16_t rx_len,
                      uint16_t *out_len, uint32_t timeout_ms)
{
    int ret;

    ret = rs485_driver_send(tx_data, tx_len, timeout_ms);
    if (ret != SYS_OK) {
        return ret;
    }
    return rs485_driver_recv(rx_data, rx_len, out_len, timeout_ms);
}

int rs485_driver_is_inited(void)
{
    return (s_inited != 0u) ? 1 : 0;
}

int rs485_driver_set_rx_callback(rs485_rx_callback_t cb, void *user)
{
    if (s_inited == 0u) {
        return SYS_ERR_INVALID_STATE;
    }
    s_rx_cb = cb;
    s_rx_cb_user = user;
    return SYS_OK;
}

void rs485_driver_on_uart3_rx_event(UART_HandleTypeDef *huart, uint16_t size)
{
    BaseType_t hp = pdFALSE;

    if (huart == NULL || huart->Instance != USART3 || s_events == NULL || s_inited == 0u) {
        return;
    }
    s_rx_len = size;
    (void)xEventGroupSetBitsFromISR(s_events, RS485_EV_RX_NOTIFY, &hp);
    portYIELD_FROM_ISR(hp);
}

void rs485_driver_on_uart3_tx_done(UART_HandleTypeDef *huart)
{
    BaseType_t hp = pdFALSE;

    if (huart == NULL || huart->Instance != USART3 || s_events == NULL || s_inited == 0u) {
        return;
    }
    (void)xEventGroupSetBitsFromISR(s_events, RS485_EV_TX_COMPLETE, &hp);
    portYIELD_FROM_ISR(hp);
}

void rs485_driver_on_uart3_error(UART_HandleTypeDef *huart)
{
    BaseType_t hp = pdFALSE;

    if (huart == NULL || huart->Instance != USART3 || s_events == NULL || s_inited == 0u) {
        return;
    }
    (void)xEventGroupSetBitsFromISR(s_events, RS485_EV_RX_ERROR, &hp);
    portYIELD_FROM_ISR(hp);
}

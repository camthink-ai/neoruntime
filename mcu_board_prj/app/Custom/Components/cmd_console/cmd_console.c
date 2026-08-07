#include "cmd_console.h"
#include "nr_micro_shell.h"
#include "usart.h"

#define UART_EVENT_RX_READY      (1 << 0)
#define UART_EVENT_TX_COMPLETE   (1 << 1)
#define UART_EVENT_ERROR         (1 << 2)
#define UART_RX_INTERVAL_MS     20
#define UART_RX_DMA_BUF_SIZE    256
static HAL_StatusTypeDef rx_status = HAL_ERROR;
static uint8_t rx_dma_buf[UART_RX_DMA_BUF_SIZE] = {0};
static int rx_read_index = 0, rx_write_index = 0;
static EventGroupHandle_t uart_event_group = NULL;
static TaskHandle_t cmd_console_task_handle = NULL;

void HAL_UART1Ex_RxEventCallback(UART_HandleTypeDef *huart, uint16_t Size)
{
    xEventGroupSetBitsFromISR(uart_event_group, UART_EVENT_RX_READY, NULL);
}

void HAL_UART1_TxCpltCallback(UART_HandleTypeDef *huart)
{
    xEventGroupSetBitsFromISR(uart_event_group, UART_EVENT_TX_COMPLETE, NULL);
}

void HAL_UART1_ErrorCallback(UART_HandleTypeDef *huart)
{
    rx_status = HAL_ERROR;
    xEventGroupSetBitsFromISR(uart_event_group, UART_EVENT_ERROR, NULL);
}

void cmd_console_task(void *arg)
{
    uint32_t event_bits = 0;

    while (1) {
        event_bits = xEventGroupWaitBits(uart_event_group, UART_EVENT_RX_READY | UART_EVENT_ERROR, pdTRUE, pdFALSE, pdMS_TO_TICKS(UART_RX_INTERVAL_MS));
        if (event_bits & UART_EVENT_ERROR || rx_status != HAL_OK) {
            WIC_LOGE("[console_task] UART error(%d) or status(%d) error!", huart1.ErrorCode, rx_status);
            HAL_UART_Abort(&huart1);
            rx_read_index = 0;
            rx_write_index = 0;
            rx_status = HAL_UARTEx_ReceiveToIdle_DMA(&huart1, rx_dma_buf, UART_RX_DMA_BUF_SIZE);
            continue;
        }
        rx_write_index = UART_RX_DMA_BUF_SIZE - __HAL_DMA_GET_COUNTER(huart1.hdmarx);
        if (rx_write_index != rx_read_index) {
            if (rx_write_index > rx_read_index) {
                for (int i = rx_read_index; i < rx_write_index; i++) {
                    shell(rx_dma_buf[i]);
                }
            } else {
                for (int i = rx_read_index; i < UART_RX_DMA_BUF_SIZE; i++) {
                    shell(rx_dma_buf[i]);
                }
                for (int i = 0; i < rx_write_index; i++) {
                    shell(rx_dma_buf[i]);
                }
            }
            rx_read_index = rx_write_index;
        }
    }
}

int cmd_console_init(void)
{
    uart_event_group = xEventGroupCreate();
    if (uart_event_group == NULL) {
        WIC_LOGE("[console_init] Failed to create event group!");
        return SYS_ERR_NO_MEM;
    }

    rx_status = HAL_UARTEx_ReceiveToIdle_DMA(&huart1, rx_dma_buf, UART_RX_DMA_BUF_SIZE);
    if (rx_status != HAL_OK) {
        WIC_LOGE("[console_init] Failed to receive data!");
        return SYS_ERR_HAL;
    }

    if (xTaskCreate(cmd_console_task, CMD_CONSOLE_TASK_NAME, CMD_CONSOLE_TASK_STACK_SIZE, NULL, CMD_CONSOLE_TASK_PRIORITY, &cmd_console_task_handle) != pdPASS) {
        WIC_LOGE("[console_init] Failed to create console task!");
        return SYS_ERR_NO_MEM;
    }

    WIC_LOGD("[console_init] OK!");
    return SYS_OK;
}

int cmd_console_putc(char c)
{
    HAL_UART_Transmit(&huart1, (uint8_t *)&c, 1, 10);
    return SYS_OK;
}

int cmd_console_puts(const char *s)
{
    HAL_StatusTypeDef status = HAL_ERROR;
    uint16_t slen = 0;
    EventBits_t event_bits = 0;

    slen = strlen(s);
    if (slen > 0) {
        if (slen <= 4) {
            status = HAL_UART_Transmit(&huart1, (uint8_t *)s, slen, 10);
            if (status != HAL_OK) {
                WIC_LOGE("[console_puts] Failed to transmit data(status: %d)!", status);
                return SYS_ERR_HAL;
            }
        } else {
            xEventGroupClearBits(uart_event_group, UART_EVENT_TX_COMPLETE);
            status = HAL_UART_Transmit_DMA(&huart1, (uint8_t *)s, slen);
            if (status != HAL_OK) {
                WIC_LOGE("[console_puts] Failed to transmit data(status: %d)!", status);
                return SYS_ERR_HAL;
            }
            event_bits = xEventGroupWaitBits(uart_event_group, UART_EVENT_TX_COMPLETE, pdTRUE, pdFALSE, pdMS_TO_TICKS(100));
            if ((event_bits & UART_EVENT_TX_COMPLETE) == 0) {
                WIC_LOGE("[console_puts] Failed to wait for TX complete!");
                return SYS_ERR_TIMEOUT;
            }
        }
    }
    return SYS_OK;
}

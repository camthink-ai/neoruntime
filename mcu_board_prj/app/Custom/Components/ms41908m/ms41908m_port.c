#include "ms41908m_port.h"
#include "spi.h"
#include "timers.h"

static SemaphoreHandle_t s_mutex = NULL;
static EventGroupHandle_t s_event_group = NULL;

static void ms41908m_delay_us(uint32_t us)
{
    uint32_t cycles = us * (SystemCoreClock / 1000000U) / 5U;
    while (cycles--) {
        __NOP();
    }
}

static inline void ms41908m_cs_low(void)
{
    HAL_GPIO_WritePin(SPI1_CS_GPIO_Port, SPI1_CS_Pin, GPIO_PIN_RESET);
}

static inline void ms41908m_cs_high(void)
{
    HAL_GPIO_WritePin(SPI1_CS_GPIO_Port, SPI1_CS_Pin, GPIO_PIN_SET);
}

static void ms41908m_vd_pulse(GPIO_TypeDef *port, uint16_t pin)
{
    HAL_GPIO_WritePin(port, pin, GPIO_PIN_SET);
    ms41908m_delay_us(5);
    HAL_GPIO_WritePin(port, pin, GPIO_PIN_RESET);
}

int ms41908m_port_init(void)
{
    if (s_mutex != NULL || s_event_group != NULL) {
        return SYS_ERR_INVALID_STATE; /* Already initialized */
    }

    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) return SYS_ERR_NO_MEM;

    s_event_group = xEventGroupCreate();
    if (s_event_group == NULL) {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
        return SYS_ERR_NO_MEM;
    }

    // enable lens limit power
    HAL_GPIO_WritePin(LENS_EN_GPIO_Port, LENS_EN_Pin, GPIO_PIN_RESET);
    ms41908m_port_delay_ms(10);

    // reset chip
    HAL_GPIO_WritePin(RSTB_GPIO_Port, RSTB_Pin, GPIO_PIN_RESET);
    ms41908m_port_delay_ms(10);
    HAL_GPIO_WritePin(RSTB_GPIO_Port, RSTB_Pin, GPIO_PIN_SET);
    ms41908m_port_delay_ms(50);

    return SYS_OK;
}

void ms41908m_port_deinit(void)
{
    if (s_event_group != NULL) {
        vEventGroupDelete(s_event_group);
        s_event_group = NULL;
    }
    if (s_mutex != NULL) {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
    }

    /* No global callbacks to clear */
}

void ms41908m_port_delay_ms(uint32_t ms)
{
    osDelay(ms);
}

void ms41908m_port_delay_us(uint32_t us)
{
    ms41908m_delay_us(us);
}

int ms41908m_port_write(uint8_t addr, uint16_t data)
{
    uint8_t tx_buf[3];
    HAL_StatusTypeDef status;

    tx_buf[0] = (uint8_t)(addr & 0x3FU); /* C0=0: write, C1=0 */
    tx_buf[1] = (uint8_t)(data & 0xFFU);
    tx_buf[2] = (uint8_t)((data >> 8) & 0xFFU);

    ms41908m_cs_high();
    ms41908m_delay_us(1);

    status = HAL_SPI_Transmit(&hspi1, tx_buf, 3, MS41908M_SPI_TIMEOUT_MS);

    ms41908m_delay_us(1);
    ms41908m_cs_low();

    return (status == HAL_OK) ? SYS_OK : SYS_ERR_HAL;
}

int ms41908m_port_read(uint8_t addr, uint16_t *data)
{
    uint8_t tx_buf[3] = {0};
    uint8_t rx_buf[3] = {0};
    HAL_StatusTypeDef status;

    if (data == NULL) return SYS_ERR_INVALID_ARG;

    /* C0=1: read, C1=0 */
    tx_buf[0] = (uint8_t)((addr & 0x3FU) | 0x40U);

    ms41908m_cs_high();
    ms41908m_delay_us(1);

    status = HAL_SPI_TransmitReceive(&hspi1, tx_buf, rx_buf, 3, MS41908M_SPI_TIMEOUT_MS);

    ms41908m_delay_us(1);
    ms41908m_cs_low();

    if (status != HAL_OK) {
        return SYS_ERR_HAL;
    }

    *data = (uint16_t)rx_buf[1] | ((uint16_t)rx_buf[2] << 8);
    return SYS_OK;
}

void ms41908m_port_output_vd(ms41908m_type_t type)
{
    switch (type) {
        case MS41908M_TYPE_IRIS:
            /* VD_IS: iris sync */
            ms41908m_vd_pulse(VD_IS_GPIO_Port, VD_IS_Pin);
            break;
        case MS41908M_TYPE_FOCUS:
        case MS41908M_TYPE_ZOOM:
            /* VD_FZ: focus/zoom sync */
            ms41908m_vd_pulse(VD_FZ_GPIO_Port, VD_FZ_Pin);
            break;
        default:
            break;
    }
}

uint16_t ms41908m_port_read_irq_status(ms41908m_irq_type_t type)
{
    GPIO_PinState st = GPIO_PIN_RESET;

    switch (type) {
        case MS41908M_IRQ_PLS1:
            st = HAL_GPIO_ReadPin(PLS1_GPIO_Port, PLS1_Pin);
            break;
        case MS41908M_IRQ_PLS2:
            st = HAL_GPIO_ReadPin(PLS2_GPIO_Port, PLS2_Pin);
            break;
        case MS41908M_IRQ_PI_ZOOM:
            st = HAL_GPIO_ReadPin(Z_RST_GPIO_Port, Z_RST_Pin);
            break;
        case MS41908M_IRQ_PI_FOCUS:
            st = HAL_GPIO_ReadPin(F_RST_GPIO_Port, F_RST_Pin);
            break;
        default:
            st = GPIO_PIN_RESET;
            break;
    }

    return (st == GPIO_PIN_SET) ? 1U : 0U;
}

void ms41908m_port_irq_handler(uint16_t irq_pin)
{
    BaseType_t hp_task_woken = pdFALSE;

    if (s_event_group == NULL) return;

    if (irq_pin == PLS1_Pin) {
        xEventGroupSetBitsFromISR(s_event_group, MS41908M_EVENT_PLS1, &hp_task_woken);
    } else if (irq_pin == PLS2_Pin) {
        xEventGroupSetBitsFromISR(s_event_group, MS41908M_EVENT_PLS2, &hp_task_woken);
    } else if (irq_pin == Z_RST_Pin) {
        xEventGroupSetBitsFromISR(s_event_group, MS41908M_EVENT_PI_ZOOM, &hp_task_woken);
    } else if (irq_pin == F_RST_Pin) {
        xEventGroupSetBitsFromISR(s_event_group, MS41908M_EVENT_PI_FOCUS, &hp_task_woken);
    }

    portYIELD_FROM_ISR(hp_task_woken);
}

int ms41908m_port_lock(void)
{
    if (s_mutex == NULL) return SYS_ERR_INVALID_STATE;
    return (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(200)) == pdTRUE) ? SYS_OK : SYS_ERR_MUTEX;
}

int ms41908m_port_unlock(void)
{
    if (s_mutex == NULL) return SYS_ERR_INVALID_STATE;
    return (xSemaphoreGive(s_mutex) == pdTRUE) ? SYS_OK : SYS_ERR_MUTEX;
}

uint32_t ms41908m_port_wait_for_event(uint32_t event_bits,
                                         uint32_t timeout_ms,
                                         uint8_t clear_on_exit,
                                         uint8_t wait_all)
{
    EventBits_t bits;

    if (s_event_group == NULL) return 0U;

    bits = xEventGroupWaitBits(s_event_group,
                              (EventBits_t)event_bits,
                              clear_on_exit ? pdTRUE : pdFALSE,
                              wait_all ? pdTRUE : pdFALSE,
                              pdMS_TO_TICKS(timeout_ms));
    return ((bits & event_bits) != 0U) ? ((uint32_t)bits) : 0U;
}

int ms41908m_port_set_event(uint32_t event_bits, uint8_t is_from_isr)
{
    if (s_event_group == NULL) return SYS_ERR_INVALID_STATE;

    if (is_from_isr) {
        BaseType_t hp_task_woken = pdFALSE;
        xEventGroupSetBitsFromISR(s_event_group, (EventBits_t)event_bits, &hp_task_woken);
        portYIELD_FROM_ISR(hp_task_woken);
    } else {
        xEventGroupSetBits(s_event_group, (EventBits_t)event_bits);
    }

    return SYS_OK;
}

void ms41908m_port_clear_event(uint32_t event_bits)
{
    if (s_event_group == NULL) return;

    /* FreeRTOS doesn't provide clear-from-isr; this wrapper is ISR-safe by design usage. */
    xEventGroupClearBits(s_event_group, (EventBits_t)event_bits);
}

/* ========================================================================== */
/* Timer helpers                                                              */
/* ========================================================================== */

static void ms41908m_timer_callback_trampoline(TimerHandle_t xTimer)
{
    void (*user_cb)(void *) = (void (*)(void *))pvTimerGetTimerID(xTimer);
    if (user_cb != NULL) {
        user_cb((void *)xTimer);
    }
}

void *ms41908m_port_create_timer(void (*callback)(void *timer), uint32_t period_ms)
{
    TimerHandle_t h = xTimerCreate(MS41908M_TIMER_NAME,
                                   pdMS_TO_TICKS(period_ms),
                                   pdTRUE,
                                   (void *)callback,
                                   ms41908m_timer_callback_trampoline);
    return (void *)h;
}

int ms41908m_port_delete_timer(void *timer)
{
    if (timer == NULL) return SYS_ERR_INVALID_ARG;
    return (xTimerDelete((TimerHandle_t)timer, 0) == pdPASS) ? SYS_OK : SYS_ERR_FAILED;
}

int ms41908m_port_start_timer(void *timer)
{
    if (timer == NULL) return SYS_ERR_INVALID_ARG;
    return (xTimerStart((TimerHandle_t)timer, 0) == pdPASS) ? SYS_OK : SYS_ERR_FAILED;
}

int ms41908m_port_stop_timer(void *timer)
{
    if (timer == NULL) return SYS_ERR_INVALID_ARG;
    return (xTimerStop((TimerHandle_t)timer, 0) == pdPASS) ? SYS_OK : SYS_ERR_FAILED;
}

/* ========================================================================== */
/* Task helpers                                                               */
/* ========================================================================== */

void *ms41908m_port_create_task(void (*task_handler)(void *arg), void *arg)
{
    TaskHandle_t handle = NULL;
    if (task_handler == NULL) return NULL;

    if (xTaskCreate(task_handler,
                    MS41908M_TASK_NAME,
                    MS41908M_TASK_STACK_SIZE,
                    arg,
                    MS41908M_TASK_PRIORITY,
                    &handle) != pdPASS) {
        return NULL;
    }

    return (void *)handle;
}

int ms41908m_port_delete_task(void *task)
{
    if (task == NULL) return SYS_ERR_INVALID_ARG;
    vTaskDelete((TaskHandle_t)task);
    return SYS_OK;
}

/* ========================================================================== */
/* PI (zero-point) IRQ gate — only enabled during reset-zero                  */
/* ========================================================================== */

void ms41908m_port_pi_irq_enable(void)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};

    /* Clear any stale PI event bits before enabling */
    if (s_event_group != NULL) {
        xEventGroupClearBits(s_event_group, MS41908M_EVENT_PI_ZOOM | MS41908M_EVENT_PI_FOCUS);
    }
    
    GPIO_InitStruct.Pin = Z_RST_Pin | F_RST_Pin;
    GPIO_InitStruct.Mode = GPIO_MODE_IT_FALLING;
    GPIO_InitStruct.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(GPIOD, &GPIO_InitStruct);
}

void ms41908m_port_pi_irq_disable(void)
{
    HAL_GPIO_DeInit(GPIOD, Z_RST_Pin | F_RST_Pin);
    /* Clear any pending PI event bits */
    if (s_event_group != NULL) {
        xEventGroupClearBits(s_event_group, MS41908M_EVENT_PI_ZOOM | MS41908M_EVENT_PI_FOCUS);
    }
}

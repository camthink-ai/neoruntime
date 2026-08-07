#ifndef __MS41908M_PORT_H__
#define __MS41908M_PORT_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "sys_config.h"

/* Task and timer definitions */
#define MS41908M_TASK_NAME              "ms41908m_task"
#define MS41908M_TASK_STACK_SIZE        (1536)
#define MS41908M_TASK_PRIORITY          (7)
#define MS41908M_TIMER_NAME             "ms41908m_timer"

/* SPI timeout (ms) */
#define MS41908M_SPI_TIMEOUT_MS         (10U)

/* Event bits for default IRQ mapping */
#define MS41908M_EVENT_PLS1             (1UL << 0)
#define MS41908M_EVENT_PLS2             (1UL << 1)
#define MS41908M_EVENT_PI_FOCUS         (1UL << 2)
#define MS41908M_EVENT_PI_ZOOM          (1UL << 3)
#define MS41908M_EVENT_FOCUS_COMPLETED  (1UL << 4)
#define MS41908M_EVENT_ZOOM_COMPLETED   (1UL << 5)
#define MS41908M_EVENT_REQ_RESET_ZOOM   (1UL << 6)
#define MS41908M_EVENT_REQ_RESET_FOCUS  (1UL << 7)
#define MS41908M_EVENT_ZOOM_RESET_DONE  (1UL << 8)
#define MS41908M_EVENT_FOCUS_RESET_DONE (1UL << 9)

/* Type definitions */
typedef enum {
    MS41908M_TYPE_FOCUS = 0,
    MS41908M_TYPE_ZOOM = 1,
    MS41908M_TYPE_IRIS = 2,
    MS41908M_TYPE_MAX = 3,
} ms41908m_type_t;

typedef enum {
    MS41908M_IRQ_PLS1 = 0,
    MS41908M_IRQ_PLS2 = 1,
    MS41908M_IRQ_PI_FOCUS = 2,
    MS41908M_IRQ_PI_ZOOM = 3,
    MS41908M_IRQ_MAX = 4,
} ms41908m_irq_type_t;

int ms41908m_port_init(void);
void ms41908m_port_deinit(void);

void ms41908m_port_delay_ms(uint32_t ms);
void ms41908m_port_delay_us(uint32_t us);

int ms41908m_port_write(uint8_t addr, uint16_t data);
int ms41908m_port_read(uint8_t addr, uint16_t *data);

void ms41908m_port_output_vd(ms41908m_type_t type);

uint16_t ms41908m_port_read_irq_status(ms41908m_irq_type_t type);
void ms41908m_port_irq_handler(uint16_t irq_pin);
void ms41908m_port_pi_irq_enable(void);   /* Enable PI (zero-point) IRQ — call before reset-zero */
void ms41908m_port_pi_irq_disable(void);  /* Disable PI IRQ — call after reset-zero completes */

int ms41908m_port_lock(void);
int ms41908m_port_unlock(void);

uint32_t ms41908m_port_wait_for_event(uint32_t event_bits, uint32_t timeout_ms, uint8_t clear_on_exit, uint8_t wait_all);
int ms41908m_port_set_event(uint32_t event_bits, uint8_t is_from_isr);
void ms41908m_port_clear_event(uint32_t event_bits);

void *ms41908m_port_create_timer(void (*callback)(void *timer), uint32_t period_ms);
int ms41908m_port_delete_timer(void *timer);
int ms41908m_port_start_timer(void *timer);
int ms41908m_port_stop_timer(void *timer);

void *ms41908m_port_create_task(void (*task_handler)(void *arg), void *arg);
int ms41908m_port_delete_task(void *task);

#ifdef __cplusplus
}
#endif

#endif /* __MS41908M_PORT_H__ */

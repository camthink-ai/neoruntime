/**
 * @file host_link_port.h
 * @brief OS porting hooks for host_link (implement one of *_freertos.c or *_posix.c).
 */
#ifndef HOST_LINK_PORT_H
#define HOST_LINK_PORT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void *host_link_mutex_t;
typedef void *host_link_sem_t;

void *host_link_port_malloc(size_t size);
void host_link_port_free(void *ptr);

uint32_t host_link_port_tick_ms(void);
/** @return diff in ms handling tick wrap (like osKernelGetTickCount wrap). */
uint32_t host_link_port_tick_diff_ms(uint32_t start_ms, uint32_t now_ms);

void host_link_port_delay_ms(uint32_t ms);

host_link_mutex_t host_link_port_mutex_create(void);
void host_link_port_mutex_destroy(host_link_mutex_t m);
void host_link_port_mutex_lock(host_link_mutex_t m);
void host_link_port_mutex_unlock(host_link_mutex_t m);

/** Binary semaphore: create empty (wait blocks until post). */
host_link_sem_t host_link_port_sem_create(void);
void host_link_port_sem_destroy(host_link_sem_t s);
/** @return 0 if signaled, non-zero on timeout */
int host_link_port_sem_wait(host_link_sem_t s, uint32_t timeout_ms);
void host_link_port_sem_post(host_link_sem_t s);

#ifdef __cplusplus
}
#endif

#endif /* HOST_LINK_PORT_H */

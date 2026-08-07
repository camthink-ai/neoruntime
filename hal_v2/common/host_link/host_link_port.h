/**
 * @file host_link_port.h
 * @brief Porting layer for host_link (POSIX for V2 hailo15 host).
 */
#pragma once

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
uint32_t host_link_port_tick_diff_ms(uint32_t start_ms, uint32_t now_ms);
void host_link_port_delay_ms(uint32_t ms);

host_link_mutex_t host_link_port_mutex_create(void);
void host_link_port_mutex_destroy(host_link_mutex_t m);
void host_link_port_mutex_lock(host_link_mutex_t m);
void host_link_port_mutex_unlock(host_link_mutex_t m);

host_link_sem_t host_link_port_sem_create(void);
void host_link_port_sem_destroy(host_link_sem_t sem);
int host_link_port_sem_wait(host_link_sem_t sem, uint32_t timeout_ms);
void host_link_port_sem_post(host_link_sem_t sem);

#ifdef __cplusplus
}
#endif


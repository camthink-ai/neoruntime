/**
 * @file host_link_port_freertos.c
 * @brief CMSIS-OS2 / FreeRTOS port for host_link (STM32CubeIDE).
 */
#include "host_link_port.h"
#include "cmsis_os2.h"
#include "FreeRTOS.h"
#include <stdint.h>
#include <stdlib.h>

void *host_link_port_malloc(size_t size)
{
    return pvPortMalloc(size);
}

void host_link_port_free(void *ptr)
{
    vPortFree(ptr);
}

uint32_t host_link_port_tick_ms(void)
{
    return osKernelGetTickCount();
}

uint32_t host_link_port_tick_diff_ms(uint32_t start_ms, uint32_t now_ms)
{
    if (now_ms >= start_ms) {
        return now_ms - start_ms;
    }
    return (uint32_t)(UINT32_MAX - start_ms + now_ms + 1u);
}

void host_link_port_delay_ms(uint32_t ms)
{
    (void)osDelay(ms);
}

host_link_mutex_t host_link_port_mutex_create(void)
{
    return (host_link_mutex_t)osMutexNew(NULL);
}

void host_link_port_mutex_destroy(host_link_mutex_t m)
{
    if (m != NULL) {
        (void)osMutexDelete((osMutexId_t)m);
    }
}

void host_link_port_mutex_lock(host_link_mutex_t m)
{
    if (m != NULL) {
        (void)osMutexAcquire((osMutexId_t)m, osWaitForever);
    }
}

void host_link_port_mutex_unlock(host_link_mutex_t m)
{
    if (m != NULL) {
        (void)osMutexRelease((osMutexId_t)m);
    }
}

host_link_sem_t host_link_port_sem_create(void)
{
    return (host_link_sem_t)osSemaphoreNew(1u, 0u, NULL);
}

void host_link_port_sem_destroy(host_link_sem_t s)
{
    if (s != NULL) {
        (void)osSemaphoreDelete((osSemaphoreId_t)s);
    }
}

int host_link_port_sem_wait(host_link_sem_t s, uint32_t timeout_ms)
{
    if (s == NULL) {
        return -1;
    }
    osStatus_t st = osSemaphoreAcquire((osSemaphoreId_t)s, timeout_ms);
    return (st == osOK) ? 0 : -1;
}

void host_link_port_sem_post(host_link_sem_t s)
{
    if (s != NULL) {
        (void)osSemaphoreRelease((osSemaphoreId_t)s);
    }
}

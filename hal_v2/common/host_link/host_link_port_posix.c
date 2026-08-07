/**
 * @file host_link_port_posix.c
 * @brief POSIX port (Linux) for host_link.
 */
#include "host_link_port.h"

#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

void *host_link_port_malloc(size_t size)
{
    return malloc(size);
}

void host_link_port_free(void *ptr)
{
    free(ptr);
}

uint32_t host_link_port_tick_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0u;
    }
    return (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
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
    struct timespec ts;
    ts.tv_sec = (time_t)(ms / 1000u);
    ts.tv_nsec = (long)((ms % 1000u) * 1000000u);
    (void)nanosleep(&ts, NULL);
}

typedef struct {
    pthread_mutex_t mtx;
} host_link_pthread_mutex_wrap_t;

host_link_mutex_t host_link_port_mutex_create(void)
{
    host_link_pthread_mutex_wrap_t *w = (host_link_pthread_mutex_wrap_t *)malloc(sizeof(*w));
    if (w == NULL) {
        return NULL;
    }
    if (pthread_mutex_init(&w->mtx, NULL) != 0) {
        free(w);
        return NULL;
    }
    return (host_link_mutex_t)w;
}

void host_link_port_mutex_destroy(host_link_mutex_t m)
{
    if (m == NULL) {
        return;
    }
    host_link_pthread_mutex_wrap_t *w = (host_link_pthread_mutex_wrap_t *)m;
    (void)pthread_mutex_destroy(&w->mtx);
    free(w);
}

void host_link_port_mutex_lock(host_link_mutex_t m)
{
    if (m != NULL) {
        (void)pthread_mutex_lock(&((host_link_pthread_mutex_wrap_t *)m)->mtx);
    }
}

void host_link_port_mutex_unlock(host_link_mutex_t m)
{
    if (m != NULL) {
        (void)pthread_mutex_unlock(&((host_link_pthread_mutex_wrap_t *)m)->mtx);
    }
}

typedef struct {
    sem_t sem;
} host_link_posix_sem_t;

host_link_sem_t host_link_port_sem_create(void)
{
    host_link_posix_sem_t *s = (host_link_posix_sem_t *)malloc(sizeof(*s));
    if (s == NULL) {
        return NULL;
    }
    if (sem_init(&s->sem, 0, 0) != 0) {
        free(s);
        return NULL;
    }
    return (host_link_sem_t)s;
}

void host_link_port_sem_destroy(host_link_sem_t sem)
{
    if (sem == NULL) {
        return;
    }
    host_link_posix_sem_t *s = (host_link_posix_sem_t *)sem;
    (void)sem_destroy(&s->sem);
    free(s);
}

int host_link_port_sem_wait(host_link_sem_t sem, uint32_t timeout_ms)
{
    host_link_posix_sem_t *s = (host_link_posix_sem_t *)sem;
    if (s == NULL) {
        return -1;
    }
    if (timeout_ms == UINT32_MAX) {
        return (sem_wait(&s->sem) == 0) ? 0 : -1;
    }
    if (timeout_ms == 0u) {
        return (sem_trywait(&s->sem) == 0) ? 0 : -1;
    }
    struct timespec abstime;
    if (clock_gettime(CLOCK_REALTIME, &abstime) != 0) {
        return -1;
    }
    uint64_t add_ms = timeout_ms;
    abstime.tv_sec += (time_t)(add_ms / 1000u);
    abstime.tv_nsec += (long)((add_ms % 1000u) * 1000000u);
    if (abstime.tv_nsec >= 1000000000L) {
        abstime.tv_sec++;
        abstime.tv_nsec -= 1000000000L;
    }
    return (sem_timedwait(&s->sem, &abstime) == 0) ? 0 : -1;
}

void host_link_port_sem_post(host_link_sem_t sem)
{
    host_link_posix_sem_t *s = (host_link_posix_sem_t *)sem;
    if (s == NULL) {
        return;
    }
    (void)sem_post(&s->sem);
}


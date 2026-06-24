/**
 * @file    osal.c
 * @brief   操作系统适配层 — Linux 实现
 *
 * 整个协议栈只有这个文件感知操作系统。
 * 移植到 FreeRTOS / 裸机时，只需替换此文件。
 */

#include "../include/osal.h"
#include <stdlib.h>    /* malloc, free */
#include <sys/time.h>  /* gettimeofday */

void osal_init(void)
{
    /* Linux 下无需额外初始化，留空 */
}

void *osal_malloc(size_t size)
{
    return malloc(size);
}

void osal_free(void *ptr)
{
    free(ptr);
}

uint32_t osal_get_time_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint32_t)(tv.tv_sec * 1000 + tv.tv_usec / 1000);
}

osal_critical_t osal_critical_enter(void)
{
    return NULL; /* 单线程轮询，无需关中断 */
}

void osal_critical_exit(osal_critical_t crit)
{
    (void)crit; /* 空实现 */
}

osal_mutex_t osal_mutex_create(void)
{
    return NULL; /* 单线程，暂不需要锁 */
}

void osal_mutex_lock(osal_mutex_t mutex)
{
    (void)mutex;
}

void osal_mutex_unlock(osal_mutex_t mutex)
{
    (void)mutex;
}

void osal_mutex_delete(osal_mutex_t mutex)
{
    (void)mutex;
}

osal_sem_t osal_sem_create(uint32_t max_count)
{
    (void)max_count;
    return NULL;
}

void osal_sem_delete(osal_sem_t sem)
{
    (void)sem;
}

int osal_sem_wait(osal_sem_t sem, uint32_t timeout)
{
    (void)sem;
    (void)timeout;
    return 0;
}

void osal_sem_signal(osal_sem_t sem)
{
    (void)sem;
}

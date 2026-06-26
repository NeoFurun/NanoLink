#include "../include/osal.h"
#include <stdlib.h> 
#include <time.h>
#include <linux/time.h>

void osal_init(void)
{
    /* Linux 下无需额外初始化，留空 */
}

/*分配内存空间*/
void *osal_malloc(size_t size)
{
    return malloc(size);
}

/*释放内存空间*/
void osal_free(void *ptr)
{
    free(ptr);
}

/*
    通过下面示例 获取程序执行时间
    uint32_t start_time = osal_get_time_ms(); // 获取开始时间
    example_function();                      // 执行目标函数
    uint32_t end_time = osal_get_time_ms();  // 获取结束时间
*/
uint32_t osal_get_time_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/*进入临界区*/
osal_critical_t osal_critical_enter(void)
{
    return NULL; /* 单线程轮询，无需关中断 */
}

/*退出临界区*/
void osal_critical_exit(osal_critical_t crit)
{
    (void)crit; /* 空实现 */
}

/*创建一把锁*/
osal_mutex_t osal_mutex_create(void)
{
    return NULL; /* 单线程，暂不需要锁 */
}

/*加锁*/
void osal_mutex_lock(osal_mutex_t mutex)
{
    (void)mutex;
}

/*解锁*/
void osal_mutex_unlock(osal_mutex_t mutex)
{
    (void)mutex;
}

/*删除锁*/
void osal_mutex_delete(osal_mutex_t mutex)
{
    (void)mutex;
}

/*创建信号量*/
osal_sem_t osal_sem_create(uint32_t max_count)
{
    (void)max_count;
    return NULL;
}

/*删除信号量*/
void osal_sem_delete(osal_sem_t sem)
{
    (void)sem;
}

/*等待信号量，timeout为等待时间，单位为毫秒*/
int osal_sem_wait(osal_sem_t sem, uint32_t timeout)
{
    (void)sem;
    (void)timeout;
    return 0;
}

/*发送信号量*/
void osal_sem_signal(osal_sem_t sem)
{
    (void)sem;
}

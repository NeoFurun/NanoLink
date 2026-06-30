#ifndef TIMER_H
#define TIMER_H

#include <stdint.h>

/* -------------------------------------------------------------------------- */
/*  预定义常量                                                                */
/* -------------------------------------------------------------------------- */

#define TIMER_MAX_COUNT 32 /**< 最大定时器数量 */

/* 定时器类型 */
#define TIMER_TYPE_ONCE 0     /**< 单次定时器，到期后自动停止 */
#define TIMER_TYPE_PERIODIC 1 /**< 周期定时器，到期后自动重新装载 */

/* -------------------------------------------------------------------------- */
/*  类型定义                                                                  */
/* -------------------------------------------------------------------------- */

/** 定时器句柄（不透明指针） */
typedef struct timer *timer_handle_t;

/** 定时器回调函数类型 */
typedef void (*timer_callback_fn)(void *arg);

/** 定时器结构体（内部实现，外部不直接访问字段） */
struct timer
{
    uint32_t interval;          /**< 定时周期（毫秒） */
    uint32_t remaining;         /**< 剩余时间（毫秒） */
    uint8_t type;               /**< TIMER_TYPE_ONCE 或 PERIODIC */
    uint8_t active;             /**< 是否激活 */
    timer_callback_fn callback; /**< 回调函数 */
    void *arg;                  /**< 回调参数 */
    struct timer *next;         /**< 链表指针 */
};


//初始化定时器模块。
void timer_init(void);

//创建一个定时器
timer_handle_t timer_create(uint32_t interval, uint8_t type,
                            timer_callback_fn callback, void *arg);

//启动（或重新启动）定时器
void timer_start(timer_handle_t handle);

//停止定时器（不删除）
void timer_stop(timer_handle_t handle);

//删除定时器
void timer_delete(timer_handle_t handle);

//检查定时器是否激活
int timer_is_active(timer_handle_t handle);

//修改定时器周期（下次到期后生效）。
void timer_set_interval(timer_handle_t handle, uint32_t interval);

//获取定时器剩余时间。
uint32_t timer_get_remaining(timer_handle_t handle);

//定时器驱动函数：由外部周期性调用（如每 1ms 或 10ms）。
//内部遍历所有激活定时器，递减计数，到期时调用回调。
//调用频率决定了定时精度。
void timer_tick(uint32_t elapsed_ms);

#endif /* TIMER_H */
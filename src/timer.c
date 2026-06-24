/**
 * @file    timer.c
 * @brief   定时器服务模块实现
 *
 * 提供单次和周期定时器，供 TCP 重传、ARP 老化、IP 分片重组超时等模块使用。
 * 外部周期性调用 timer_tick() 驱动，不依赖特定时钟源——谁调它、调多快，
 * 由用户决定（主循环 10ms 一次、硬件定时中断 1ms 一次都可以）。
 *
 * 架构：静态池 + 双链表
 *   free_list   — 空闲定时器，初始化时 32 个全串在这
 *   active_list — 正在倒计时的定时器，timer_tick 遍历的就是它
 *
 *   定时器在两条链表之间流转：
 *   free_list → [timer_create] → 句柄 → [timer_start] → active_list
 *   active_list → [timer_tick 到期(ONCE)] → free_list
 *   active_list → [timer_stop] → 游离 → [timer_delete] → free_list
 */

#include "../include/timer.h"
#include <stddef.h>

static struct timer timer_pool[TIMER_MAX_COUNT];
static struct timer *free_list   = NULL;  /**< 空闲定时器链表 */
static struct timer *active_list = NULL;  /**< 工作中的定时器链表 */

/* ==========================================================================
   timer_init — 初始化定时器模块（协议栈启动时调用一次）
   ========================================================================== */
/**
 * 把 timer_pool[32] 用 next 指针串成单向空闲链表。
 *
 * 例（简化为 4 个）:
 *   调用前: timer_pool = [t0] [t1] [t2] [t3]  (杂乱)
 *   调用后: free_list → t0 → t1 → t2 → t3 → NULL
 *          active_list = NULL
 */
void timer_init(void)
{
    int i;
    for (i = 0; i < TIMER_MAX_COUNT - 1; i++) {
        timer_pool[i].next = &timer_pool[i + 1];
    }
    timer_pool[TIMER_MAX_COUNT - 1].next = NULL;
    free_list   = &timer_pool[0];
    active_list = NULL;
}

/* ==========================================================================
   timer_create — 从池中分配一个定时器（不启动）
   ========================================================================== */
/**
 * 例（TCP 连接建立后创建重传定时器）:
 *   timer_handle_t retrans = timer_create(500,             // 500ms 超时
 *                                         TIMER_TYPE_ONCE, // 单次
 *                                         tcp_retransmit,  // 回调函数
 *                                         pcb);            // 回调参数
 *   // retrans->interval=500, remaining=0, active=0, type=ONCE
 *   // 此时定时器还没开始跑，需要调 timer_start(retrans) 才会倒计时。
 *
 *   如果 32 个定时器全部在外面没归还，返回 NULL。
 */
timer_handle_t timer_create(uint32_t interval, uint8_t type,
                             timer_callback_fn callback, void *arg)
{
    struct timer *t;

    if (type != TIMER_TYPE_ONCE && type != TIMER_TYPE_PERIODIC) return NULL;

    if (free_list == NULL) return NULL;

    t = free_list;
    free_list = t->next;

    t->interval  = interval;
    t->remaining = 0;
    t->type      = type;
    t->active    = 0;
    t->callback  = callback;
    t->arg       = arg;
    t->next      = NULL;

    return t;
}

/* ==========================================================================
   timer_start — 启动（或重新启动）定时器
   ========================================================================== */
/**
 * remaining 重新装填为 interval，active 置 1，插入 active_list 头部。
 *
 * 例（TCP 发出 SYN 后启动重传定时器）:
 *   发完 SYN 包，期望 500ms 内收到 SYN-ACK:
 *     timer_start(retrans);
 *   此时 retrans->remaining=500, active=1。
 *
 *   如果 500ms 后没收到 ACK，回调触发重传，然后再次 timer_start：
 *     timer_start(retrans);
 *   倒计时重新从 500ms 开始。
 */
void timer_start(timer_handle_t handle)
{
    struct timer *t = (struct timer *)handle;

    if (t == NULL) return;

    /* If already active, stop first to avoid creating a circular list
     * (timer would appear in active_list twice). */
    if (t->active) {
        timer_stop(t);
    }

    t->remaining = t->interval;
    t->active    = 1;

    t->next  = active_list;
    active_list = t;
}

/* ==========================================================================
   timer_tick — 定时器驱动函数，由外部周期性调用
   ========================================================================== */
/**
 * 遍历 active_list，每个激活定时器的 remaining 减 elapsed_ms。
 * 到期则触发回调：ONCE 归池，PERIODIC 自动重新装载。
 *
 * 例（主循环每 10ms 调一次）:
 *   active_list: t0(TCP重传, rem=500, ONCE) → t2(ARP老化, rem=60000, PERIODIC)
 *
 *   timer_tick(10):  // 第一次
 *     t0: 500→490  未到期
 *     t2: 60000→59990 未到期
 *
 *   ... 490ms 后（共调了 50 次）...
 *
 *   timer_tick(10):  // 第 50 次
 *     t0: 10→0  ★ 到期!
 *       → 调 t0.callback(pcb)  // 重传 TCP 包
 *       → t0 是 ONCE → 从 active_list 拆下，归还 free_list
 *     t2: 10010→10000  未到期
 *
 *   ... 很久以后 t2 也到期 ...
 *     → 调 t2.callback(NULL)  // 扫描 ARP 表
 *     → t2 是 PERIODIC → remaining 重新装 60000，继续跑
 */
void timer_tick(uint32_t elapsed_ms)
{
    struct timer *t, *prev;

    prev = NULL;
    t = active_list;
    while (t != NULL) {
        if (t->remaining > elapsed_ms) {
            t->remaining -= elapsed_ms;
            prev = t;
            t = t->next;
        } else {
            struct timer *current = t;
            struct timer *next   = t->next;

            current->remaining = 0;
            current->active    = 0;

            /* Remove current from active_list */
            if (prev == NULL) {
                active_list = next;
            } else {
                prev->next = next;
            }

            /* Fire the callback */
            if (current->callback != NULL) {
                current->callback(current->arg);
            }

            /* Bug 4 fix: the callback may have re-armed this timer by calling
             * timer_start(). If current->active is still 1, the timer is
             * already back in active_list — do NOT return it to free_list or
             * re-start it. Just advance to next. */
            if (current->active) {
                t = next;
                continue;
            }

            /* Timer was not re-armed. Handle expiration normally. */
            if (current->type == TIMER_TYPE_ONCE) {
                current->next = free_list;
                free_list = current;
            } else {
                timer_start(current);
            }

            /* Bug 7 fix: the callback may have deleted the next timer via
             * timer_delete(). Verify that `next` is still in active_list
             * before dereferencing its fields in the next iteration. */
            if (next != NULL) {
                struct timer *scan = active_list;
                int found = 0;
                while (scan != NULL) {
                    if (scan == next) {
                        found = 1;
                        break;
                    }
                    scan = scan->next;
                }
                if (!found) {
                    /* next was deleted by the callback; stop iterating */
                    break;
                }
            }

            t = next;
        }
    }
}

/* ==========================================================================
   timer_stop — 停止定时器，但不归还池
   ========================================================================== */
/**
 * 从 active_list 摘下，active=0。句柄保留，后续可 restart 或 delete。
 *
 * 例（TCP 收到 ACK，取消重传定时器）:
 *   active_list: t0(ARP, rem=300) → t1(TCP重传, rem=150) → t2(老化, rem=50000)
 *   timer_stop(t1):
 *     active_list: t0(rem=300) → t2(rem=50000)
 *     t1 游离，active=0，不再倒计时。
 *   随后可以 timer_start(t1) 重新用，也可以 timer_delete(t1) 彻底还池。
 */
void timer_stop(timer_handle_t handle)
{
    struct timer *target = (struct timer *)handle;
    struct timer *t, *prev;

    if (target == NULL || target->active == 0) return;

    prev = NULL;
    t = active_list;
    while (t != NULL && t != target) {
        prev = t;
        t = t->next;
    }

    if (t == NULL) return;

    if (prev == NULL) {
        active_list = target->next;
    } else {
        prev->next = target->next;
    }

    target->next   = NULL;
    target->active = 0;
}

/* ==========================================================================
   timer_delete — 删除定时器，归还池
   ========================================================================== */
/**
 * 如果还在 active_list 上就摘下来，然后插回 free_list。句柄从此失效。
 *
 * 例（TCP 连接关闭，释放重传定时器）:
 *   timer_delete(retrans);
 *   // retrans 被插回 free_list，可以再次被 timer_create 取走
 *   // retrans 句柄不再有效
 */
void timer_delete(timer_handle_t handle)
{
    struct timer *t = (struct timer *)handle;

    if (t == NULL) return;

    if (t->active) {
        timer_stop(t);
    }

    t->next = free_list;
    free_list = t;
}

/* ==========================================================================
   timer_is_active — 检查定时器是否在倒计时
   ========================================================================== */
/**
 * 例（ARP 发请求前，看是否已有请求在等待回复）:
 *   if (timer_is_active(arp_timer)) {
 *       // 已经在等待回复，不重复发请求
 *   } else {
 *       // 发 ARP 请求，启动定时器
 *       timer_start(arp_timer);
 *   }
 */
int timer_is_active(timer_handle_t handle)
{
    struct timer *t = (struct timer *)handle;
    if (t == NULL) return 0;
    return t->active;
}

/* ==========================================================================
   timer_set_interval — 修改定时周期（下次装载时生效）
   ========================================================================== */
/**
 * 只改 interval 字段，不影响当前这轮的 remaining。
 *
 * 例（TCP RTT 探测调整超时时间）:
 *   测得 RTT 变短，把重传定时器周期从 500ms 缩短到 200ms:
 *     timer_set_interval(retrans, 200);
 *   retrans->interval=200, retrans->remaining 仍是当前剩余值不变。
 *   下次 timer_start 或 PERIODIC 自动 reload 时用新值 200。
 */
void timer_set_interval(timer_handle_t handle, uint32_t interval)
{
    struct timer *t = (struct timer *)handle;
    if (t != NULL) {
        t->interval = interval;
    }
}

/* ==========================================================================
   timer_get_remaining — 查询距离到期还有多少毫秒
   ========================================================================== */
/**
 * 例（TCP 快速重传判断）:
 *   某个段一直没确认，查一下距离超时还有多久:
 *     uint32_t left = timer_get_remaining(retrans);
 *   left=50  → 还剩 50ms，快超时了
 *   left=300 → 还早，再等等
 *   left=0   → 已停止或已到期
 */
uint32_t timer_get_remaining(timer_handle_t handle)
{
    struct timer *t = (struct timer *)handle;
    if (t == NULL) return 0;
    return t->remaining;
}

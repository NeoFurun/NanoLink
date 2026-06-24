/**
 * @file    event_bus.c
 * @brief   事件总线模块实现
 *
 * 发布-订阅模式，模块间松耦合通信。
 * 例: 网卡 up 时发布 EVENT_NETIF_UP，应用层订阅后自动收到通知。
 */

#include "../include/event_bus.h"
#include <string.h>

struct listener {
    event_type_t type;
    event_handler_fn handler;
};

static struct listener listeners[EVENT_BUS_MAX_LISTENERS];

/* ==========================================================================
   event_bus_init — 初始化事件总线
   ========================================================================== */
void event_bus_init(void)
{
    memset(listeners, 0, sizeof(listeners));
}

/* ==========================================================================
   event_subscribe — 订阅事件
   ========================================================================== */
/**
 * 例（应用层监听网卡状态变化）:
 *   void on_netif_change(const struct event *e) {
 *       if (e->type == EVENT_NETIF_UP) printf("网卡起来了\n");
 *   }
 *   event_subscribe(EVENT_NETIF_UP, on_netif_change);
 */

int event_subscribe(event_type_t type, event_handler_fn handler)
{
    int i;

    if (handler == NULL) return -1;

    for (i = 0; i < EVENT_BUS_MAX_LISTENERS; i++) {
        if (listeners[i].handler == NULL) {
            listeners[i].type    = type;
            listeners[i].handler = handler;
            return 0;
        }
    }

    return -1; /* 表满 */
}

/* ==========================================================================
   event_unsubscribe — 取消订阅
   ========================================================================== */
void event_unsubscribe(event_type_t type, event_handler_fn handler)
{
    int i;

    for (i = 0; i < EVENT_BUS_MAX_LISTENERS; i++) {
        if (listeners[i].type == type && listeners[i].handler == handler) {
            listeners[i].type    = EVENT_NONE;
            listeners[i].handler = NULL;
            return;
        }
    }
}

/* ==========================================================================
   event_publish — 发布事件
   ========================================================================== */
/**
 * 同步遍历所有订阅者，匹配类型的就调回调。
 *
 * 例（网卡驱动调完 netif_set_up 后发布事件）:
 *   struct event e = {EVENT_NETIF_UP, &tap0, NULL, 0, 0};
 *   event_publish(&e);
 *   // 所有订阅 EVENT_NETIF_UP 的回调依次执行
 */
void event_publish(const struct event *e)
{
    int i;

    if (e == NULL) return;

    for (i = 0; i < EVENT_BUS_MAX_LISTENERS; i++) {
        if (listeners[i].handler != NULL && listeners[i].type == e->type) {
            listeners[i].handler(e);
        }
    }
}

/* ==========================================================================
   event_publish_simple — 便捷发布
   ========================================================================== */
void event_publish_simple(event_type_t type, void *source, void *data,
                          uint32_t param1, uint32_t param2)
{
    struct event e;
    e.type   = type;
    e.source = source;
    e.data   = data;
    e.param1 = param1;
    e.param2 = param2;
    event_publish(&e);
}

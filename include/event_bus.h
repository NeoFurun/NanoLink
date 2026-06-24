/**
 * @file    event_bus.h
 * @brief   事件总线模块公开接口（可选）
 *
 * 提供发布-订阅模式的异步事件通知，
 * 降低模块间耦合，便于扩展新模块。
 */

#ifndef EVENT_BUS_H
#define EVENT_BUS_H

#include <stdint.h>

/* -------------------------------------------------------------------------- */
/*  预定义常量                                                                */
/* -------------------------------------------------------------------------- */

#define EVENT_BUS_MAX_LISTENERS 16 /**< 最大监听者数量 */

/* -------------------------------------------------------------------------- */
/*  事件类型定义                                                              */
/* -------------------------------------------------------------------------- */

/** 事件类型枚举（可按需扩展） */
typedef enum
{
    EVENT_NONE = 0, /**< 无事件 */

    /* 网络接口事件 */
    EVENT_NETIF_UP,           /**< 接口启用 */
    EVENT_NETIF_DOWN,         /**< 接口停用 */
    EVENT_NETIF_ADDR_CHANGED, /**< 接口地址变更 */

    /* IP 层事件 */
    EVENT_IP_ADDR_ACQUIRED, /**< 获取到 IP 地址（如 DHCP 完成） */
    EVENT_IP_ADDR_LOST,     /**< IP 地址失效 */

    /* TCP 事件 */
    EVENT_TCP_CONNECTED, /**< TCP 连接建立 */
    EVENT_TCP_CLOSED,    /**< TCP 连接关闭 */
    EVENT_TCP_ERROR,     /**< TCP 错误 */

    /* 应用层事件 */
    EVENT_APP_CUSTOM_1 = 100, /**< 用户自定义事件起始 */
    EVENT_APP_CUSTOM_2,
    EVENT_APP_CUSTOM_3,

    EVENT_MAX
} event_type_t;

/* -------------------------------------------------------------------------- */
/*  类型定义                                                                  */
/* -------------------------------------------------------------------------- */

/** 事件数据结构 */
struct event
{
    event_type_t type; /**< 事件类型 */
    void *source;      /**< 事件来源（模块指针） */
    void *data;        /**< 附加数据（类型相关） */
    uint32_t param1;   /**< 附加参数1 */
    uint32_t param2;   /**< 附加参数2 */
};

/** 事件回调函数类型 */
typedef void (*event_handler_fn)(const struct event *e);

/* -------------------------------------------------------------------------- */
/*  公开函数接口                                                              */
/* -------------------------------------------------------------------------- */

/**
 * @brief 初始化事件总线模块。
 */
void event_bus_init(void);

/**
 * @brief 订阅指定类型的事件。
 * @param type     事件类型。
 * @param handler  事件处理回调函数。
 * @return 0 成功，-1 订阅表满。
 */
int event_subscribe(event_type_t type, event_handler_fn handler);

/**
 * @brief 取消订阅。
 * @param type     事件类型。
 * @param handler  之前注册的回调函数。
 */
void event_unsubscribe(event_type_t type, event_handler_fn handler);

/**
 * @brief 发布一个事件（同步调用所有订阅者的回调）。
 * @param e 事件结构体指针。
 */
void event_publish(const struct event *e);

/**
 * @brief 便捷函数：发布一个简单事件（无需手动构造 event 结构体）。
 * @param type    事件类型。
 * @param source  事件来源。
 * @param data    附加数据（可为 NULL）。
 * @param param1  附加参数1。
 * @param param2  附加参数2。
 */
void event_publish_simple(event_type_t type, void *source, void *data,
                          uint32_t param1, uint32_t param2);

#endif /* EVENT_BUS_H */
/**
 * @file    netif.h
 * @brief   网络接口抽象层公开接口
 *
 * 向上层提供统一的收发接口，管理所有网络接口，
 * 通过回调函数注入接收数据，不依赖具体协议。
 */

#ifndef NETIF_H
#define NETIF_H

#include <stdint.h>
#include "mbuf.h"

/* -------------------------------------------------------------------------- */
/*  预定义常量                                                                */
/* -------------------------------------------------------------------------- */

#define NETIF_NAME_LEN 8   /**< 接口名称最大长度 */
#define NETIF_HWADDR_LEN 6 /**< 硬件地址长度（以太网为6） */

/* 接口状态标志 */
#define NETIF_FLAG_UP 0x01        /**< 接口已启用 */
#define NETIF_FLAG_BROADCAST 0x02 /**< 支持广播 */
#define NETIF_FLAG_LOOPBACK 0x04  /**< 回环接口 */

/* -------------------------------------------------------------------------- */
/*  类型定义                                                                  */
/* -------------------------------------------------------------------------- */

struct netif;
struct driver_ops;

/** 输入回调函数类型：接收包时调用，返回 0 表示成功消费，非 0 表示未处理 */
typedef int (*netif_input_fn)(struct netif *ni, struct mbuf *m);

/** 发送回调函数类型：驱动层实现的发送函数 */
typedef int (*netif_send_fn)(struct netif *ni, struct mbuf *m);

/** 网络接口结构体 */
struct netif
{
    char name[NETIF_NAME_LEN];        /**< 接口名称 */
    uint8_t hwaddr[NETIF_HWADDR_LEN]; /**< 硬件地址 */
    uint16_t hwaddr_len;              /**< 硬件地址长度 */
    uint16_t mtu;                     /**< 最大传输单元 */
    uint16_t flags;                   /**< 状态标志位 */
    uint32_t ip_addr;                 /**< 本接口 IPv4 地址（网络字节序） */
    uint32_t netmask;                 /**< 子网掩码（网络字节序） */
    const struct driver_ops *ops;     /**< 驱动操作接口 */
    void *priv;                       /**< 驱动私有数据指针 */

    netif_send_fn send;   /**< 驱动实现的发送函数 */
    netif_input_fn input; /**< 向上分发的输入回调 */

    struct netif *next; /**< 全局链表指针（内部使用） */
};

/* -------------------------------------------------------------------------- */
/*  公开函数接口                                                              */
/* -------------------------------------------------------------------------- */

/**
 * @brief 初始化一个网络接口结构体。
 * @param ni    接口指针。
 * @param name  接口名称（如 "eth0"）。
 * @param send  驱动实现的发送函数。
 * @param input 接收时的输入分发回调（通常由协议层注册）。
 */
void netif_init(struct netif *ni, const char *name,
                netif_send_fn send, netif_input_fn input);

/**
 * @brief 注册一个网络接口到全局列表，使其可被使用。
 * @param ni 接口指针。
 * @return 0 成功，-1 重复注册。
 */
int netif_register(struct netif *ni);

/**
 * @brief 注销网络接口。
 * @param ni 接口指针。
 */
void netif_unregister(struct netif *ni);

/**
 * @brief 根据 IPv4 地址查找匹配的 netif（用于路由）。
 * @param ip_addr 目标 IP 地址。
 * @return 找到的接口指针，未找到返回 NULL。
 */
struct netif *netif_find_by_ip(uint32_t ip_addr);

/**
 * @brief 获取默认网络接口（通常是第一个注册的）。
 * @return 默认接口指针，无接口返回 NULL。
 */
struct netif *netif_get_default(void);

/**
 * @brief 驱动层收到数据包后注入协议栈的入口。
 *        内部调用 ni->input 回调，将包向上分发。
 * @param ni 接收包的网络接口。
 * @param m  收到的数据包（mbuf）。
 * @return input 回调的返回值，0 表示被消费。
 */
int netif_input(struct netif *ni, struct mbuf *m);

/**
 * @brief 启用网络接口。
 * @param ni 接口指针。
 */
void netif_set_up(struct netif *ni);

/**
 * @brief 停用网络接口。
 * @param ni 接口指针。
 */
void netif_set_down(struct netif *ni);

#endif /* NETIF_H */
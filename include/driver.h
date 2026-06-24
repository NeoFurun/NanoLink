/**
 * @file    driver.h
 * @brief   驱动程序适配层公开接口
 *
 * 定义网卡驱动需要实现的操作接口，
 * 提供驱动的注册与收包注入入口。
 */

#ifndef DRIVER_H
#define DRIVER_H

#include <stdint.h>
#include "netif.h"
#include "mbuf.h"

/* -------------------------------------------------------------------------- */
/*  预定义常量                                                                */
/* -------------------------------------------------------------------------- */

#define DRIVER_MAX_COUNT 4 /**< 最多支持的网卡数量 */

/* 驱动类型 */
#define DRIVER_TYPE_ETHERNET 1 /**< 以太网 */
#define DRIVER_TYPE_LOOPBACK 2 /**< 回环 */
#define DRIVER_TYPE_TAP 3      /**< TAP 虚拟接口（调试用） */

/* 链路层头部长度 */
#define ETH_HEADER_LEN 14    /**< 以太网帧头长度（6+6+2） */
#define ETH_ADDR_LEN 6       /**< MAC 地址长度 */
#define ETH_MTU 1500         /**< 以太网标准 MTU */
#define ETH_MIN_FRAME_LEN 60 /**< 最小帧长（不含 FCS） */

/* -------------------------------------------------------------------------- */
/*  类型定义                                                                  */
/* -------------------------------------------------------------------------- */

/** 以太网帧头部结构体 */
struct eth_header
{
    uint8_t dst_mac[ETH_ADDR_LEN]; /**< 目标 MAC */
    uint8_t src_mac[ETH_ADDR_LEN]; /**< 源 MAC */
    uint16_t ether_type;           /**< 帧类型 */
} __attribute__((packed));

/** 驱动操作接口：每个网卡驱动需要实现这些函数 */
struct driver_ops
{
    /** 初始化网卡硬件 */
    int (*init)(struct netif *ni);

    /** 发送一个数据包 */
    int (*send)(struct netif *ni, struct mbuf *m);

    /** 关闭网卡 */
    void (*close)(struct netif *ni);

    /** 设置 MAC 地址 */
    void (*set_mac)(struct netif *ni, const uint8_t *mac);

    /** 获取链路状态 */
    int (*get_status)(struct netif *ni);
};

/* -------------------------------------------------------------------------- */
/*  公开函数接口                                                              */
/* -------------------------------------------------------------------------- */

/**
 * @brief 使用给定的驱动操作接口注册一个网络接口。
 * @param name  接口名称（如 "eth0"）。
 * @param ops   驱动操作接口指针。
 * @param priv  驱动私有数据指针（可选，填入 netif->priv）。
 * @return 注册成功的 netif 指针，失败返回 NULL。
 */
struct netif *driver_register(const char *name, const struct driver_ops *ops,
                              void *priv);

/**
 * @brief 注销一个网络接口。
 * @param ni 接口指针。
 */
void driver_unregister(struct netif *ni);

/**
 * @brief 获取指定名称的接口。
 * @param name 接口名称。
 * @return 接口指针，未找到返回 NULL。
 */
struct netif *driver_get_by_name(const char *name);

/**
 * @brief 驱动初始化总入口：初始化所有已注册的驱动。
 */
void driver_init_all(void);

/**
 * @brief 从 mbuf 中解析以太网帧头，返回 ether_type。
 * @param m         包含以太网帧的 mbuf（data 指向帧头起始）。
 * @param eth_hdr   输出解析后的以太网头结构体。
 * @return 0 成功，-1 数据太短。
 */
int eth_header_parse(struct mbuf *m, struct eth_header *eth_hdr);

/**
 * @brief 为 mbuf 添加以太网帧头，封装为完整帧。
 * @param m          数据包 mbuf（data 应指向 IP 等负载的起始）。
 * @param dst_mac    目标 MAC 地址。
 * @param src_mac    源 MAC 地址。
 * @param ether_type 帧类型（ETHERTYPE_IPV4 等）。
 * @return 0 成功，-1 prepend 空间不足。
 */
int eth_header_add(struct mbuf *m, const uint8_t *dst_mac,
                   const uint8_t *src_mac, uint16_t ether_type);

/**
 * @brief 比较两个 MAC 地址是否相等。
 * @return 1 相等，0 不相等。
 */
int eth_addr_cmp(const uint8_t *a, const uint8_t *b);

/**
 * @brief 将 MAC 地址字符串转为字节数组（"aa:bb:cc:dd:ee:ff" → 6字节）。
 * @param str  MAC 字符串。
 * @param addr 输出字节数组。
 * @return 0 成功，-1 格式错误。
 */
int eth_addr_from_str(const char *str, uint8_t *addr);

#endif /* DRIVER_H */
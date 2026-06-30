#ifndef DRIVER_H
#define DRIVER_H

#include <stdint.h>
#include "netif.h"
#include "mbuf.h"

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
    /// 初始化网卡，返回 0 成功，-1 失败
    int (*init)(struct netif *ni);

    /// 发送数据包，返回 0 成功，-1 失败
    int (*send)(struct netif *ni, struct mbuf *m);

    /// 关闭网卡，释放资源
    void (*close)(struct netif *ni);

    /// 获取网卡 MAC 地址，返回 0 成功，-1 失败
    void (*set_mac)(struct netif *ni, const uint8_t *mac);

    /// 获取网卡状态，返回 0 up，1 down，-1 错误
    int (*get_status)(struct netif *ni);
};


//注册一张网卡
struct netif *driver_register(const char *name, const struct driver_ops *ops,
                              void *priv);

//程序退出时注销网卡
void driver_unregister(struct netif *ni);

//从 netif_list 里查找 name 匹配的 netif
struct netif *driver_get_by_name(const char *name);

//初始化所有已注册网卡
void driver_init_all(void);

//收包时解析以太网头
int eth_header_parse(struct mbuf *m, struct eth_header *eth_hdr);

//发包时添加以太网头
int eth_header_add(struct mbuf *m, const uint8_t *dst_mac,
                   const uint8_t *src_mac, uint16_t ether_type);

//比较两个 MAC 是否相等
int eth_addr_cmp(const uint8_t *a, const uint8_t *b);

//把 "00:11:22:33:44:55" 转成 6 字节 MAC
int eth_addr_from_str(const char *str, uint8_t *addr);

#endif
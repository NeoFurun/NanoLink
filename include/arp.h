#ifndef ARP_H
#define ARP_H

#include <stdint.h>
#include "mbuf.h"
#include "netif.h"


#define ARP_HW_TYPE_ETHERNET 1     /**< 以太网硬件类型 */
#define ARP_PROTO_TYPE_IPV4 0x0800 /**< IPv4 协议类型 */

#define ARP_TABLE_SIZE 32 /**< 默认 ARP 表大小，可配置 */
#define ARP_MAX_PENDING 4 /**< 每个表项最多挂起的待发送包 */

/* ARP 表项状态 */
#define ARP_STATE_FREE 0     /**< 未使用 */
#define ARP_STATE_PENDING 1  /**< 已发送请求，等待应答 */
#define ARP_STATE_RESOLVED 2 /**< 已解析，有有效 MAC */
#define ARP_STATE_STATIC 3   /**< 静态配置，永不过期 */

/* -------------------------------------------------------------------------- */
/*  类型定义                                                                  */
/* -------------------------------------------------------------------------- */

/** ARP 表项 */
struct arp_entry
{
    uint32_t ip_addr;                   /**< IPv4 地址（网络字节序） */
    uint8_t mac_addr[NETIF_HWADDR_LEN]; /**< 硬件地址 */
    uint8_t state;                      /**< 表项状态 */
    uint8_t retry_count;                /**< 当前重试次数 */
    struct netif *netif;                /**< 关联的网络接口 */

    /* 挂起队列：解析完成前暂存要发给该 IP 的包 */
    struct mbuf *pending_queue;
    uint8_t pending_count;
};

/* -------------------------------------------------------------------------- */
/*  公开函数接口                                                              */
/* -------------------------------------------------------------------------- */

/**
 * @brief 初始化 ARP 模块（清空 ARP 表等）。
 */
void arp_init(void);

/**
 * @brief ARP 输入处理函数，注册到 ll_dispatch（ETHERTYPE_ARP）。
 * @param ni 收到包的接口。
 * @param m  包含 ARP 消息的 mbuf（不包含以太网头，dispatch 已跳过）。
 * @return 0 成功，非 0 错误。
 */
int arp_input(struct netif *ni, struct mbuf *m);

/**
 * @brief 查询 IPv4 地址对应的 MAC 地址（供 IP 层发送时调用）。
 *        如果已解析则直接返回 MAC，并返回 0。
 *        如果未命中则触发 ARP 请求，并将当前 mbuf 挂起，返回 -1。
 *        调用者在返回 -1 后应直接放弃当前发送，待 ARP 解析完成会重新注入包。
 * @param ni       发送包的网络接口。
 * @param ip_addr  目标 IPv4 地址（网络字节序）。
 * @param mac_out  输出 MAC 地址缓冲区（6字节）。
 * @param m        待发送的 mbuf，解析期间挂起（解析成功后会重新注入）。
 * @return 0 成功解析，-1 未命中已挂起包。
 */
int arp_query(struct netif *ni, uint32_t ip_addr, uint8_t *mac_out,
              struct mbuf *m);

/**
 * @brief 添加/更新一个静态 ARP 表项。
 * @param ni       关联接口。
 * @param ip_addr  IPv4 地址。
 * @param mac_addr MAC 地址指针。
 * @return 0 成功，-1 表满。
 */
int arp_add_static(struct netif *ni, uint32_t ip_addr, const uint8_t *mac_addr);

/**
 * @brief 删除与 IP 地址对应的 ARP 表项。
 * @param ip_addr IPv4 地址。
 */
void arp_remove(uint32_t ip_addr);

/**
 * @brief 从接收到的包中学习 IP→MAC 映射（非静态，可老化）。
 * @param ni       收到包的接口。
 * @param ip_addr  源 IPv4 地址。
 * @param mac_addr  源 MAC 地址。
 */
void arp_learn(struct netif *ni, uint32_t ip_addr, const uint8_t *mac_addr);

/**
 * @brief 按接口删除所有关联的 ARP 表项（网卡注销时调用）。
 * @param ni 网络接口指针。
 */
void arp_remove_by_netif(struct netif *ni);

/**
 * @brief 定期维护 ARP 表（超时重传、老化等），由定时器或主循环调用。
 */
void arp_tick(void);

#endif /* ARP_H */
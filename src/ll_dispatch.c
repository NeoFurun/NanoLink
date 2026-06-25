/**
 * @file    ll_dispatch.c
 * @brief   链路层协议分发器实现
 *
 * 根据以太网帧类型（EtherType）将收到的数据包分发给注册的协议处理函数。
 * 这是 netif 和上层协议之间的胶水层——netif 只管收包，不关心上层是谁。
 *
 * 工作流程:
 *   网卡收到帧 → netif_input → ni->input (= ll_dispatch_input)
 *     → 读帧头 ether_type → 查注册表 → mbuf_pull 剥 MAC 头 → 调对应 handler
 *       ├→ 0x0800 → ip_input
 *       └→ 0x0806 → arp_input
 *
 * 全局就一张 dispatch_table[16]，存 ether_type → handler 的映射。
 */

#include "../include/ll_dispatch.h"
#include "../include/driver.h"
#include "../include/arp.h"
#include <stddef.h>
#include <string.h>

/* 注册表条目 */
struct ll_entry {
    uint16_t ether_type;
    ll_handler_fn handler;
};

static struct ll_entry dispatch_table[LL_DISPATCH_MAX_PROTO];

/* ==========================================================================
   ll_dispatch_register — 注册协议处理器
   ========================================================================== */
/**
 * 把 ether_type → handler 写入注册表。先检查重复，再找空位。
 *
 * 例（协议栈启动时注册）:
 *   ll_dispatch_register(ETHERTYPE_IPV4, ip_input);
 *   ll_dispatch_register(ETHERTYPE_ARP,  arp_input);
 *
 *   注册表:
 *     [0] 0x0800 → ip_input
 *     [1] 0x0806 → arp_input
 *     [2..15] 空
 *
 *   重复注册同类型 → 返回 -1。表满（16个）→ 返回 -1。
 */
int ll_dispatch_register(uint16_t ether_type, ll_handler_fn handler)
{
    int i;

    if (handler == NULL) return -1;

    for (i = 0; i < LL_DISPATCH_MAX_PROTO; i++) {
        if (dispatch_table[i].ether_type == ether_type) {
            return -1; /* 类型已被占用 */
        }
    }

    for (i = 0; i < LL_DISPATCH_MAX_PROTO; i++) {
        if (dispatch_table[i].handler == NULL) {
            dispatch_table[i].ether_type = ether_type;
            dispatch_table[i].handler    = handler;
            return 0;
        }
    }

    return -1; /* 注册表满 */
}

/* ==========================================================================
   ll_dispatch_unregister — 注销协议处理器
   ========================================================================== */
/**
 * 清空指定帧类型的注册项。
 *
 * 例（卸载 IPv6 支持）:
 *   ll_dispatch_unregister(ETHERTYPE_IPV6);
 *   // 注册表该项清空，之后收到 IPv6 帧直接丢弃
 */
void ll_dispatch_unregister(uint16_t ether_type)
{
    int i;

    for (i = 0; i < LL_DISPATCH_MAX_PROTO; i++) {
        if (dispatch_table[i].ether_type == ether_type) {
            dispatch_table[i].ether_type = 0;
            dispatch_table[i].handler    = NULL;
            return;
        }
    }
}

/* ==========================================================================
   ll_dispatch_input — 收包分发入口（传给 netif->input 的回调）
   ========================================================================== */
/**
 * 网卡收到帧后最终调到这里。读帧类型 → 查表 → 剥头 → 调 handler。
 *
 * 例（收到一个 ARP 请求帧）:
 *   mbuf m: data → [ FF:FF:FF:FF:FF:FF | AA:BB:CC:DD:EE:FF | 08 06 | ARP数据 ]
 *                   ↑ dst_mac             src_mac             type
 *
 *   ll_dispatch_input(&tap0, m):
 *     1. 读 m->data[12..13] → 0x0806 (ARP)
 *     2. 查注册表 → arp_input
 *     3. mbuf_pull(m, 14) → data 前移
 *
 *       m 变为: data → [ ARP数据... ]   ← 上层直接看到 ARP 包
 *
 *     4. 调 arp_input(&tap0, m)
 *
 *   如果 ether_type 没注册过 → mbuf_free(m)，丢弃。
 *   帧长不足 14 字节 → 垃圾帧，丢弃。
 */
int ll_dispatch_input(struct netif *ni, struct mbuf *m)
{
    uint16_t ether_type;
    int i;

    if (ni == NULL || m == NULL) return -1;
    if (m->len < ETH_HEADER_LEN) {
        mbuf_free(m);
        return -1;
    }

    /* 从帧头第12-13字节读出帧类型（大端序） */
    ether_type = (uint16_t)(m->data[12] << 8) | m->data[13];


    /* 查找注册表 */
    for (i = 0; i < LL_DISPATCH_MAX_PROTO; i++) {
        if (dispatch_table[i].handler != NULL &&
            dispatch_table[i].ether_type == ether_type) {

            /* 从以太网帧头学习源 MAC：IP 包的 IP 在偏移 26~29 */
            if (ether_type == ETHERTYPE_IPV4 && m->len >= 34) {
                uint32_t src_ip;
                memcpy(&src_ip, m->data + 26, 4);
                arp_learn(ni, src_ip, m->data + 6);
            }

            /* 剥掉以太网帧头，data 前移 14 字节 */
            if (mbuf_pull(m, ETH_HEADER_LEN) != 0) {
                mbuf_free(m);
                return -1;
            }

            return dispatch_table[i].handler(ni, m);
        }
    }

    /* 未注册的帧类型，丢弃 */

    mbuf_free(m);
    return -1;
}

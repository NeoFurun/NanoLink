/**
 * @file    arp.c
 * @brief   ARP 地址解析协议模块实现
 *
 * 把 IPv4 地址翻译成 MAC 地址。每个表项有四种状态：
 *   FREE     → 空位，可用
 *   PENDING  → 已发请求，等待回复（期间包挂起）
 *   RESOLVED → 已拿到 MAC，直接可用
 *   STATIC   → 手工配置，永不过期
 *
 * 核心流程:
 *   发送: IP 层调 arp_query
 *     → 有 MAC 就返回
 *     → 没 MAC 就发 ARP 请求，把包挂进 pending_queue
 *   接收: ll_dispatch 调 arp_input
 *     → ARP 请求 → 回复（顺便学对方的 IP→MAC）
 *     → ARP 回复 → 更新表项，flush pending_queue
 */

#include "../include/arp.h"
#include "../include/ll_dispatch.h"
#include "../include/driver.h"
#include <string.h>

/* ARP 缓存表 */
static struct arp_entry arp_table[ARP_TABLE_SIZE];

/* ARP 包结构（28 字节，硬件+协议组合的标准格式） */
struct arp_packet {
    uint16_t hw_type;       /* 硬件类型: 1=以太网 */
    uint16_t proto_type;    /* 协议类型: 0x0800=IPv4 */
    uint8_t  hw_addr_len;   /* MAC 长度: 6 */
    uint8_t  proto_addr_len;/* IP 长度: 4 */
    uint16_t opcode;        /* 1=请求(ARP_REQUEST), 2=回复(ARP_REPLY) */
    uint8_t  sender_mac[6];
    uint8_t  sender_ip[4];
    uint8_t  target_mac[6];
    uint8_t  target_ip[4];
} __attribute__((packed));

/* ARP opcode */
#define ARP_REQUEST 1
#define ARP_REPLY   2

/* 16位字节序翻转（htons 和 ntohs 操作相同） */
static inline uint16_t swap16(uint16_t n) {
    return (uint16_t)((n >> 8) | (n << 8));
}

/* ==========================================================================
   arp_init — 初始化 ARP 模块
   ========================================================================== */
/**
 * 清空 ARP 缓存表，并向链路层分发器注册 arp_input。
 *
 * 例（协议栈启动时调用）:
 *   arp_init();
 *   // arp_table[32] 全部 FREE
 *   // ll_dispatch: ether_type 0x0806 → arp_input (已有 ip_input 注册 0x0800)
 */
void arp_init(void)
{
    memset(arp_table, 0, sizeof(arp_table));

    /* 注册到链路层分发器：收到以太网帧类型 0x0806 时调 arp_input */
    ll_dispatch_register(ETHERTYPE_ARP, arp_input);
}

/* ==========================================================================
   arp_input — 处理收到的 ARP 包
   ========================================================================== */
/**
 * 两分支：收到请求 → 回复；收到回复 → 更新表项并 flush 挂起队列。
 * 直接在原 mbuf 上改动，零拷贝。
 *
 * 例（收到 ARP 请求——别人 ping 我们）:
 *   对方广播 "谁是 10.0.0.1？"
 *   arp = (struct arp_packet *)m->data:
 *     opcode=1(REQUEST), target_ip=10.0.0.1, sender_ip=10.0.0.2, sender_mac=AA:BB:...
 *
 *   arp_input 检查 target_ip == ni->ip_addr → 是问我的
 *     → 原 mbuf 上改: opcode=2(REPLY), sender↔target 交换
 *     → 加回以太网帧头，发回去
 *     → 顺手把 10.0.0.2→AA:BB:... 记进缓存
 *
 * 例（收到 ARP 回复——我们之前的询问被回答了）:
 *   对方回复 "10.0.0.2 的 MAC 是 AA:BB:CC:DD:EE:FF"
 *     → 找到 ip=10.0.0.2 的 PENDING 表项
 *     → 填入 MAC，改为 RESOLVED
 *     → 逐个取出 pending_queue 里的包，加 MAC 头，发出去
 */
int arp_input(struct netif *ni, struct mbuf *m)
{
    struct arp_packet *arp;
    struct arp_entry *entry;
    struct mbuf *pending;
    int i;

    if (ni == NULL || m == NULL) return -1;

    if (m->len < sizeof(struct arp_packet)) {
        mbuf_free(m);
        return -1;
    }

    arp = (struct arp_packet *)m->data;

    /* 只看以太网 + IPv4 的 ARP */
    if (arp->hw_type != swap16(ARP_HW_TYPE_ETHERNET) ||
        arp->proto_type != swap16(ARP_PROTO_TYPE_IPV4)) {

        mbuf_free(m);
        return -1;
    }

    if (arp->opcode == swap16(ARP_REQUEST)) {
        /* ================================================================
           ARP 请求：别人问"谁是 X.X.X.X？"
           ================================================================ */

        uint32_t target_ip, orig_sender_ip;
        uint8_t orig_sender_mac[6];

        memcpy(&target_ip, arp->target_ip, 4);

        if (target_ip != ni->ip_addr) {
            mbuf_free(m);
            return -1; /* 不是问我的 */
        }

        /* 保存原始发送者地址，随后就地改写为回复包 */
        memcpy(&orig_sender_ip, arp->sender_ip, 4);
        memcpy(orig_sender_mac,  arp->sender_mac, 6);

        /* 把请求包就地改为回复包 */
        arp->opcode = swap16(ARP_REPLY);

        /* target = 发问者（使用保存的原始 sender） */
        memcpy(arp->target_mac, orig_sender_mac, 6);
        memcpy(arp->target_ip,  &orig_sender_ip, 4);

        /* sender = 本机 */
        memcpy(arp->sender_mac, ni->hwaddr, 6);
        memcpy(arp->sender_ip,  &ni->ip_addr, 4);

        /* 加回以太网帧头，直接回复 */
        if (eth_header_add(m, arp->target_mac, ni->hwaddr, ETHERTYPE_ARP) != 0) {
            mbuf_free(m);
            return -1;
        }

        if (ni->send(ni, m) != 0) {
            mbuf_free(m);
        }

        /* 免费学到的：把发问者的 IP→MAC 记下来（使用保存的原始地址） */
        for (i = 0; i < ARP_TABLE_SIZE; i++) {
            if (arp_table[i].state == ARP_STATE_FREE) {
                arp_table[i].ip_addr = orig_sender_ip;
                memcpy(arp_table[i].mac_addr, orig_sender_mac, 6);
                arp_table[i].state = ARP_STATE_RESOLVED;
                arp_table[i].netif = ni;
                break;
            }
        }

        return 0;
    }

    if (arp->opcode == swap16(ARP_REPLY)) {
        /* ================================================================
           ARP 回复：有人回答我们的询问
           ================================================================ */

        uint32_t sender_ip;
        memcpy(&sender_ip, arp->sender_ip, 4);

        /* 找到对应的 PENDING 表项 */
        for (i = 0; i < ARP_TABLE_SIZE; i++) {
            if (arp_table[i].state == ARP_STATE_PENDING &&
                arp_table[i].ip_addr == sender_ip) {
                entry = &arp_table[i];
                break;
            }
        }

        if (i >= ARP_TABLE_SIZE) {
            mbuf_free(m);
            return -1; /* 没找到对应 PENDING 项（可能已超时清理） */
        }

        /* 更新为 RESOLVED */
        memcpy(entry->mac_addr, arp->sender_mac, 6);
        entry->state = ARP_STATE_RESOLVED;
        entry->retry_count = 0;

        /* flush 挂起队列：使用表项关联的网卡发送 */
        while (entry->pending_queue != NULL) {
            pending = entry->pending_queue;
            entry->pending_queue = pending->next_pkt;
            pending->next_pkt = NULL;

            if (entry->netif != NULL &&
                eth_header_add(pending, entry->mac_addr, entry->netif->hwaddr,
                               ETHERTYPE_IPV4) == 0) {
                if (entry->netif->send(entry->netif, pending) != 0) {
                    mbuf_free(pending);
                }
            } else {
                mbuf_free(pending);
            }
        }
        entry->pending_count = 0;

        mbuf_free(m);
        return 0;
    }

    /* 未知 opcode */
    mbuf_free(m);
    return -1;
}

/* ==========================================================================
   arp_add_static — 添加静态 ARP 表项
   ========================================================================== */
/**
 * 手工写入 IP→MAC 映射，不收发自学习。STATIC 永不过期。
 *
 * 例（写入网关 MAC，省掉每次查询）:
 *   uint8_t gw_mac[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
 *   arp_add_static(&eth0, inet_addr("10.0.0.1"), gw_mac);
 *   // arp_table 多一条 STATIC: 10.0.0.1 → 00:11:22:33:44:55
 *   之后发给 10.0.0.1 走 arp_query 直接命中，不广播 ARP 请求。
 */
int arp_add_static(struct netif *ni, uint32_t ip_addr, const uint8_t *mac_addr)
{
    int i;

    if (ni == NULL || mac_addr == NULL) return -1;

    /* 先检查是否已有同 IP 表项，有则原地更新 */
    for (i = 0; i < ARP_TABLE_SIZE; i++) {
        if (arp_table[i].state != ARP_STATE_FREE &&
            arp_table[i].ip_addr == ip_addr) {
            memcpy(arp_table[i].mac_addr, mac_addr, 6);
            arp_table[i].state = ARP_STATE_STATIC;
            arp_table[i].retry_count = 0;
            arp_table[i].netif = ni;
            /* 释放可能存在的挂起包 */
            while (arp_table[i].pending_queue != NULL) {
                struct mbuf *m = arp_table[i].pending_queue;
                arp_table[i].pending_queue = m->next_pkt;
                mbuf_free(m);
            }
            arp_table[i].pending_count = 0;
            return 0;
        }
    }

    /* 无重复，找空闲槽 */
    for (i = 0; i < ARP_TABLE_SIZE; i++) {
        if (arp_table[i].state == ARP_STATE_FREE) {
            arp_table[i].ip_addr = ip_addr;
            memcpy(arp_table[i].mac_addr, mac_addr, 6);
            arp_table[i].state = ARP_STATE_STATIC;
            arp_table[i].retry_count = 0;
            arp_table[i].netif = ni;
            arp_table[i].pending_queue = NULL;
            arp_table[i].pending_count = 0;
            return 0;
        }
    }

    return -1; /* 表满 */
}

/* ==========================================================================
   arp_remove — 删除 ARP 表项
   ========================================================================== */
/**
 * 按 IP 删除一条缓存，同时释放挂起队列里的所有包。
 *
 * 例（网卡 down 时清理相关表项）:
 *   arp_remove(inet_addr("10.0.0.2"));
 *   // 该 IP 的表项清零变 FREE，pending_queue 上挂的包全释放
 */
void arp_remove(uint32_t ip_addr)
{
    int i;

    for (i = 0; i < ARP_TABLE_SIZE; i++) {
        if (arp_table[i].ip_addr == ip_addr &&
            arp_table[i].state != ARP_STATE_FREE) {

            /* 释放所有挂起包 */
            while (arp_table[i].pending_queue != NULL) {
                struct mbuf *m = arp_table[i].pending_queue;
                arp_table[i].pending_queue = m->next_pkt;
                mbuf_free(m);
            }

            memset(&arp_table[i], 0, sizeof(struct arp_entry));
            return;
        }
    }
}

/* ==========================================================================
   arp_remove_by_netif — 按接口删除所有相关 ARP 表项
   ========================================================================== */
/**
 * 网卡注销时调用，清理与该接口关联的全部表项（含挂起包）。
 *
 * 例（driver_unregister 内部）:
 *   arp_remove_by_netif(&eth0);
 *   // eth0 上的所有 PENDING/RESOLVED/STATIC 表项全部清零
 */
void arp_remove_by_netif(struct netif *ni)
{
    int i;

    if (ni == NULL) return;

    for (i = 0; i < ARP_TABLE_SIZE; i++) {
        if (arp_table[i].netif == ni &&
            arp_table[i].state != ARP_STATE_FREE) {

            /* 释放所有挂起包 */
            while (arp_table[i].pending_queue != NULL) {
                struct mbuf *m = arp_table[i].pending_queue;
                arp_table[i].pending_queue = m->next_pkt;
                mbuf_free(m);
            }

            memset(&arp_table[i], 0, sizeof(struct arp_entry));
        }
    }
}

/* ==========================================================================
   arp_send_request — 广播一个 ARP 请求（内部辅助函数）
   ========================================================================== */
static int arp_send_request(struct netif *ni, uint32_t target_ip)
{
    struct arp_packet *arp;
    struct mbuf *m;
    uint8_t broadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    uint8_t zero_mac[6]   = {0};

    m = mbuf_alloc();
    if (m == NULL) return -1;

    if (mbuf_append(m, sizeof(struct arp_packet)) != 0) {
        mbuf_free(m);
        return -1;
    }
    arp = (struct arp_packet *)m->data;

    arp->hw_type    = swap16(ARP_HW_TYPE_ETHERNET);
    arp->proto_type = swap16(ARP_PROTO_TYPE_IPV4);
    arp->hw_addr_len    = 6;
    arp->proto_addr_len = 4;
    arp->opcode = swap16(ARP_REQUEST);

    memcpy(arp->sender_mac, ni->hwaddr, 6);
    memcpy(arp->sender_ip,  &ni->ip_addr, 4);
    memcpy(arp->target_mac, zero_mac, 6);      /* 未知，填 0 */
    memcpy(arp->target_ip,  &target_ip, 4);

    /* 加以太网帧头，目标 MAC 填广播 */
    if (eth_header_add(m, broadcast, ni->hwaddr, ETHERTYPE_ARP) != 0) {
        mbuf_free(m);
        return -1;
    }

    return ni->send(ni, m);
}

/* ==========================================================================
   arp_pending_enqueue — 把 mbuf 追加到挂起队列尾部（内部辅助函数）
   ========================================================================== */
static void arp_pending_enqueue(struct arp_entry *entry, struct mbuf *m)
{
    struct mbuf *q;

    if (entry->pending_queue == NULL) {
        entry->pending_queue = m;
    } else {
        q = entry->pending_queue;
        while (q->next_pkt != NULL) {
            q = q->next_pkt;
        }
        q->next_pkt = m;
    }
    m->next_pkt = NULL;
    entry->pending_count++;
}

/* ==========================================================================
   arp_tick — 定期维护 ARP 表
   ========================================================================== */
/**
 * 主循环每秒调一次。PENDING 表项重试 or 超时放弃。RESOLVED 暂不做老化。
 *
 * 例（arp_tick 被循环中的 1 秒定时器驱动）:
 *   第 1 秒: retry=1 → 重发 ARP 请求
 *   第 2 秒: retry=2 → 再重发
 *   第 3 秒: retry=3 → ≥3，超时！释放 pending_queue，表项变 FREE
 *
 *   如果中途收到回复，arp_input 会把 retry_count 清零，再也不会走到 ≥3 的分支。
 */
void arp_tick(void)
{
    int i;

    for (i = 0; i < ARP_TABLE_SIZE; i++) {
        if (arp_table[i].state == ARP_STATE_PENDING) {
            arp_table[i].retry_count++;

            if (arp_table[i].retry_count >= 3) {
                /* 超时放弃：释放挂起队列，回收表项 */
                while (arp_table[i].pending_queue != NULL) {
                    struct mbuf *m = arp_table[i].pending_queue;
                    arp_table[i].pending_queue = m->next_pkt;
                    mbuf_free(m);
                }
                memset(&arp_table[i], 0, sizeof(struct arp_entry));
            } else if (arp_table[i].netif != NULL &&
                       (arp_table[i].netif->flags & NETIF_FLAG_UP)) {
                /* 重发 ARP 请求（仅当接口仍有效） */
                arp_send_request(arp_table[i].netif, arp_table[i].ip_addr);
            } else {
                /* 接口已失效，释放挂起队列，回收表项 */
                while (arp_table[i].pending_queue != NULL) {
                    struct mbuf *m = arp_table[i].pending_queue;
                    arp_table[i].pending_queue = m->next_pkt;
                    mbuf_free(m);
                }
                memset(&arp_table[i], 0, sizeof(struct arp_entry));
            }
        }
    }
}

/* ==========================================================================
   arp_query — 查询 IP 对应的 MAC（供 IP 层发送时调用）
   ========================================================================== */
/**
 * IP 层要发包给某个 IP 时先调这个。三条路径：
 *   已解析 → 直接返回 MAC（返回 0）
 *   正在等 → 挂起当前包（返回 -1）
 *   没见过 → 新建 PENDING 表项 + 广播请求 + 挂包（返回 -1）
 *
 * 例（IP 层发 UDP 包给 10.0.0.100）:
 *   uint8_t dst_mac[6];
 *   int ret = arp_query(&eth0, dest_ip, dst_mac, mbuf_packet);
 *
 *   if (ret == 0) {
 *       // dst_mac 已填好，直接封装以太网帧头发送
 *       eth_header_add(mbuf_packet, dst_mac, eth0.hwaddr, ETHERTYPE_IPV4);
 *       eth0.send(&eth0, mbuf_packet);
 *   } else {
 *       // ret == -1: 包被 ARP 模块接管了，解析完成后自动发出
 *       // IP 层什么都不用做
 *   }
 */
int arp_query(struct netif *ni, uint32_t ip_addr, uint8_t *mac_out,
              struct mbuf *m)
{
    int i;
    int free_slot = -1;

    if (ni == NULL || mac_out == NULL) return -1;

    /* 查 ARP 表 */
    for (i = 0; i < ARP_TABLE_SIZE; i++) {
        if (arp_table[i].ip_addr == ip_addr) {
            if (arp_table[i].state == ARP_STATE_RESOLVED ||
                arp_table[i].state == ARP_STATE_STATIC) {
                /* 已解析，直接返回 MAC */

                memcpy(mac_out, arp_table[i].mac_addr, 6);
                return 0;
            }
            if (arp_table[i].state == ARP_STATE_PENDING) {
                /* 正在解析中，只挂包，不重复广播 */

                if (m != NULL && arp_table[i].pending_count < ARP_MAX_PENDING) {
                    arp_pending_enqueue(&arp_table[i], m);
                } else if (m != NULL) {
                    mbuf_free(m);
                }
                return -1;
            }
        }
        if (free_slot == -1 && arp_table[i].state == ARP_STATE_FREE) {
            free_slot = i;
        }
    }

    /* 未命中，新建 PENDING 表项并发起 ARP 请求 */
    if (free_slot == -1) {
        if (m != NULL) {
            mbuf_free(m);
        }
        return -1; /* 表满 */
    }

    arp_table[free_slot].ip_addr       = ip_addr;
    arp_table[free_slot].state         = ARP_STATE_PENDING;
    arp_table[free_slot].retry_count   = 0;
    arp_table[free_slot].netif         = ni;
    arp_table[free_slot].pending_queue = NULL;
    arp_table[free_slot].pending_count = 0;

    if (m != NULL) {
        arp_pending_enqueue(&arp_table[free_slot], m);
    }

    /* 广播 ARP 请求："谁是 ip_addr？" */

    arp_send_request(ni, ip_addr);

    return -1;
}

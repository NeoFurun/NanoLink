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

// 初始化 ARP 模块
void arp_init(void)
{
    memset(arp_table, 0, sizeof(arp_table));

    /* 注册到链路层分发器：收到以太网帧类型 0x0806 时调 arp_input */
    ll_dispatch_register(ETHERTYPE_ARP, arp_input);
}

// 处理收到的 ARP 包
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

// 添加静态 ARP 表项
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
// 学习到一个 IP→MAC 映射（用于收到 ARP 回复或请求时更新表项）
void arp_learn(struct netif *ni, uint32_t ip_addr, const uint8_t *mac_addr)
{
    int i;
    int free_slot = -1;

    if (ni == NULL || mac_addr == NULL) return;

    /* 先找是否已有同 IP 条目，有则更新 MAC */
    for (i = 0; i < ARP_TABLE_SIZE; i++) {
        if (arp_table[i].state != ARP_STATE_FREE &&
            arp_table[i].ip_addr == ip_addr) {
            memcpy(arp_table[i].mac_addr, mac_addr, 6);
            return;
        }
        if (free_slot == -1 && arp_table[i].state == ARP_STATE_FREE)
            free_slot = i;
    }

    if (free_slot == -1) return; /* 表满 */

    arp_table[free_slot].ip_addr       = ip_addr;
    arp_table[free_slot].state         = ARP_STATE_RESOLVED;
    arp_table[free_slot].retry_count   = 0;
    arp_table[free_slot].netif         = ni;
    arp_table[free_slot].pending_queue = NULL;
    arp_table[free_slot].pending_count = 0;
    memcpy(arp_table[free_slot].mac_addr, mac_addr, 6);
}
// 删除指定 IP 的 ARP 表项（释放挂起队列）
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
// 删除指定网络接口的所有 ARP 表项（释放挂起队列）
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

// ARP 定时器处理函数：检查 PENDING 表项，重发请求或超时清理
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

// 查询 IPv4 地址对应的 MAC 地址（供 IP 层发送时调用）
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

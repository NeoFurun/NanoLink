/**
 * @file    ip.c
 * @brief   IPv4 网络层模块实现
 *
 * 负责 IPv4 数据包的收发、校验、分片识别，向上层协议（ICMP/TCP/UDP）分发。
 *
 * 核心流程:
 *   发送: 传输层 → ip_output → 路由查找 → 加 IP 头 → ARP 查 MAC → eth_header_add → netif->send
 *   接收: ll_dispatch → ip_input → 校验 → 查协议表 → mbuf_pull(IP头) → 上层 handler
 */

#include "../include/ip.h"
#include "../include/ll_dispatch.h"
#include "../include/driver.h"
#include "../include/arp.h"
#include "../include/route.h"
#include "../include/osal.h"
#include <string.h>

/* 上层协议注册表（ICMP/TCP/UDP 在这里注册） */
static struct {
    uint8_t proto;
    ip_proto_handler_fn handler;
} ip_proto_table[IP_PROTO_MAX];

/* 分片重装表 */
static struct ip_frag_entry ip_frag_table[IP_FRAG_MAX_ENTRIES];

/* 全局 IP 标识符，每发一个包 +1 */
static uint16_t ip_id_counter = 0;

/* 16 位字节序翻转（htons 和 ntohs 操作相同） */
static inline uint16_t swap16(uint16_t n) {
    return (uint16_t)((n >> 8) | (n << 8));
}

/* ==========================================================================
   ip_checksum — IP 头部校验和（内部辅助函数）
   ========================================================================== */
/**
 * 对 IP 头每 16 位求和，进位回卷。mode=0 验证，mode=1 计算并填入。
 */
static uint16_t ip_checksum(struct ip_header *iph, int mode)
{
    uint32_t sum = 0;
    uint16_t *p = (uint16_t *)iph;
    int i;
    int len = (iph->ver_ihl & 0x0F) * 4;

    if (mode == 1) iph->checksum = 0; /* 计算时清零 */

    for (i = 0; i < len / 2; i++) {
        sum += p[i];
    }

    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }

    if (mode == 1) {
        iph->checksum = (uint16_t)(~sum);
        return iph->checksum;
    }

    /* mode == 0: 验证 — 包含校验和字段求和应为 0xFFFF */
    return (uint16_t)sum;
}

/* ==========================================================================
   ip_init — 初始化 IP 模块
   ========================================================================== */
/**
 * 清空协议表和分片表，向链路层注册 ip_input。
 *
 * 例（协议栈启动）:
 *   ip_init();
 *   // ip_proto_table[8] 全空
 *   // ip_frag_table[8] 全空
 *   // ll_dispatch: 0x0800 → ip_input
 */
void ip_init(void)
{
    memset(ip_proto_table, 0, sizeof(ip_proto_table));
    memset(ip_frag_table, 0, sizeof(ip_frag_table));

    ll_dispatch_register(ETHERTYPE_IPV4, ip_input);
}

/* ==========================================================================
   ip_register_proto — 注册上层协议处理器
   ========================================================================== */
/**
 * ICMP/TCP/UDP 启动时调用，把协议号和回调函数绑在一起。
 *
 * 例（ICMP 模块初始化时）:
 *   ip_register_proto(IP_PROTO_ICMP, icmp_input);  // 1 → icmp_input
 *   ip_register_proto(IP_PROTO_UDP,  udp_input);   // 17 → udp_input
 *
 *   注册表:
 *     [0] proto=1  → icmp_input
 *     [1] proto=17 → udp_input
 *
 *   重复注册同一协议号 → 返回 -1。表满（8个）→ 返回 -1。
 */
int ip_register_proto(uint8_t proto, ip_proto_handler_fn handler)
{
    int i;

    if (handler == NULL) return -1;

    for (i = 0; i < IP_PROTO_MAX; i++) {
        if (ip_proto_table[i].handler != NULL &&
            ip_proto_table[i].proto == proto) {
            return -1;
        }
    }

    for (i = 0; i < IP_PROTO_MAX; i++) {
        if (ip_proto_table[i].handler == NULL) {
            ip_proto_table[i].proto   = proto;
            ip_proto_table[i].handler = handler;
            return 0;
        }
    }

    return -1;
}

/* ==========================================================================
   ip_unregister_proto — 注销上层协议处理器
   ========================================================================== */
/**
 * 例（卸载 UDP 支持）:
 *   ip_unregister_proto(IP_PROTO_UDP);
 *   // 注册表中 proto=17 的条目清零，之后收到 UDP 包直接丢弃
 */
void ip_unregister_proto(uint8_t proto)
{
    int i;

    for (i = 0; i < IP_PROTO_MAX; i++) {
        if (ip_proto_table[i].proto == proto) {
            ip_proto_table[i].proto   = 0;
            ip_proto_table[i].handler = NULL;
            return;
        }
    }
}

/* ==========================================================================
   ip_set_address — 给网卡配置 IP 和子网掩码
   ========================================================================== */
/**
 * 例（main 启动时配 IP）:
 *   ip_set_address(&tap0, inet_addr("10.0.0.1"), inet_addr("255.255.255.0"));
 *   // tap0.ip_addr = 0x0A000001, tap0.netmask = 0xFFFFFF00
 */
void ip_set_address(struct netif *ni, uint32_t ip_addr, uint32_t netmask)
{
    if (ni == NULL) return;
    ni->ip_addr = ip_addr;
    ni->netmask = netmask;
}

/* ==========================================================================
   ip_get_address — 获取网卡 IP
   ========================================================================== */
uint32_t ip_get_address(struct netif *ni)
{
    if (ni == NULL) return 0;
    return ni->ip_addr;
}

/* ==========================================================================
   ip_is_local — 检查 IP 是否属于本机
   ========================================================================== */
/**
 * 遍历所有网卡，看有没有 ip_addr 匹配的。
 *
 * 例:
 *   tap0.ip_addr = 10.0.0.1,  lo.ip_addr = 127.0.0.1
 *   ip_is_local(0x0A000001) → 1   (10.0.0.1)
 *   ip_is_local(0x7F000001) → 1   (127.0.0.1)
 *   ip_is_local(0x08080808) → 0   (8.8.8.8，非本机)
 */
int ip_is_local(uint32_t ip_addr)
{
    struct netif *ni;

    ni = netif_get_default();
    while (ni != NULL) {
        if (ni->ip_addr == ip_addr) return 1;
        ni = ni->next;
    }
    return 0;
}

/* ==========================================================================
   ip_input — IP 包输入处理
   ========================================================================== */
/**
 * 校验 IP 头 → 查是否本机 → 剥头 → 分发给上层。
 *
 * 例（收到一个 UDP 包从 10.0.0.2 发给 10.0.0.1）:
 *   mbuf m: data → [ IP头(20B): ver=4, proto=17(UDP), src=10.0.0.2, dst=10.0.0.1 ]
 *                     [ UDP头 | 数据 ]
 *
 *   ip_input(&tap0, m):
 *     1. ver=4 ✓
 *     2. 校验和 ✓
 *     3. dst=10.0.0.1 → ip_is_local → 是本机 ✓
 *     4. 无分片 ✓
 *     5. proto=17 → 查表 → udp_input
 *     6. mbuf_pull(m, 20) → data 指向 UDP 头
 *     7. udp_input(&tap0, m, 10.0.0.2, 10.0.0.1)
 *
 *   非本机、校验错、无上层 handler → 丢弃 m。
 */
int ip_input(struct netif *ni, struct mbuf *m)
{
    struct ip_header *iph;
    uint16_t hdr_len;
    uint16_t calc_cs;
    int i;

    if (ni == NULL || m == NULL) return -1;
    if (m->len < IP_HEADER_MIN_LEN) {

        mbuf_free(m);
        return -1;
    }

    iph = (struct ip_header *)m->data;

    /* 只处理 IPv4 */
    if ((iph->ver_ihl >> 4) != IP_VERSION_4) {

        mbuf_free(m);
        return -1;
    }

    hdr_len = (iph->ver_ihl & 0x0F) * 4;
    if (hdr_len < IP_HEADER_MIN_LEN || hdr_len > IP_HEADER_MAX_LEN) {

        mbuf_free(m);
        return -1;
    }

    /* 校验头部 */
    calc_cs = ip_checksum(iph, 0);
    if (calc_cs != 0xFFFF) {

        mbuf_free(m);
        return -1;
    }

    /* 不是发给本机的，丢弃（暂不支持转发） */
    if (!ip_is_local(iph->dst_addr)) {

        mbuf_free(m);
        return -1;
    }

    /* 分片检查：字节级解析，避免字节序问题 */
    {
        uint8_t *raw = (uint8_t *)&iph->frag_offset;
        uint16_t flags  = (raw[0] >> 5) & 0x07;           /* 高 3 位 */
        uint16_t offset = ((raw[0] & 0x1F) << 8) | raw[1]; /* 低 13 位 */

        if ((flags & 0x01) || offset > 0) {
            /* 是分片，暂不支持重组，直接丢弃 */
            mbuf_free(m);
            return -1;
        }
    }

    /* 根据协议号分发给上层 */
    for (i = 0; i < IP_PROTO_MAX; i++) {
        if (ip_proto_table[i].handler != NULL &&
            ip_proto_table[i].proto == iph->proto) {

            mbuf_pull(m, hdr_len);
            return ip_proto_table[i].handler(ni, m, iph->src_addr,
                                             iph->dst_addr);
        }
    }

    /* 未注册的协议 */

    mbuf_free(m);
    return -1;
}

/* ==========================================================================
   ip_output — 发送 IPv4 数据包
   ========================================================================== */
/**
 * 传输层（UDP/TCP/ICMP）调这个函数发数据。内部完成路由 + IP头 + ARP。
 *
 * 例（UDP 层发一个包给 10.0.0.2）:
 *   mbuf m: data → [ UDP头 | 100字节数据 ]
 *
 *   ip_output(0x0A000002, IP_PROTO_UDP, m):
 *     1. netif_find_by_ip → tap0（匹配子网）
 *     2. mbuf_prepend(m, 20) → m 变为:
 *        [ IP头 | UDP头 | 数据 ]
 *     3. 填 IP 头: ver=4, proto=17, src=10.0.0.1, dst=10.0.0.2, ttl=64
 *     4. ip_checksum → 写校验和
 *     5. arp_query → RESOLVED → 拿到 MAC
 *     6. eth_header_add → [ MAC头 | IP头 | UDP头 | 数据 ]
 *     7. tap0.send → write(fd) → Wireshark 可见！
 *
 *   ARP 未解析 → arp_query 返回 -1，m 被 ARP 挂起，解析后自动发出。
 */
int ip_output(uint32_t dst_addr, uint8_t proto, struct mbuf *m)
{
    struct netif *ni;
    struct ip_header *iph;
    uint8_t dst_mac[6];
    uint16_t total_len;
    uint32_t next_hop;

    if (m == NULL) return -1;

    /* 路由查找 */
    if (route_lookup(dst_addr, &ni, &next_hop) != 0) {
        mbuf_free(m);
        return -1;
    }

    /* 添加 IP 头部 */
    if (mbuf_prepend(m, IP_HEADER_MIN_LEN) != 0) {
        mbuf_free(m);
        return -1;
    }

    total_len = m->total_len;

    iph = (struct ip_header *)m->data;
    memset(iph, 0, IP_HEADER_MIN_LEN);

    iph->ver_ihl     = (IP_VERSION_4 << 4) | (IP_HEADER_MIN_LEN / 4);
    iph->tos         = 0;
    iph->total_len   = swap16(total_len);
    iph->id          = swap16(ip_id_counter);
    iph->frag_offset = 0;
    iph->ttl         = IP_DEFAULT_TTL;
    iph->proto       = proto;
    iph->checksum    = 0;
    iph->src_addr    = ni->ip_addr;
    iph->dst_addr    = dst_addr;

    ip_id_counter++;

    /* 计算校验和 */
    ip_checksum(iph, 1);

    /* ARP 查 MAC → 已解析直接发，未解析 ARP 接管 m */

    if (arp_query(ni, next_hop, dst_mac, m) == 0) {

        if (eth_header_add(m, dst_mac, ni->hwaddr, ETHERTYPE_IPV4) != 0) {
            mbuf_free(m);
            return -1;
        }
        return ni->send(ni, m);
    }

    /* m 已被 ARP 挂起，调用者不用再管 */
    return -1;
}

/* ==========================================================================
   ip_frag_tick — 清理超时的分片重组条目
   ========================================================================== */
/**
 * 主循环每秒调用一次。分片 15 秒没到齐就丢弃。
 *
 * 例:
 *   主循环: 每 1 秒调 ip_frag_tick()
 *   timeout 从 15 倒数到 0 → 超时，释放已收集的片段
 */
void ip_frag_tick(void)
{
    int i;

    for (i = 0; i < IP_FRAG_MAX_ENTRIES; i++) {
        if (ip_frag_table[i].active) {
            if (ip_frag_table[i].timeout > 0) {
                ip_frag_table[i].timeout--;
            }
            if (ip_frag_table[i].timeout == 0) {
                struct mbuf *p = ip_frag_table[i].chain;
                while (p != NULL) {
                    struct mbuf *next = p->next_pkt;
                    mbuf_free(p);
                    p = next;
                }
                memset(&ip_frag_table[i], 0, sizeof(struct ip_frag_entry));
            }
        }
    }
}

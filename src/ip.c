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

// 计算 IP 头部校验和
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

void ip_init(void)
{
    memset(ip_proto_table, 0, sizeof(ip_proto_table));
    memset(ip_frag_table, 0, sizeof(ip_frag_table));

    ll_dispatch_register(ETHERTYPE_IPV4, ip_input);
}

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

void ip_set_address(struct netif *ni, uint32_t ip_addr, uint32_t netmask)
{
    if (ni == NULL) return;
    ni->ip_addr = ip_addr;
    ni->netmask = netmask;
}

uint32_t ip_get_address(struct netif *ni)
{
    if (ni == NULL) return 0;
    return ni->ip_addr;
}

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

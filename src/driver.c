/**
 * @file    driver.c
 * @brief   驱动程序适配层实现
 *
 * 管理网卡驱动注册、注销、查找，并提供以太网帧头的解析与封装工具。
 *
 * 架构：静态池 + driver_ops 回调表
 *   netif_pool[4] — 最多 4 张网卡的结构体存储
 *   driver_ops     — 每张网卡绑定的驱动操作（init/send/close/set_mac/get_status）
 *
 *   数据流:
 *   发送: 上层 → eth_header_add(加MAC头) → ni->ops->send → TAP设备 → 内核
 *   接收: 内核 → TAP设备 → ni->ops->send上层 → eth_header_parse(拆MAC头) → 链路层
 */

#include "../include/driver.h"
#include "../include/osal.h"
#include <string.h>

/* 网卡结构体静态池 */
static struct netif netif_pool[DRIVER_MAX_COUNT];
static int netif_pool_used = 0;

/* ==========================================================================
   driver_register — 绑定驱动并注册网卡
   ========================================================================== */
/**
 * 从静态池取一个 netif，填名字、绑定 ops、注册到全局链表。
 *
 * 例（TAP 驱动注册虚拟网卡）:
 *   struct driver_ops tap_ops = { tap_init, tap_send, tap_close, NULL, NULL };
 *   struct netif *ni = driver_register("tap0", &tap_ops, NULL);
 *   // ni->name = "tap0"
 *   // ni->ops  = &tap_ops
 *   // ni->send 通过 netif_init 设为 tap_send
 *   // ni 已挂在 netif_list 尾部
 *   // 返回 NULL 表示池满了（超过 4 张）或重复注册
 */
struct netif *driver_register(const char *name, const struct driver_ops *ops,
                              void *priv)
{
    struct netif *ni;

    if (name == NULL || ops == NULL) return NULL;
    if (netif_pool_used >= DRIVER_MAX_COUNT) return NULL;

    ni = &netif_pool[netif_pool_used++];

    netif_init(ni, name, ops->send, NULL);
    ni->ops  = ops;
    ni->priv = priv;

    if (netif_register(ni) != 0) {
        netif_pool_used--;
        return NULL;
    }

    return ni;
}

/* ==========================================================================
   driver_unregister — 注销网卡，关闭驱动
   ========================================================================== */
/**
 * 调 ops->close 关硬件，然后从全局链表摘除。
 *
 * 例（TAP 驱动卸载）:
 *   driver_unregister(tap0);
 *   // 内部:
 *   //   1. tap0->ops->close(tap0) → close(fd), TAP 设备关闭
 *   //   2. netif_unregister(tap0) → 从 netif_list 摘除
 *   // 之后协议栈再也看不到 tap0
 */
void driver_unregister(struct netif *ni)
{
    int i;

    if (ni == NULL) return;

    if (ni->ops != NULL && ni->ops->close != NULL) {
        ni->ops->close(ni);
    }

    netif_unregister(ni);

    /* 如果 ni 来自静态池，递减已用计数 */
    for (i = 0; i < netif_pool_used; i++) {
        if (&netif_pool[i] == ni) {
            netif_pool_used--;
            break;
        }
    }
}

/* ==========================================================================
   driver_get_by_name — 按名称查找网卡
   ========================================================================== */
/**
 * 遍历全局链表，strcmp 匹配 name 字段。
 *
 * 例:
 *   netif_list: [eth0] → [wlan0] → [tap0] → NULL
 *   driver_get_by_name("tap0")  → tap0
 *   driver_get_by_name("eth3")  → NULL
 */
struct netif *driver_get_by_name(const char *name)
{
    struct netif *p;

    if (name == NULL) return NULL;

    p = netif_get_default();
    while (p != NULL) {
        if (strcmp(p->name, name) == 0) {
            return p;
        }
        p = p->next;
    }

    return NULL;
}

/* ==========================================================================
   driver_init_all — 初始化所有已注册网卡
   ========================================================================== */
/**
 * 遍历链表，每张网卡调 ops->init。在协议栈启动时统一调用。
 *
 * 例（main 函数中）:
 *   driver_register("tap0", &tap_ops, NULL);
 *   driver_register("lo",   &loop_ops, NULL);
 *   driver_init_all();
 *   // tap0: ops->init(tap0) → open("/dev/net/tun"), 设 MAC, 设 MTU
 *   // lo:   ops->init(lo)   → 回环初始化
 *   // 之后两张网卡就绪，可以收发数据了
 */
void driver_init_all(void)
{
    struct netif *p, *next;

    p = netif_get_default();
    while (p != NULL) {
        next = p->next; /* 保存后继指针，init 失败时 unregister 会置 NULL */
        if (p->ops != NULL && p->ops->init != NULL) {
            if (p->ops->init(p) != 0) {
                netif_unregister(p);
            }
        }
        p = next;
    }
}

/* ==========================================================================
   eth_header_parse — 从 mbuf 中解析以太网帧头
   ========================================================================== */
/**
 * 从 data 起始位置读 14 字节，拆出目标 MAC、源 MAC、帧类型。
 * 收包路径使用。
 *
 * 例（网卡收到一个 ARP 帧）:
 *   mbuf m: data → [ FF:FF:FF:FF:FF:FF | AA:BB:CC:DD:EE:FF | 08 06 | ARP负载... ]
 *   struct eth_header hdr;
 *   eth_header_parse(m, &hdr);
 *   // hdr.dst_mac    = {FF, FF, FF, FF, FF, FF}  (广播)
 *   // hdr.src_mac    = {AA, BB, CC, DD, EE, FF}
 *   // hdr.ether_type = 0x0806  (ARP)
 *
 *   如果 m->len < 14，数据太短，返回 -1。
 */
int eth_header_parse(struct mbuf *m, struct eth_header *eth_hdr)
{
    if (m == NULL || eth_hdr == NULL) return -1;
    if (m->len < ETH_HEADER_LEN) return -1;

    memcpy(eth_hdr->dst_mac, m->data, ETH_ADDR_LEN);
    memcpy(eth_hdr->src_mac, m->data + ETH_ADDR_LEN, ETH_ADDR_LEN);
    eth_hdr->ether_type = (uint16_t)(m->data[12] << 8) | m->data[13];

    return 0;
}

/* ==========================================================================
   eth_header_add — 给 mbuf 前面加上以太网帧头
   ========================================================================== */
/**
 * mbuf_prepend 前移 data 指针 14 字节，填入 MAC 头和帧类型。
 * 发包路径使用。
 *
 * 例（IP 层准备好数据报后，链路层封装）:
 *   mbuf m: [ headroom 50 ][ IP头 | UDP头 | 数据 ]
 *                          ↑ data
 *   eth_header_add(m, server_mac, my_mac, 0x0800);
 *   mbuf m: [ headroom 36 ][ dst_mac(6) | src_mac(6) | 08 00 | IP头 | ... ]
 *                          ↑ data
 *
 *   如果 headroom 不够 14 字节，mbuf_prepend 失败，返回 -1。
 */
int eth_header_add(struct mbuf *m, const uint8_t *dst_mac,
                   const uint8_t *src_mac, uint16_t ether_type)
{
    if (m == NULL || dst_mac == NULL || src_mac == NULL) return -1;

    if (mbuf_prepend(m, ETH_HEADER_LEN) != 0) return -1;

    memcpy(m->data, dst_mac, ETH_ADDR_LEN);
    memcpy(m->data + ETH_ADDR_LEN, src_mac, ETH_ADDR_LEN);
    m->data[12] = (uint8_t)(ether_type >> 8);
    m->data[13] = (uint8_t)(ether_type & 0xFF);

    return 0;
}

/* ==========================================================================
   eth_addr_cmp — 比较两个 MAC 地址是否相等
   ========================================================================== */
/**
 * memcmp 6 字节，相等返回 1，不同返回 0。
 *
 * 例（收包时判断帧是不是发给本机）:
 *   uint8_t my_mac[6] = {00, 11, 22, 33, 44, 55};
 *   struct eth_header hdr;
 *   eth_header_parse(m, &hdr);
 *   if (eth_addr_cmp(hdr.dst_mac, my_mac)) {
 *       // 是发给我的，继续处理
 *   } else {
 *       // 不是发给我的（可能是混杂模式抓到的），丢弃
 *   }
 */
int eth_addr_cmp(const uint8_t *a, const uint8_t *b)
{
    if (a == NULL || b == NULL) return 0;
    return (memcmp(a, b, ETH_ADDR_LEN) == 0) ? 1 : 0;
}

/* ==========================================================================
   eth_addr_from_str — MAC 字符串转字节数组
   ========================================================================== */
/**
 * "AA:BB:CC:DD:EE:FF" → {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF}
 *
 * 例（配置文件或命令行指定 MAC 地址）:
 *   uint8_t mac[6];
 *   eth_addr_from_str("00:11:22:33:44:55", mac);
 *   // mac = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55}
 *
 *   格式不对（分隔符不是冒号、hex 超范围、字符串太短）返回 -1。
 */
static int hex_char_to_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int eth_addr_from_str(const char *str, uint8_t *addr)
{
    int i;

    if (str == NULL || addr == NULL) return -1;

    /* "AA:BB:CC:DD:EE:FF" 至少需要 17 个字符 */
    if (strlen(str) < 17) return -1;

    for (i = 0; i < ETH_ADDR_LEN; i++) {
        int hi = hex_char_to_nibble(str[i * 3]);
        int lo = hex_char_to_nibble(str[i * 3 + 1]);
        if (hi < 0 || lo < 0) return -1;

        /* 最后一字节后面不用冒号 */
        if (i < ETH_ADDR_LEN - 1 && str[i * 3 + 2] != ':') return -1;

        addr[i] = (uint8_t)((hi << 4) | lo);
    }

    return 0;
}

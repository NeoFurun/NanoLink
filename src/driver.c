#include "../include/driver.h"
#include "../include/osal.h"
#include <string.h>

static struct netif netif_pool[DRIVER_MAX_COUNT];
static int netif_pool_used = 0;

//注册一张网卡
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

// 注销一张网卡
void driver_unregister(struct netif *ni)
{
    int i;

    if (ni == NULL) return;

    if (ni->ops != NULL && ni->ops->close != NULL) {
        ni->ops->close(ni);
    }

    netif_unregister(ni);

    for (i = 0; i < netif_pool_used; i++) {
        if (&netif_pool[i] == ni) {
            netif_pool_used--;
            break;
        }
    }
}

// 根据网卡名查找网卡
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

// 初始化所有注册的网卡
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

//从mbuf中解析以太网帧头
int eth_header_parse(struct mbuf *m, struct eth_header *eth_hdr)
{
    if (m == NULL || eth_hdr == NULL) return -1;
    if (m->len < ETH_HEADER_LEN) return -1;

    memcpy(eth_hdr->dst_mac, m->data, ETH_ADDR_LEN);
    memcpy(eth_hdr->src_mac, m->data + ETH_ADDR_LEN, ETH_ADDR_LEN);
    eth_hdr->ether_type = (uint16_t)(m->data[12] << 8) | m->data[13];

    return 0;
}

//在mbuf前头添加以太网帧头
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

//比较两个 MAC 地址是否相等
int eth_addr_cmp(const uint8_t *a, const uint8_t *b)
{
    if (a == NULL || b == NULL) return 0;
    return (memcmp(a, b, ETH_ADDR_LEN) == 0) ? 1 : 0;
}

static int hex_char_to_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
// 将 MAC 地址字符串转换为字节数组
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

#include "../include/netif.h"
#include <string.h>

// 全局链表头，指向第一张注册的网卡
static struct netif *netif_list = NULL;


void netif_init(struct netif *ni, const char *name,
                netif_send_fn send, netif_input_fn input)
{
    if (ni == NULL) return;

    memset(ni, 0, sizeof(struct netif));

    strncpy(ni->name, name, NETIF_NAME_LEN - 1);
    ni->name[NETIF_NAME_LEN - 1] = '\0';

    ni->send  = send;
    ni->input = input;
}

int netif_register(struct netif *ni)
{
    struct netif *p;

    if (ni == NULL) return -1;

    /* 检查是否重复注册 */
    p = netif_list;
    while (p != NULL) {
        if (p == ni) return -1;
        p = p->next;
    }

    /* 插入链表尾部 */
    if (netif_list == NULL) {
        netif_list = ni;
    } else {
        p = netif_list;
        while (p->next != NULL) {
            p = p->next;
        }
        p->next = ni;
    }

    ni->next = NULL;

    return 0;
}

void netif_unregister(struct netif *ni)
{
    struct netif *p, *prev;

    if (ni == NULL) return;

    prev = NULL;
    p = netif_list;
    while (p != NULL && p != ni) {
        prev = p;
        p = p->next;
    }

    if (p == NULL) return;

    if (prev == NULL) {
        netif_list = ni->next;
    } else {
        prev->next = ni->next;
    }

    ni->next = NULL;
}

struct netif *netif_find_by_ip(uint32_t ip_addr)
{
    struct netif *p;

    p = netif_list;
    while (p != NULL) {
        /* 跳过未配置子网掩码的接口（netmask==0 会匹配所有 IP） */
        if (p->netmask == 0) {
            p = p->next;
            continue;
        }

        /* 跳过未启用的接口 */
        if (!(p->flags & NETIF_FLAG_UP)) {
            p = p->next;
            continue;
        }

        /* 同一子网: (目标IP & 掩码) == (网卡IP & 掩码) */
        if ((ip_addr & p->netmask) == (p->ip_addr & p->netmask)) {
            return p;
        }
        p = p->next;
    }

    return NULL;
}

struct netif *netif_get_default(void)
{
    return netif_list;
}

int netif_input(struct netif *ni, struct mbuf *m)
{
    if (ni == NULL || m == NULL) return -1;

    if (ni->input == NULL) {
        mbuf_free(m);
        return -1;
    }

    return ni->input(ni, m);
}

void netif_set_up(struct netif *ni)
{
    if (ni == NULL) return;
    ni->flags |= NETIF_FLAG_UP;
}

void netif_set_down(struct netif *ni)
{
    if (ni == NULL) return;
    ni->flags &= ~NETIF_FLAG_UP;
}

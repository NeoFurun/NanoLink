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

    return -1;
}

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

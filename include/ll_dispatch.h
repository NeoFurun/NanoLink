#ifndef LL_DISPATCH_H
#define LL_DISPATCH_H

#include <stdint.h>
#include "mbuf.h"
#include "netif.h"

#define LL_DISPATCH_MAX_PROTO 16 /**< 最大注册协议数量 */

/* 常见以太网类型（可扩展） */
#define ETHERTYPE_IPV4 0x0800
#define ETHERTYPE_ARP 0x0806
#define ETHERTYPE_IPV6 0x86DD

/** 链路层协议处理函数指针类型 */
typedef int (*ll_handler_fn)(struct netif *ni, struct mbuf *m);

//往 dispatch_table 里添加一条ether_type -> handler
int ll_dispatch_register(uint16_t ether_type, ll_handler_fn handler);

//从 dispatch_table 里删除一条ether_type -> handler
void ll_dispatch_unregister(uint16_t ether_type);

//读取以太网帧的 EtherType，查 dispatch_table，剥掉以太网头，调用对应协议处理函数。
int ll_dispatch_input(struct netif *ni, struct mbuf *m);

#endif
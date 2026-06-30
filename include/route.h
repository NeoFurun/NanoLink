#ifndef ROUTE_H
#define ROUTE_H

#include <stdint.h>
#include "netif.h"

#define ROUTE_MAX_ENTRIES 32    /**< 最大路由表条目数 */
#define ROUTE_FLAG_UP 0x01      /**< 路由有效 */
#define ROUTE_FLAG_GATEWAY 0x02 /**< 路由指向网关（非直连） */
#define ROUTE_FLAG_HOST 0x04    /**< 主机路由（掩码为全1） */
#define ROUTE_FLAG_DEFAULT 0x08 /**< 默认路由（0.0.0.0/0） */

/** 路由表条目 */
struct route_entry
{
    uint32_t dest;       /**< 目标网络地址（网络字节序） */
    uint32_t netmask;    /**< 子网掩码（网络字节序） */
    uint32_t gateway;    /**< 下一跳地址（网络字节序，直连则为0） */
    uint8_t flags;       /**< 标志位 */
    uint8_t metric;      /**< 度量值（越小越优先，预留） */
    struct netif *netif; /**< 出口网络接口 */
};

//初始化路由表
void route_init(void);

//添加一条路由条目
int route_add(uint32_t dest, uint32_t netmask, uint32_t gateway,struct netif *ni);

//删除一条路由条目
void route_remove(uint32_t dest, uint32_t netmask);

//查找路由条目，返回出口网卡和下一跳地址
int route_lookup(uint32_t dst_addr, struct netif **ni_out, uint32_t *next_hop);

//设置默认路由
int route_set_default(uint32_t gateway, struct netif *ni);

//打印路由表
void route_dump(void);

#endif

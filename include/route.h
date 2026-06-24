/**
 * @file    route.h
 * @brief   路由表模块公开接口
 *
 * 维护目标网络 → 出口接口 + 下一跳的映射。
 * 对外仅暴露查询和添加/删除接口，内部实现可替换。
 */

#ifndef ROUTE_H
#define ROUTE_H

#include <stdint.h>
#include "netif.h"

/* -------------------------------------------------------------------------- */
/*  预定义常量                                                                */
/* -------------------------------------------------------------------------- */

#define ROUTE_MAX_ENTRIES 32    /**< 最大路由表条目数 */
#define ROUTE_FLAG_UP 0x01      /**< 路由有效 */
#define ROUTE_FLAG_GATEWAY 0x02 /**< 路由指向网关（非直连） */
#define ROUTE_FLAG_HOST 0x04    /**< 主机路由（掩码为全1） */
#define ROUTE_FLAG_DEFAULT 0x08 /**< 默认路由（0.0.0.0/0） */

/* -------------------------------------------------------------------------- */
/*  类型定义                                                                  */
/* -------------------------------------------------------------------------- */

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

/* -------------------------------------------------------------------------- */
/*  公开函数接口                                                              */
/* -------------------------------------------------------------------------- */

/**
 * @brief 初始化路由表（清空所有条目）。
 */
void route_init(void);

/**
 * @brief 添加一条路由条目。
 * @param dest     目标网络地址（网络字节序）。
 * @param netmask  子网掩码（网络字节序）。
 * @param gateway  下一跳地址（网络字节序），直连网络可填 0。
 * @param ni       出口网络接口。
 * @return 0 成功，-1 表满或参数无效。
 */
int route_add(uint32_t dest, uint32_t netmask, uint32_t gateway,
              struct netif *ni);

/**
 * @brief 删除一条路由条目。
 * @param dest    目标网络地址。
 * @param netmask 子网掩码。
 */
void route_remove(uint32_t dest, uint32_t netmask);

/**
 * @brief 查找路由：根据目标 IP 返回最佳匹配的出口接口和下一跳。
 *        采用最长前缀匹配（Longest Prefix Match）。
 *        本机地址在调用前已由 IP 层自行处理。
 * @param dst_addr  目标 IPv4 地址（网络字节序）。
 * @param ni_out    输出出口网络接口指针。
 * @param next_hop  输出下一跳 IPv4 地址（网络字节序），直连则为目标地址本身。
 * @return 0 找到路由，-1 未找到（目标不可达）。
 */
int route_lookup(uint32_t dst_addr, struct netif **ni_out, uint32_t *next_hop);

/**
 * @brief 添加默认路由（0.0.0.0/0）。
 * @param gateway  默认网关地址。
 * @param ni       出口接口。
 * @return 0 成功，-1 失败。
 */
int route_set_default(uint32_t gateway, struct netif *ni);

/**
 * @brief 打印路由表（调试用）。
 */
void route_dump(void);

#endif /* ROUTE_H */
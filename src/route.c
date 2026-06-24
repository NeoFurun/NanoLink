/**
 * @file    route.c
 * @brief   路由表模块实现
 *
 * 最长前缀匹配（Longest Prefix Match）找出口接口和下一跳。
 *
 * 例:
 *   route_add(10.0.0.0, 255.255.255.0, 0, &tap0);      // 直连 10.0.0.0/24
 *   route_set_default(10.0.0.1, &tap0);                  // 默认网关
 *
 *   route_lookup(10.0.0.50)  → tap0, next_hop=10.0.0.50  (直连)
 *   route_lookup(8.8.8.8)    → tap0, next_hop=10.0.0.1   (走网关)
 */

#include "../include/route.h"
#include <string.h>
#include <stdio.h>

static struct route_entry route_table[ROUTE_MAX_ENTRIES];

/* ==========================================================================
   route_init — 初始化路由表
   ========================================================================== */
/**
 * 例:
 *   route_init();
 *   // route_table[32] 全清零
 */
void route_init(void)
{
    memset(route_table, 0, sizeof(route_table));
}

/* ==========================================================================
   route_add — 添加路由条目
   ========================================================================== */
/**
 * 例（添加直连路由和网关路由）:
 *   route_add(0x0A000000, 0xFFFFFF00, 0, &tap0);
 *     dest=10.0.0.0, netmask=255.255.255.0, gateway=0(直连), flags=UP
 *
 *   route_add(0x0A000100, 0xFFFFFFFF, 0x0A000001, &tap0);
 *     dest=10.0.1.0, netmask=255.255.255.255, gateway=10.0.0.1, flags=UP|GATEWAY|HOST
 */
int route_add(uint32_t dest, uint32_t netmask, uint32_t gateway,
              struct netif *ni)
{
    int i;

    if (ni == NULL) return -1;

    for (i = 0; i < ROUTE_MAX_ENTRIES; i++) {
        if (!(route_table[i].flags & ROUTE_FLAG_UP)) {
            route_table[i].dest    = dest;
            route_table[i].netmask = netmask;
            route_table[i].gateway = gateway;
            route_table[i].flags   = ROUTE_FLAG_UP;
            route_table[i].metric  = 0;
            route_table[i].netif   = ni;

            if (gateway != 0) route_table[i].flags |= ROUTE_FLAG_GATEWAY;
            if (netmask == 0xFFFFFFFF) route_table[i].flags |= ROUTE_FLAG_HOST;
            if (dest == 0 && netmask == 0) route_table[i].flags |= ROUTE_FLAG_DEFAULT;

            return 0;
        }
    }

    return -1; /* 表满 */
}

/* ==========================================================================
   route_remove — 删除路由条目
   ========================================================================== */
/**
 * 例:
 *   route_remove(0x0A000000, 0xFFFFFF00);
 *   // 10.0.0.0/24 条目清零
 */
void route_remove(uint32_t dest, uint32_t netmask)
{
    int i;

    for (i = 0; i < ROUTE_MAX_ENTRIES; i++) {
        if ((route_table[i].flags & ROUTE_FLAG_UP) &&
            route_table[i].dest == dest &&
            route_table[i].netmask == netmask) {
            memset(&route_table[i], 0, sizeof(struct route_entry));
            return;
        }
    }
}

/* ==========================================================================
   route_lookup — 最长前缀匹配查路由
   ========================================================================== */
/**
 * 遍历路由表，找 (dst & netmask) == dest 且 netmask 最长的那个。
 *
 * 例（路由表有两条: 10.0.0.0/24 和 0.0.0.0/0）:
 *   route_lookup(10.0.0.50, &ni, &next):
 *     10.0.0.0/24 → (50 & 0xFFFFFF00)=10.0.0.0 ✓  前缀24 → 最佳!
 *     0.0.0.0/0   → (50 & 0)=0 ✓  前缀0
 *     → ni=tap0, next=10.0.0.50 (直连)
 *
 *   route_lookup(8.8.8.8, &ni, &next):
 *     10.0.0.0/24 → (8.8.8.8 & 0xFFFFFF00) ≠ 10.0.0.0 ✗
 *     0.0.0.0/0   → ✓  前缀0
 *     → ni=tap0, next=10.0.0.1 (网关)
 */
int route_lookup(uint32_t dst_addr, struct netif **ni_out, uint32_t *next_hop)
{
    int i;
    int best_idx = -1;
    int best_prefix = -1;

    for (i = 0; i < ROUTE_MAX_ENTRIES; i++) {
        if (!(route_table[i].flags & ROUTE_FLAG_UP)) continue;

        if ((dst_addr & route_table[i].netmask) == route_table[i].dest) {
            /* 计算前缀长度（netmask 中 1 的个数） */
            uint32_t m = route_table[i].netmask;
            int prefix = 0;
            while (m) {
                prefix++;
                m &= m - 1; /* 清除最低位的 1 */
            }

            if (prefix > best_prefix) {
                best_prefix = prefix;
                best_idx = i;
            }
        }
    }

    if (best_idx < 0) return -1;

    *ni_out = route_table[best_idx].netif;

    if (route_table[best_idx].flags & ROUTE_FLAG_GATEWAY) {
        *next_hop = route_table[best_idx].gateway;
    } else {
        *next_hop = dst_addr; /* 直连，目标即下一跳 */
    }

    return 0;
}

/* ==========================================================================
   route_set_default — 设置默认路由
   ========================================================================== */
/**
 * 例:
 *   route_set_default(0x0A000001, &tap0);
 *   // 添加 0.0.0.0/0 → gateway 10.0.0.1 的路由
 *   // 所有找不到具体路由的包都走这条
 */
int route_set_default(uint32_t gateway, struct netif *ni)
{
    return route_add(0, 0, gateway, ni);
}

/* ==========================================================================
   route_dump — 打印路由表（调试用）
   ========================================================================== */
void route_dump(void)
{
    int i;
    printf("Route Table:\n");
    for (i = 0; i < ROUTE_MAX_ENTRIES; i++) {
        if (route_table[i].flags & ROUTE_FLAG_UP) {
            printf("  [%d] dest=%08x mask=%08x gw=%08x flags=%02x\n",
                   i, route_table[i].dest, route_table[i].netmask,
                   route_table[i].gateway, route_table[i].flags);
        }
    }
}

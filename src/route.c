#include "../include/route.h"
#include <string.h>
#include <stdio.h>

static struct route_entry route_table[ROUTE_MAX_ENTRIES];

void route_init(void)
{
    memset(route_table, 0, sizeof(route_table));
}


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

int route_set_default(uint32_t gateway, struct netif *ni)
{
    return route_add(0, 0, gateway, ni);
}

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

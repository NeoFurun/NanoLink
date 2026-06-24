/**
 * @file    netif.c
 * @brief   网络接口抽象层实现
 *
 * 管理所有网络接口，用单向全局链表串起来。
 * 上层发包时通过 IP + 子网掩码匹配找对应网卡，找不到就走默认网卡。
 *
 * 架构：全局链表 + 回调注入
 *   netif_list — 全局链表头，注册的网卡全串在这里
 *   send       — 驱动提供的发送回调（网卡如何把包发出去）
 *   input      — 协议栈提供的输入回调（网卡收到包后交给谁处理）
 *
 *   数据流向:
 *   发送: 上层 → netif_find_by_ip → ni->send → 驱动 → 网线
 *   接收: 网线 → 驱动 → netif_input → ni->input → 上层协议
 */

#include "../include/netif.h"
#include <string.h>

/* 全局网卡链表头 */
static struct netif *netif_list = NULL;

/* ==========================================================================
   netif_init — 初始化一个网络接口结构体
   ========================================================================== */
/**
 * 把传入的名称、回调函数填入结构体，其余字段清零。
 * 此时网卡还没注册，协议栈看不到它。
 *
 * 例（TAP 驱动初始化时）:
 *   struct netif eth0;
 *   netif_init(&eth0, "eth0", tap_send, ll_dispatch_input);
 *   // eth0.name  = "eth0"
 *   // eth0.send  = tap_send     (驱动发送函数)
 *   // eth0.input = ll_dispatch_input (链路层分发)
 *   // eth0.flags = 0, ip_addr = 0, netmask = 0, next = NULL
 *   // 此时 eth0 还不在全局链表中
 */
void netif_init(struct netif *ni, const char *name,
                netif_send_fn send, netif_input_fn input)
{
    if (ni == NULL) return;

    /* 清零所有字段 */
    memset(ni, 0, sizeof(struct netif));

    /* 安全拷贝名称，保留 '\0' 位置 */
    strncpy(ni->name, name, NETIF_NAME_LEN - 1);
    ni->name[NETIF_NAME_LEN - 1] = '\0';

    ni->send  = send;
    ni->input = input;
}

/* ==========================================================================
   netif_register — 将网卡加入全局链表尾部
   ========================================================================== */
/**
 * 把网卡挂到全局链表，协议栈从此可以找到它。
 * 按注册顺序排列，先注册的在前——也即 netif_get_default 优先选它。
 *
 * 例:
 *   netif_list: [eth0] → NULL
 *   netif_register(&wlan0);
 *   netif_list: [eth0] → [wlan0] → NULL
 *
 * 重复注册（同一个指针再次调用）返回 -1。
 */
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

    ni->next = NULL; /* defense-in-depth */

    return 0;
}

/* ==========================================================================
   netif_unregister — 从全局链表摘除网卡
   ========================================================================== */
/**
 * 拔网线或驱动卸载时调用，协议栈之后不会再碰它。
 *
 * 例:
 *   netif_list: [eth0] → [wlan0] → NULL
 *   netif_unregister(&wlan0);
 *   netif_list: [eth0] → NULL
 *   // wlan0 被摘走，路由查找不再返回它
 *
 * 如果 ni 不在链表中，什么也不做。
 */
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

    if (p == NULL) return; /* 不在链表中 */

    if (prev == NULL) {
        netif_list = ni->next; /* 摘除的是头节点 */
    } else {
        prev->next = ni->next; /* 摘除中间/尾部节点 */
    }

    ni->next = NULL;
}

/* ==========================================================================
   netif_find_by_ip — 根据目标 IP 的子网匹配网卡
   ========================================================================== */
/**
 * 遍历全局链表，用 (目标IP & 掩码) == (网卡IP & 掩码) 找同一子网的网卡。
 * 返回第一个匹配的，都不匹配返回 NULL。
 *
 * 例:
 *   网卡列表:
 *     eth0:  ip_addr=192.168.1.5, netmask=255.255.255.0
 *     wlan0: ip_addr=10.0.0.5,    netmask=255.0.0.0
 *
 *   netif_find_by_ip(192.168.1.100):
 *     eth0:  (100 & 255.255.255.0) = 192.168.1.0 ✓ → 返回 eth0
 *
 *   netif_find_by_ip(10.0.0.99):
 *     eth0:  ✗
 *     wlan0: (99 & 255.0.0.0) = 10.0.0.0 ✓ → 返回 wlan0
 *
 *   netif_find_by_ip(8.8.8.8):
 *     eth0: ✗  wlan0: ✗ → 返回 NULL，上层走默认网关
 */
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

/* ==========================================================================
   netif_get_default — 返回第一个注册的网卡
   ========================================================================== */
/**
 * 直接返回全局链表头。路由找不到匹配网卡时走它。
 *
 * 例:
 *   netif_list: [eth0] → [wlan0] → NULL
 *   netif_get_default() → eth0
 *
 *   空链表则返回 NULL。
 */
struct netif *netif_get_default(void)
{
    return netif_list;
}

/* ==========================================================================
   netif_input — 驱动收到包后丢给上层
   ========================================================================== */
/**
 * 网卡收到以太网帧后，驱动调这个函数把包注入协议栈。
 * 内部调 ni->input 回调，不关心上层具体是什么协议。
 *
 * 例（TAP 驱动收到一个帧）:
 *   struct mbuf *m = mbuf_alloc_large(2048);
 *   // 把帧数据拷进 m ...
 *   netif_input(&eth0, m);
 *     → 内部调 eth0.input(&eth0, m)
 *     → input 指向 ll_dispatch，开始链路层解析
 *
 * 如果 input 回调未设置（NULL），自动释放 mbuf 防止泄漏。
 * 返回 input 回调的返回值，0 表示包被上层消费。
 */
int netif_input(struct netif *ni, struct mbuf *m)
{
    if (ni == NULL || m == NULL) return -1;

    if (ni->input == NULL) {
        mbuf_free(m);
        return -1;
    }

    return ni->input(ni, m);
}

/* ==========================================================================
   netif_set_up — 启用网卡
   ========================================================================== */
/**
 * 给 flags 置上 NETIF_FLAG_UP 位。上层发包前检查此标志，DOWN 的网卡不发。
 *
 * 例:
 *   netif_set_up(&eth0);
 *   // eth0.flags: 0x00 → 0x01 (UP)
 */
void netif_set_up(struct netif *ni)
{
    if (ni == NULL) return;
    ni->flags |= NETIF_FLAG_UP;
}

/* ==========================================================================
   netif_set_down — 停用网卡
   ========================================================================== */
/**
 * 清除 NETIF_FLAG_UP 位。网线拔了或手动关闭时调。
 *
 * 例:
 *   netif_set_down(&eth0);
 *   // eth0.flags: 0x01 → 0x00 (DOWN)
 *   // 之后发包跳过此网卡
 */
void netif_set_down(struct netif *ni)
{
    if (ni == NULL) return;
    ni->flags &= ~NETIF_FLAG_UP;
}

/**
 * @file    main.c
 * @brief   NanoLink 协议栈入口
 *
 * 用法: sudo ./nanolink <接口名> <IP> [netmask]
 * 例:   sudo ./nanolink tap0 10.0.0.1 255.255.255.0
 *
 * 启动后:
 *   sudo ip link set tap0 up
 *   ping 10.0.0.1           # 测试 ICMP
 *   echo "hi" | nc -u ...   # 测试 UDP
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include "../include/tap.h"
#include "../include/netif.h"
#include "../include/mbuf.h"
#include "../include/timer.h"
#include "../include/arp.h"
#include "../include/ip.h"
#include "../include/icmp.h"
#include "../include/udp.h"
#include "../include/tcp.h"
#include "../include/socket.h"
#include "../include/route.h"
#include "../include/event_bus.h"
#include "../include/ll_dispatch.h"
#include "../include/osal.h"

static struct netif *g_netif = NULL;
static volatile int running = 1;

static void sig_handler(int sig) {
    (void)sig;
    running = 0;
}

/* 把 "10.0.0.1" 转成 uint32_t（网络字节序：
 *   10.0.0.1 → 内存中字节为 [0x0A, 0x00, 0x00, 0x01]） */
static uint32_t parse_ip(const char *str)
{
    uint32_t a, b, c, d;
    if (sscanf(str, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return 0;
    return (uint32_t)(a | (b << 8) | (c << 16) | (d << 24));
}

/* 把 "255.255.255.0" 转成 uint32_t（网络字节序） */
static uint32_t parse_netmask(const char *str)
{
    uint32_t a, b, c, d;
    if (sscanf(str, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return 0xFFFFFF00;
    return (uint32_t)(a | (b << 8) | (c << 16) | (d << 24));
}

/* 主循环每秒驱动一次定时器和维护任务 */
static void main_loop_tick(void)
{
    static uint32_t last_tick = 0;
    uint32_t now = osal_get_time_ms();

    if (now - last_tick >= 1000) {
        last_tick = now;
        timer_tick(1000);
        arp_tick();
        ip_frag_tick();
        tcp_slow_tick();
    }

    /* 快定时器 200ms */
    static uint32_t last_fast = 0;
    if (now - last_fast >= 200) {
        last_fast = now;
        tcp_fast_tick();
    }
}

/* 从 TAP 读取数据并注入协议栈 */
static void tap_poll(struct netif *ni)
{
    int fd;
    uint8_t buf[2048];
    ssize_t n;
    struct mbuf *m;

    if (ni == NULL || ni->priv == NULL) return;
    fd = *(int *)ni->priv;

    while (1) {
        n = read(fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            perror("[tap] read error");
            break;
        }
        if (n == 0) break;


        /* SMALL mbuf 只有 64 字节可用数据，超过的用 LARGE */
        if (n > 64) {
            m = mbuf_alloc_large((uint16_t)n);
            if (m != NULL && mbuf_append(m, (uint16_t)n) != 0) {
                mbuf_free(m);
                m = NULL;
            }
        } else {
            m = mbuf_alloc();
            if (m != NULL && mbuf_append(m, (uint16_t)n) != 0) {
                mbuf_free(m);
                m = NULL;
            }
        }

        if (m == NULL) continue;

        mbuf_copy_from(m, buf, (uint16_t)n);
        netif_input(ni, m);
    }
}

int main(int argc, char *argv[])
{
    const char *ifname = "tap0";
    const char *ip_str = "10.0.0.1";
    const char *mask_str = "255.255.255.0";
    uint32_t ip_addr, netmask;

    if (argc >= 2) ifname   = argv[1];
    if (argc >= 3) ip_str   = argv[2];
    if (argc >= 4) mask_str = argv[3];

    ip_addr = parse_ip(ip_str);
    netmask = parse_netmask(mask_str);

    if (ip_addr == 0) {
        fprintf(stderr, "Invalid IP: %s\n", ip_str);
        return 1;
    }

    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);

    printf("NanoLink starting on %s (%s/%s)...\n", ifname, ip_str, mask_str);

    /* 初始化各模块 */
    osal_init();
    mbuf_init();
    timer_init();
    netif_init(NULL, "", NULL, NULL); /* 仅触发链接 */
    socket_init();
    event_bus_init();
    route_init();
    arp_init();
    ip_init();
    icmp_init();
    udp_init();
    tcp_init();

    /* 注册 TAP 网卡 */
    g_netif = driver_register(ifname, &tap_ops, NULL);
    if (g_netif == NULL) {
        fprintf(stderr, "Failed to register interface %s\n", ifname);
        return 1;
    }

    /* 初始化驱动 */
    driver_init_all();

    /* 配置 IP */
    ip_set_address(g_netif, ip_addr, netmask);

    /* 添加直连路由 */
    route_add(ip_addr & netmask, netmask, 0, g_netif);

    /* 设置默认路由 (网关 = 网段第一个地址或 .1) */
    {
        uint32_t gw = (ip_addr & netmask) | 0x01000000; /* 假设网关是 .1 */
        route_set_default(gw, g_netif);
    }

    /* 设置 ll_dispatch 为 netif 的 input 回调 */
    g_netif->input = ll_dispatch_input;

    printf("Interface %s ready. IP=%s, Mask=%s\n",
           g_netif->name, ip_str, mask_str);
    printf("Run: sudo ip link set %s up && sudo ip addr add %s/24 dev %s\n",
           ifname, ip_str, ifname);

    /* 主循环 */
    while (running) {
        tap_poll(g_netif);
        main_loop_tick();
        usleep(10000); /* 10ms，降低 CPU 占用 */
    }

    printf("\nShutting down...\n");
    driver_unregister(g_netif);
    printf("Goodbye.\n");

    return 0;
}

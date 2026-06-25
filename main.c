/**
 * @file    main.c
 * @brief   NanoLink 协议栈入口
 *
 * 用法: sudo ./nanolink <接口名> <IP> [netmask]
 * 例:   sudo ./nanolink tap0 10.0.0.1 255.255.255.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
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

/* TCP echo 回调：收到数据后原样回复 */
static void tcp_echo_recv(struct tcp_pcb *pcb, struct mbuf *m)
{
    if (m != NULL) {
        printf("[tcp-echo] recv %d bytes, echoing\n", m->total_len);
        tcp_send(pcb, m);
    } else {
        printf("[tcp-echo] connection closed\n");
    }
}

static void tcp_echo_accept(struct tcp_pcb *new_pcb)
{
    printf("[tcp-echo] new connection accepted\n");
    tcp_set_recv_callback(new_pcb, tcp_echo_recv);
}

static void sig_handler(int sig) {
    (void)sig;
    running = 0;
}

static uint32_t parse_ip(const char *str)
{
    uint32_t a, b, c, d;
    if (sscanf(str, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return 0;
    return (uint32_t)(a | (b << 8) | (c << 16) | (d << 24));
}

static uint32_t parse_netmask(const char *str)
{
    uint32_t a, b, c, d;
    if (sscanf(str, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return 0xFFFFFF00;
    return (uint32_t)(a | (b << 8) | (c << 16) | (d << 24));
}

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

    static uint32_t last_fast = 0;
    if (now - last_fast >= 200) {
        last_fast = now;
        tcp_fast_tick();
    }
}

/* 从 TAP 读取数据并注入协议栈（非阻塞） */
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
        if (n <= 0) break;

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

    osal_init();
    mbuf_init();
    timer_init();
    netif_init(NULL, "", NULL, NULL);
    socket_init();
    event_bus_init();
    route_init();
    arp_init();
    ip_init();
    icmp_init();
    udp_init();
    tcp_init();

    g_netif = driver_register(ifname, &tap_ops, NULL);
    if (g_netif == NULL) {
        fprintf(stderr, "Failed to register interface %s\n", ifname);
        return 1;
    }

    driver_init_all();
    ip_set_address(g_netif, ip_addr, netmask);
    route_add(ip_addr & netmask, netmask, 0, g_netif);

    {
        uint32_t gw = (ip_addr & netmask) | 0x01000000;
        route_set_default(gw, g_netif);
    }

    g_netif->input = ll_dispatch_input;

    /* 添加内核侧 ARP 条目（避免 TCP 握手时 ARP 延迟） */
    {
        /* 10.0.0.2 在内核侧，TAP 设备默认 MAC 通常是随机的，
         * 用 ioctl SIOCGIFHWADDR 获取或手动指定。
         * 简化：从 tap0 的邻居表读，或用伪 MAC。
         * 这里先留空让 ARP 自动解析。
         */
    }

    printf("Interface %s ready. IP=%s, Mask=%s\n",
           g_netif->name, ip_str, mask_str);
    printf("Run: sudo ip link set %s up && sudo ip addr add %s/24 dev %s\n",
           ifname, ip_str, ifname);

    /* TCP echo 测试：监听 9999 端口 */
    struct tcp_pcb *tcp_listen_pcb = tcp_new();
    if (tcp_listen_pcb != NULL) {
        tcp_bind(tcp_listen_pcb, ip_addr, 9999);     /* 绑定到实际 IP */
        tcp_listen(tcp_listen_pcb, 5);               /* backlog 5 */
        tcp_set_accept_callback(tcp_listen_pcb, tcp_echo_accept);
        printf("TCP echo server listening on port 9999\n");
    }

    /* UDP echo 测试：监听 8888 端口 */
    struct socket *echo_sock = socket_create(SOCK_DGRAM);
    struct sockaddr_in bind_addr;
    char echo_buf[1024];

    if (echo_sock != NULL) {
        memset(&bind_addr, 0, sizeof(bind_addr));
        bind_addr.sin_family = AF_INET;
        bind_addr.sin_port   = 8888;
        bind_addr.sin_addr   = 0;
        socket_bind(echo_sock, &bind_addr);
        printf("UDP echo server listening on port 8888\n");
    }

    /* 主循环 */
    while (running) {
        tap_poll(g_netif);
        main_loop_tick();

        if (echo_sock != NULL) {
            struct sockaddr_in remote_addr;
            int n = socket_recv(echo_sock, echo_buf, sizeof(echo_buf), &remote_addr);
            if (n > 0) {
                printf("[udp-echo] recv %d bytes, echoing back\n", n);
                socket_send(echo_sock, echo_buf, (uint16_t)n, &remote_addr);
            }
        }
        usleep(5000); /* 5ms */
    }

    printf("\nShutting down...\n");
    if (echo_sock) socket_close(echo_sock);
    driver_unregister(g_netif);
    printf("Goodbye.\n");

    return 0;
}

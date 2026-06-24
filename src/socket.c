/**
 * @file    socket.c
 * @brief   Socket 抽象层实现
 *
 * 提供类 BSD socket API，通过 transport_ops 统一 UDP/TCP 差异。
 * 接收路径: 协议层回调 → 数据进 recv_queue → socket_recv 取出
 *
 * 使用层次:
 *   应用层 → socket_send/socket_recv → transport_ops → udp_send/tcp_send → ip_output
 */

#include "../include/socket.h"
#include "../include/udp.h"
#include "../include/osal.h"
#include <string.h>

static struct socket socket_pool[SOCKET_MAX];
static struct socket *socket_free_list = NULL;

/* Socket 接收回调（匹配 udp_recv_fn 签名，忽略 src_ip/src_port） */
static void socket_recv_callback(struct udp_pcb *pcb, struct mbuf *m,
                                  uint32_t src_ip, uint16_t src_port)
{
    struct socket *sock;
    int i;
    (void)src_ip;
    (void)src_port;

    if (m == NULL) return;

    /* 找到关联此 pcb 的 socket */
    for (i = 0; i < SOCKET_MAX; i++) {
        if (socket_pool[i].pcb == pcb && socket_pool[i].state != SOCK_STATE_FREE) {
            sock = &socket_pool[i];
            break;
        }
    }
    if (i >= SOCKET_MAX) {
        mbuf_free(m);
        return;
    }

    /* 追加到接收队列 */
    if (sock->recv_queue == NULL) {
        sock->recv_queue = m;
    } else {
        struct mbuf *q = sock->recv_queue;
        while (q->next_pkt != NULL) q = q->next_pkt;
        q->next_pkt = m;
    }
    m->next_pkt = NULL;
}

/* ==========================================================================
   socket_init — 初始化 socket 层
   ========================================================================== */
/**
 * 把 socket_pool[16] 串成空闲链表。
 *
 * 例（协议栈启动）:
 *   socket_init();
 *   // free_list: s0 → s1 → ... → s15 → NULL
 */
void socket_init(void)
{
    int i;
    for (i = 0; i < SOCKET_MAX - 1; i++) {
        socket_pool[i].next = &socket_pool[i + 1];
    }
    socket_pool[SOCKET_MAX - 1].next = NULL;
    socket_free_list = &socket_pool[0];
}

/* ==========================================================================
   socket_create — 创建一个 socket
   ========================================================================== */
/**
 * 例（创建 UDP socket）:
 *   struct socket *s = socket_create(SOCK_DGRAM);
 *   // s→type=SOCK_DGRAM, s→pcb=udp_pcb, s→ops=udp_ops
 *
 *   创建 TCP socket:
 *   struct socket *s = socket_create(SOCK_STREAM);
 *   // 先创建占位，后续 connect/listen 时才真正创建 TCP PCB
 */
struct socket *socket_create(int type)
{
    struct socket *sock;

    if (socket_free_list == NULL) return NULL;

    sock = socket_free_list;
    socket_free_list = sock->next;

    memset(sock, 0, sizeof(struct socket));
    sock->type  = (uint8_t)type;
    sock->state = SOCK_STATE_ALLOC;

    if (type == SOCK_DGRAM) {
        struct udp_pcb *pcb = udp_new();
        if (pcb == NULL) {
            sock->next = socket_free_list;
            socket_free_list = sock;
            return NULL;
        }
        sock->pcb = pcb;
        udp_set_recv_callback(pcb, socket_recv_callback);
    }
    /* SOCK_STREAM 的 PCB 在 connect/listen 时创建 */

    return sock;
}

/* ==========================================================================
   socket_close — 关闭 socket
   ========================================================================== */
/**
 * 例:
 *   socket_close(s);
 *   // 释放 recv_queue 上的数据，释放 PCB，socket 归还池
 */
void socket_close(struct socket *sock)
{
    if (sock == NULL || sock->state == SOCK_STATE_FREE) return;

    /* 释放接收队列 */
    while (sock->recv_queue != NULL) {
        struct mbuf *m = sock->recv_queue;
        sock->recv_queue = m->next_pkt;
        mbuf_free(m);
    }

    /* 释放 PCB */
    if (sock->pcb != NULL) {
        if (sock->type == SOCK_DGRAM) {
            udp_remove((struct udp_pcb *)sock->pcb);
        }
        /* TCP PCB cleanup not yet implemented */
        sock->pcb = NULL;
    }

    /* 归还 socket 池 */
    sock->state = SOCK_STATE_FREE;
    sock->next  = socket_free_list;
    socket_free_list = sock;
}

/* ==========================================================================
   socket_bind — 绑定本地地址
   ========================================================================== */
int socket_bind(struct socket *sock, const struct sockaddr_in *addr)
{
    if (sock == NULL || sock->state < SOCK_STATE_ALLOC || addr == NULL) return -1;

    if (sock->type == SOCK_DGRAM) {
        struct udp_pcb *pcb = (struct udp_pcb *)sock->pcb;
        return udp_bind(pcb, addr->sin_addr, addr->sin_port);
    }
    /* TCP 暂未实现 */
    return -1;
}

/* ==========================================================================
   socket_connect — 连接远端
   ========================================================================== */
int socket_connect(struct socket *sock, const struct sockaddr_in *addr)
{
    if (sock == NULL || addr == NULL) return -1;

    if (sock->type == SOCK_DGRAM) {
        struct udp_pcb *pcb = (struct udp_pcb *)sock->pcb;
        udp_connect(pcb, addr->sin_addr, addr->sin_port);
        sock->state = SOCK_STATE_CONNECTED;
        return 0;
    }
    /* TCP 暂未实现 */
    return -1;
}

/* ==========================================================================
   socket_listen — 开始监听（仅 TCP）
   ========================================================================== */
int socket_listen(struct socket *sock, uint8_t backlog)
{
    if (sock == NULL || sock->type != SOCK_STREAM) return -1;
    /* TCP 暂未实现 */
    (void)backlog;
    return -1;
}

/* ==========================================================================
   socket_accept — 接受新连接（仅 TCP）
   ========================================================================== */
struct socket *socket_accept(struct socket *listen_sock)
{
    if (listen_sock == NULL || listen_sock->type != SOCK_STREAM) return NULL;
    /* TCP 暂未实现 */
    return NULL;
}

/* ==========================================================================
   socket_send — 发送数据
   ========================================================================== */
/**
 * 例（UDP 发送 "Hello" 到 10.0.0.2:8080）:
 *   struct sockaddr_in addr = {AF_INET, 8080, 10.0.0.2};
 *   socket_send(sock, "Hello", 5, &addr);
 *
 *   内部:
 *     1. mbuf_alloc + mbuf_append(5) + mbuf_copy_from
 *     2. udp_send(pcb, m, 10.0.0.2, 8080)
 */
int socket_send(struct socket *sock, const void *data, uint16_t len,
                const struct sockaddr_in *addr)
{
    struct mbuf *m;
    uint32_t dst_ip = 0;
    uint16_t dst_port = 0;
    int ret;

    if (sock == NULL || data == NULL || len == 0) return -1;

    m = mbuf_alloc();
    if (m == NULL) return -1;

    if (mbuf_append(m, len) != 0) {
        mbuf_free(m);
        return -1;
    }

    mbuf_copy_from(m, data, len);

    if (addr != NULL) {
        dst_ip   = addr->sin_addr;
        dst_port = addr->sin_port;
    }

    if (sock->type == SOCK_DGRAM) {
        ret = udp_send((struct udp_pcb *)sock->pcb, m, dst_ip, dst_port);
    } else {
        /* TCP 暂未实现 */
        mbuf_free(m);
        return -1;
    }

    return (ret == 0) ? len : -1;
}

/* ==========================================================================
   socket_recv — 接收数据
   ========================================================================== */
/**
 * 从 recv_queue 取数据拷到用户 buf。非阻塞。
 *
 * 返回值:
 *   > 0  : 实际读取的字节数
 *   0    : 当前无数据可读（稍后再试）
 *   -1   : 连接已关闭 或 参数错误
 *
 * 例:
 *   char buf[1024];
 *   int n = socket_recv(sock, buf, sizeof(buf), NULL);
 *   if (n > 0)      { ... 处理数据 ... }
 *   else if (n == 0) { ... 暂无数据，稍后重试 ... }
 *   else             { ... n == -1: 连接关闭或出错 ... }
 */
int socket_recv(struct socket *sock, void *buf, uint16_t len,
                struct sockaddr_in *addr)
{
    struct mbuf *m;
    uint16_t copied;

    if (sock == NULL || buf == NULL) return -1;

    m = sock->recv_queue;
    if (m == NULL) {
        /* recv_queue 为空但已关闭 → 通知调用者连接结束 */
        if (sock->state == SOCK_STATE_CLOSED) return -1;
        return 0;
    }

    copied = mbuf_copy_to(m, buf, len);

    /* 如果整个 mbuf 被读完，从队列摘除 */
    if (copied >= m->total_len) {
        sock->recv_queue = m->next_pkt;
        mbuf_free(m);
    } else {
        /* 只读了一部分，pull 掉已读的 */
        mbuf_pull(m, copied);
    }

    if (addr != NULL) {
        /* 简化：暂不记录源地址 */
        memset(addr, 0, sizeof(*addr));
    }

    return copied;
}

/* ==========================================================================
   socket_shutdown — 关闭读写
   ========================================================================== */
void socket_shutdown(struct socket *sock, int how)
{
    if (sock == NULL) return;
    /* 简化实现：标记状态 */
    (void)how;
}

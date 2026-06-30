#include "../include/udp.h"
#include "../include/ip.h"
#include "../include/osal.h"
#include <string.h>

static struct udp_pcb *udp_pcb_list = NULL;

/* 16 位字节序翻转 */
static inline uint16_t swap16(uint16_t n) {
    return (uint16_t)((n >> 8) | (n << 8));
}

/* UDP 校验和（覆盖伪头部 + UDP头 + 数据，填 0 表示不校验） */
__attribute__((unused))
static uint16_t udp_checksum(struct udp_header *udp, uint16_t len,
                              uint32_t src_ip, uint32_t dst_ip)
{
    uint32_t sum = 0;
    uint16_t *p = (uint16_t *)udp;
    int i;

    /* 伪头部 */
    sum += (uint16_t)(src_ip >> 16);
    sum += (uint16_t)(src_ip & 0xFFFF);
    sum += (uint16_t)(dst_ip >> 16);
    sum += (uint16_t)(dst_ip & 0xFFFF);
    sum += swap16(IP_PROTO_UDP);
    sum += swap16(len);

    /* UDP 头 + 数据 */
    for (i = 0; i < len / 2; i++) {
        sum += p[i];
    }
    if (len & 1) {
        sum += ((uint8_t *)udp)[len - 1] << 8;
    }

    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }

    return (uint16_t)(~sum);
}


void udp_init(void)
{
    ip_register_proto(IP_PROTO_UDP, udp_input);
}

struct udp_pcb *udp_new(void)
{
    struct udp_pcb *pcb;

    pcb = (struct udp_pcb *)osal_malloc(sizeof(struct udp_pcb));
    if (pcb == NULL) return NULL;

    memset(pcb, 0, sizeof(struct udp_pcb));
    pcb->next = udp_pcb_list;
    udp_pcb_list = pcb;

    return pcb;
}


int udp_bind(struct udp_pcb *pcb, uint32_t local_ip, uint16_t port)
{
    struct udp_pcb *p;

    if (pcb == NULL) return -1;

    /* 检查端口是否被占用 */
    for (p = udp_pcb_list; p != NULL; p = p->next) {
        if (p != pcb && (p->flags & UDP_FLAG_BOUND) &&
            p->local_port == port && p->local_ip == local_ip) {
            return -1;
        }
    }

    pcb->local_ip   = local_ip;
    pcb->local_port = port;
    pcb->flags |= UDP_FLAG_BOUND;

    return 0;
}

void udp_connect(struct udp_pcb *pcb, uint32_t remote_ip, uint16_t remote_port)
{
    if (pcb == NULL) return;
    pcb->remote_ip   = remote_ip;
    pcb->remote_port = remote_port;
}

void udp_set_recv_callback(struct udp_pcb *pcb, udp_recv_fn callback)
{
    if (pcb == NULL) return;
    pcb->recv_callback = callback;
}


int udp_send(struct udp_pcb *pcb, struct mbuf *m, uint32_t dst_ip, uint16_t dst_port)
{
    struct udp_header *udp;
    uint16_t total_len;

    if (pcb == NULL || m == NULL) return -1;

    /* 若已 connect，可用默认值 */
    if (dst_ip == 0)   dst_ip   = pcb->remote_ip;
    if (dst_port == 0) dst_port = pcb->remote_port;

    if (dst_ip == 0 || dst_port == 0) {
        mbuf_free(m);
        return -1;
    }

    if (mbuf_prepend(m, UDP_HEADER_LEN) != 0) {
        mbuf_free(m);
        return -1;
    }

    total_len = m->total_len;

    udp = (struct udp_header *)m->data;
    udp->src_port = swap16(pcb->local_port);
    udp->dst_port = swap16(dst_port);
    udp->length   = swap16(total_len);
    udp->checksum = 0; /* 暂不校验 */

    return ip_output(dst_ip, IP_PROTO_UDP, m);
}


void udp_remove(struct udp_pcb *pcb)
{
    struct udp_pcb *p, *prev;

    if (pcb == NULL) return;

    prev = NULL;
    p = udp_pcb_list;
    while (p != NULL && p != pcb) {
        prev = p;
        p = p->next;
    }

    if (p == NULL) return;

    if (prev == NULL) {
        udp_pcb_list = pcb->next;
    } else {
        prev->next = pcb->next;
    }

    osal_free(pcb);
}

int udp_input(struct netif *ni, struct mbuf *m, uint32_t src_ip, uint32_t dst_ip)
{
    (void)dst_ip;
    struct udp_header *udp;
    struct udp_pcb *pcb;
    uint16_t src_port, dst_port;

    if (ni == NULL || m == NULL) return -1;
    if (m->len < UDP_HEADER_LEN) {
        mbuf_free(m);
        return -1;
    }

    udp = (struct udp_header *)m->data;
    src_port = swap16(udp->src_port);
    dst_port = swap16(udp->dst_port);

    /* 按端口找 PCB */
    for (pcb = udp_pcb_list; pcb != NULL; pcb = pcb->next) {
        if (!(pcb->flags & UDP_FLAG_BOUND)) continue;
        if (pcb->local_port != dst_port) continue;

        /* 如果 PCB 限制了远端地址，检查是否匹配 */
        if (pcb->remote_port != 0 && pcb->remote_port != src_port) continue;
        if (pcb->remote_ip != 0 && pcb->remote_ip != src_ip) continue;

        /* 跳过 UDP 头，data 指向应用数据 */
        mbuf_pull(m, UDP_HEADER_LEN);

        if (pcb->recv_callback != NULL) {
            pcb->recv_callback(pcb, m, src_ip, src_port);
            return 0;
        }

        /* 无回调，丢弃 */
        mbuf_free(m);
        return -1;
    }

    /* 无匹配端口，丢弃（TODO: 发送 ICMP 端口不可达） */
    mbuf_free(m);
    return -1;
}

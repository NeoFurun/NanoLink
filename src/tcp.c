#include "../include/tcp.h"
#include "../include/ip.h"
#include "../include/timer.h"
#include "../include/osal.h"
#include <string.h>

/* PCB 池 */
static struct tcp_pcb tcp_pcb_pool[TCP_MAX_PCB];
static struct tcp_pcb *tcp_free_list = NULL;
static struct tcp_pcb *tcp_active_list = NULL;

/* accept 等待队列 (已完成的连接等待 accept) */
static struct tcp_pcb *tcp_accept_queue = NULL;

static inline uint16_t swap16(uint16_t n) { return (uint16_t)((n >> 8) | (n << 8)); }
static inline uint32_t swap32(uint32_t n) {
    return ((n >> 24) & 0xFF) | ((n >> 8) & 0xFF00) |
           ((n << 8) & 0xFF0000) | ((n << 24) & 0xFF000000);
}

static uint32_t tcp_initial_seq = 0;


/* TCP 校验和 (伪头部 + TCP段) */
static uint16_t tcp_checksum(struct tcp_header *tcp, uint16_t tcp_len,
                              uint32_t src_ip, uint32_t dst_ip)
{
    uint32_t sum = 0;
    uint8_t *sip = (uint8_t *)&src_ip;
    uint8_t *dip = (uint8_t *)&dst_ip;
    uint8_t *b   = (uint8_t *)tcp;
    int i;

    /* 伪头部 — 网络字节序 */
    sum += ((uint16_t)sip[0] << 8) | sip[1];
    sum += ((uint16_t)sip[2] << 8) | sip[3];
    sum += ((uint16_t)dip[0] << 8) | dip[1];
    sum += ((uint16_t)dip[2] << 8) | dip[3];
    sum += ((uint16_t)0 << 8) | IP_PROTO_TCP;
    sum += ((uint16_t)(tcp_len >> 8) << 8) | (tcp_len & 0xFF);

    /* TCP 段 — 按网络字节序逐字节读取 */
    for (i = 0; i < tcp_len - 1; i += 2) {
        sum += ((uint16_t)b[i] << 8) | b[i + 1];
    }
    if (tcp_len & 1) {
        sum += (uint16_t)b[tcp_len - 1] << 8;
    }

    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)(~sum);
}

/* 生成初始序列号 */
static uint32_t tcp_gen_seq(void) {
    tcp_initial_seq += 64000;
    return tcp_initial_seq;
}

/* 发送一个 TCP 段 (只包含 TCP 头，无数据) */
static int tcp_send_segment(struct tcp_pcb *pcb, uint8_t flags,
                             uint32_t seq, uint32_t ack, struct mbuf *data)
{
    struct tcp_header *tcp;
    struct mbuf *m;
    uint16_t data_len = 0;
    uint16_t tcp_len;
    uint8_t buf[2048];

    if (data != NULL) data_len = data->total_len;
    tcp_len = TCP_HEADER_LEN_MIN + data_len;

    if (tcp_len > sizeof(buf)) {
        if (data != NULL) mbuf_free(data);
        return -1;
    }

    /* 直接在 flat buffer 中构建 TCP 段，确保校验和计算在连续内存上进行 */
    tcp = (struct tcp_header *)buf;
    memset(tcp, 0, TCP_HEADER_LEN_MIN);

    tcp->src_port = swap16(pcb->local_port);
    tcp->dst_port = swap16(pcb->remote_port);
    tcp->seq_num  = swap32(seq);
    tcp->ack_num  = swap32(ack);
    tcp->hdrlen_flags = swap16((TCP_HEADER_LEN_MIN / 4) << 12 | flags);
    tcp->window   = swap16(pcb->rcv_wnd);
    tcp->checksum = 0;
    tcp->urg_ptr  = 0;

    /* 拷贝数据到头部之后 */
    if (data != NULL) {
        mbuf_copy_to(data, buf + TCP_HEADER_LEN_MIN, data_len);
        mbuf_free(data);
    }

    /* 在连续内存上计算校验和 */
    {
        uint16_t cs = tcp_checksum(tcp, tcp_len, pcb->local_ip, pcb->remote_ip);
        tcp->checksum = swap16(cs);
    }

    /* 拷贝到 mbuf 并发送 */
    m = mbuf_alloc();
    if (m == NULL) return -1;
    mbuf_append(m, tcp_len);
    mbuf_copy_from(m, buf, tcp_len);

    return ip_output(pcb->remote_ip, IP_PROTO_TCP, m);
}

/* 发送缓冲中的数据 */
static int tcp_output(struct tcp_pcb *pcb)
{
    uint32_t seq;
    struct mbuf *m;
    uint16_t seg_len;
    uint8_t flags;

    if (pcb->state != TCP_STATE_ESTABLISHED &&
        pcb->state != TCP_STATE_CLOSE_WAIT) return -1;

    if (pcb->unsent_queue == NULL) return 0;

    m    = pcb->unsent_queue;
    seq  = pcb->snd_nxt;

    /* 简单处理：一次发一个包，不超过 MSS */
    seg_len = m->len;
    if (seg_len > pcb->mss) {
        seg_len = pcb->mss;
        /* 实际应该分片 */
    }

    flags = TCP_FLAG_ACK;

    /* 从 unsent 移到 unacked */
    pcb->unsent_queue = m->next_pkt;
    m->next_pkt = NULL;

    if (pcb->unacked_queue == NULL) {
        pcb->unacked_queue = m;
    } else {
        struct mbuf *q = pcb->unacked_queue;
        while (q->next_pkt != NULL) q = q->next_pkt;
        q->next_pkt = m;
    }

    pcb->snd_nxt += seg_len;

    return tcp_send_segment(pcb, flags, seq, pcb->rcv_nxt, m);
}

/* 从链表摘除 PCB */
static void tcp_pcb_remove(struct tcp_pcb *pcb)
{
    struct tcp_pcb *p, *prev;

    prev = NULL;
    p = tcp_active_list;
    while (p != NULL && p != pcb) { prev = p; p = p->next; }
    if (p == NULL) return;
    if (prev == NULL) tcp_active_list = pcb->next;
    else prev->next = pcb->next;
}

/* 释放 PCB 资源 */
static void tcp_pcb_free(struct tcp_pcb *pcb)
{
    if (pcb == NULL) return;

    while (pcb->unsent_queue  != NULL) { struct mbuf *m = pcb->unsent_queue;  pcb->unsent_queue  = m->next_pkt; mbuf_free(m); }
    while (pcb->unacked_queue != NULL) { struct mbuf *m = pcb->unacked_queue; pcb->unacked_queue = m->next_pkt; mbuf_free(m); }
    while (pcb->out_of_order != NULL)  { struct mbuf *m = pcb->out_of_order;  pcb->out_of_order  = m->next_pkt; mbuf_free(m); }
    while (pcb->recv_queue    != NULL) { struct mbuf *m = pcb->recv_queue;    pcb->recv_queue    = m->next_pkt; mbuf_free(m); }

    if (pcb->retransmit_timer != NULL) { timer_delete(pcb->retransmit_timer); pcb->retransmit_timer = NULL; }
    if (pcb->timewait_timer   != NULL) { timer_delete(pcb->timewait_timer);   pcb->timewait_timer   = NULL; }

    tcp_pcb_remove(pcb);
    memset(pcb, 0, sizeof(struct tcp_pcb));
    pcb->next = tcp_free_list;
    tcp_free_list = pcb;
}

/* 重传定时器回调 */
static void tcp_retransmit_callback(void *arg)
{
    struct tcp_pcb *pcb = (struct tcp_pcb *)arg;

    if (pcb->retransmit_count >= 3) {
        /* 超时，放弃连接。先将 retransmit_timer 置 NULL，
         * 防止 tcp_pcb_free 中 timer_delete 操作正在触发的定时器。 */
        pcb->retransmit_timer = NULL;
        if (pcb->err_callback) pcb->err_callback(pcb, -1);
        tcp_pcb_free(pcb);
        return;
    }

    /* 把 unacked 队列移回 unsent 头部 */
    if (pcb->unacked_queue != NULL) {
        struct mbuf *q = pcb->unacked_queue;
        uint32_t total = 0;
        while (q->next_pkt != NULL) q = q->next_pkt;
        q->next_pkt = pcb->unsent_queue;
        pcb->unsent_queue = pcb->unacked_queue;
        pcb->unacked_queue = NULL;
        /* 累加所有被移回 unsent 队列的数据长度 */
        for (q = pcb->unsent_queue; q != NULL; q = q->next_pkt)
            total += q->total_len;
        pcb->snd_nxt -= total;
    }

    pcb->retransmit_count++;
    timer_start(pcb->retransmit_timer);
    tcp_output(pcb);
}

/* TIME-WAIT 定时器回调 */
static void tcp_timewait_callback(void *arg)
{
    struct tcp_pcb *pcb = (struct tcp_pcb *)arg;
    tcp_pcb_free(pcb);
}

/* 在监听 PCB 上创建新连接 PCB */
static struct tcp_pcb *tcp_new_connection(struct tcp_pcb *listen_pcb,
                                           uint32_t remote_ip, uint16_t remote_port,
                                           uint32_t rcv_nxt)
{
    struct tcp_pcb *pcb;

    if (tcp_free_list == NULL) return NULL;

    pcb = tcp_free_list;
    tcp_free_list = pcb->next;

    memset(pcb, 0, sizeof(struct tcp_pcb));
    pcb->local_ip    = listen_pcb->local_ip;
    pcb->local_port  = listen_pcb->local_port;
    pcb->remote_ip   = remote_ip;
    pcb->remote_port = remote_port;
    pcb->state       = TCP_STATE_SYN_RECV;
    pcb->snd_una     = tcp_gen_seq();
    pcb->snd_nxt     = pcb->snd_una + 1; /* SYN 占一个序号 */
    pcb->rcv_nxt     = rcv_nxt + 1;
    pcb->rcv_wnd     = TCP_DEFAULT_WINDOW;
    pcb->snd_wnd     = TCP_DEFAULT_WINDOW;
    pcb->mss         = TCP_DEFAULT_MSS;
    pcb->rto         = 1000;
    pcb->retransmit_timer = timer_create(pcb->rto, TIMER_TYPE_ONCE,
                                          tcp_retransmit_callback, pcb);

    /* 继承 listen PCB 的回调 */
    pcb->recv_callback   = listen_pcb->recv_callback;
    pcb->accept_callback = listen_pcb->accept_callback;
    pcb->sent_callback   = listen_pcb->sent_callback;
    pcb->err_callback    = listen_pcb->err_callback;

    /* 加入活跃链表 */
    pcb->next = tcp_active_list;
    tcp_active_list = pcb;

    return pcb;
}


void tcp_init(void)
{
    int i;
    for (i = 0; i < TCP_MAX_PCB - 1; i++)
        tcp_pcb_pool[i].next = &tcp_pcb_pool[i + 1];
    tcp_pcb_pool[TCP_MAX_PCB - 1].next = NULL;
    tcp_free_list = &tcp_pcb_pool[0];

    ip_register_proto(IP_PROTO_TCP, tcp_input);
}

struct tcp_pcb *tcp_new(void)
{
    struct tcp_pcb *pcb;

    if (tcp_free_list == NULL) return NULL;

    pcb = tcp_free_list;
    tcp_free_list = pcb->next;

    memset(pcb, 0, sizeof(struct tcp_pcb));
    pcb->state    = TCP_STATE_CLOSED;
    pcb->rcv_wnd  = TCP_DEFAULT_WINDOW;
    pcb->snd_wnd  = TCP_DEFAULT_WINDOW;
    pcb->mss      = TCP_DEFAULT_MSS;
    pcb->rto      = 1000;

    pcb->retransmit_timer = timer_create(pcb->rto, TIMER_TYPE_ONCE,
                                          tcp_retransmit_callback, pcb);

    pcb->next = tcp_active_list;
    tcp_active_list = pcb;

    return pcb;
}

int tcp_bind(struct tcp_pcb *pcb, uint32_t local_ip, uint16_t port)
{
    struct tcp_pcb *p;

    if (pcb == NULL) return -1;

    for (p = tcp_active_list; p != NULL; p = p->next) {
        if (p != pcb && p->local_port == port &&
            (p->local_ip == local_ip || p->local_ip == 0 || local_ip == 0)) {
            return -1;
        }
    }

    pcb->local_ip   = local_ip;
    pcb->local_port = port;
    pcb->flags |= TCP_PCB_FLAG_BOUND;
    pcb->state  = TCP_STATE_CLOSED;

    return 0;
}

void tcp_listen(struct tcp_pcb *pcb, uint8_t backlog)
{
    if (pcb == NULL) return;
    pcb->state = TCP_STATE_LISTEN;
    pcb->flags |= TCP_PCB_FLAG_LISTEN;
    (void)backlog;
}

int tcp_connect(struct tcp_pcb *pcb, uint32_t remote_ip, uint16_t remote_port)
{
    if (pcb == NULL) return -1;

    pcb->remote_ip   = remote_ip;
    pcb->remote_port = remote_port;
    pcb->snd_una     = tcp_gen_seq();
    pcb->snd_nxt     = pcb->snd_una + 1;
    pcb->rcv_nxt     = 0;
    pcb->state       = TCP_STATE_SYN_SENT;

    tcp_send_segment(pcb, TCP_FLAG_SYN, pcb->snd_una, 0, NULL);
    timer_start(pcb->retransmit_timer);

    return 0;
}

struct tcp_pcb *tcp_accept(struct tcp_pcb *listen_pcb)
{
    struct tcp_pcb *pcb;
    (void)listen_pcb;

    if (tcp_accept_queue == NULL) return NULL;

    pcb = tcp_accept_queue;
    tcp_accept_queue = pcb->next;
    pcb->next = NULL;

    return pcb;
}

int tcp_send(struct tcp_pcb *pcb, struct mbuf *m)
{
    if (pcb == NULL || m == NULL) return -1;
    if (pcb->state != TCP_STATE_ESTABLISHED &&
        pcb->state != TCP_STATE_CLOSE_WAIT) {
        mbuf_free(m);
        return -1;
    }

    /* 追加到 unsent 队列 */
    if (pcb->unsent_queue == NULL) {
        pcb->unsent_queue = m;
    } else {
        struct mbuf *q = pcb->unsent_queue;
        while (q->next_pkt != NULL) q = q->next_pkt;
        q->next_pkt = m;
    }
    m->next_pkt = NULL;

    /* 尝试发送 */
    return tcp_output(pcb);
}

void tcp_close(struct tcp_pcb *pcb)
{
    if (pcb == NULL) return;

    if (pcb->state == TCP_STATE_ESTABLISHED) {
        pcb->state = TCP_STATE_FIN_WAIT_1;
        tcp_send_segment(pcb, TCP_FLAG_FIN | TCP_FLAG_ACK,
                         pcb->snd_nxt, pcb->rcv_nxt, NULL);
        pcb->snd_nxt++;
    } else if (pcb->state == TCP_STATE_CLOSE_WAIT) {
        pcb->state = TCP_STATE_LAST_ACK;
        tcp_send_segment(pcb, TCP_FLAG_FIN | TCP_FLAG_ACK,
                         pcb->snd_nxt, pcb->rcv_nxt, NULL);
        pcb->snd_nxt++;
    }
}

void tcp_abort(struct tcp_pcb *pcb)
{
    if (pcb == NULL) return;
    tcp_send_segment(pcb, TCP_FLAG_RST | TCP_FLAG_ACK,
                     pcb->snd_nxt, 0, NULL);
    tcp_pcb_free(pcb);
}

void tcp_set_recv_callback(struct tcp_pcb *pcb, tcp_recv_fn cb)   { if (pcb) pcb->recv_callback = cb; }
void tcp_set_accept_callback(struct tcp_pcb *pcb, tcp_accept_fn cb){ if (pcb) pcb->accept_callback = cb; }
void tcp_set_sent_callback(struct tcp_pcb *pcb, tcp_sent_fn cb)    { if (pcb) pcb->sent_callback = cb; }
void tcp_set_err_callback(struct tcp_pcb *pcb, tcp_err_fn cb)      { if (pcb) pcb->err_callback = cb; }

int tcp_input(struct netif *ni, struct mbuf *m, uint32_t src_ip, uint32_t dst_ip)
{
    struct tcp_header *tcp;
    (void)dst_ip;
    struct tcp_pcb *pcb;
    uint16_t hdr_len, flags;
    uint32_t seq, ack;
    uint16_t src_port, dst_port;
    int i;

    if (ni == NULL || m == NULL) return -1;
    if (m->len < TCP_HEADER_LEN_MIN) {
        mbuf_free(m); return -1;
    }

    tcp = (struct tcp_header *)m->data;
    hdr_len  = (swap16(tcp->hdrlen_flags) >> 12) & 0xF;
    hdr_len *= 4;
    if (hdr_len < TCP_HEADER_LEN_MIN) { mbuf_free(m); return -1; }

    flags    = swap16(tcp->hdrlen_flags) & 0x3F;
    seq      = swap32(tcp->seq_num);
    ack      = swap32(tcp->ack_num);
    src_port = swap16(tcp->src_port);
    dst_port = swap16(tcp->dst_port);

    /* 校验和验证 */
    {
        uint16_t cksum;
        uint8_t verify_buf[2048];
        uint16_t verify_len = m->len;
        if (verify_len > sizeof(verify_buf)) {
            mbuf_free(m);
            return -1;
        }
        mbuf_copy_to(m, verify_buf, verify_len);
        cksum = tcp_checksum((struct tcp_header *)verify_buf, verify_len, src_ip, dst_ip);
        if (cksum != 0xFFFF && cksum != 0) {
            mbuf_free(m);
            return -1;
        }
    }

    /* 查找匹配的 PCB: 先精确匹配，再找 LISTEN */
    pcb = NULL;
    for (i = 0; i < TCP_MAX_PCB; i++) {
        struct tcp_pcb *p = &tcp_pcb_pool[i];
        if (p->state == TCP_STATE_CLOSED) continue;
        if (p->local_port == dst_port && p->remote_port == src_port &&
            p->remote_ip == src_ip) {
            pcb = p;
            break;
        }
    }
    if (pcb == NULL && (flags & TCP_FLAG_SYN)) {
        /* 查找 LISTEN PCB */
        struct tcp_pcb *listen_pcb = NULL;
        for (i = 0; i < TCP_MAX_PCB; i++) {
            struct tcp_pcb *p = &tcp_pcb_pool[i];
            if (p->state == TCP_STATE_LISTEN && p->local_port == dst_port) {
                listen_pcb = p;
                break;
            }
        }
        if (listen_pcb != NULL) {
            pcb = tcp_new_connection(listen_pcb, src_ip, src_port, seq);
            if (pcb != NULL) {
                tcp_send_segment(pcb, TCP_FLAG_SYN | TCP_FLAG_ACK,
                                 pcb->snd_una, pcb->rcv_nxt, NULL);
                timer_start(pcb->retransmit_timer);
                mbuf_free(m);
                return 0;
            }
        }
    }

    if (pcb == NULL) {
        /* 无匹配: 构造并发送 RST */
        if (!(flags & TCP_FLAG_RST)) {
            uint8_t rst_flags = TCP_FLAG_RST;
            uint32_t rst_seq = 0;
            uint32_t rst_ack = 0;

            if (flags & TCP_FLAG_ACK) {
                rst_flags |= TCP_FLAG_ACK;
                rst_seq = ack;
            } else {
                rst_seq = 0;
            }

            {
                uint8_t rst_buf[TCP_HEADER_LEN_MIN];
                struct tcp_header *rst_hdr = (struct tcp_header *)rst_buf;
                memset(rst_hdr, 0, TCP_HEADER_LEN_MIN);
                rst_hdr->src_port = tcp->dst_port;
                rst_hdr->dst_port = tcp->src_port;
                rst_hdr->seq_num  = swap32(rst_seq);
                rst_hdr->ack_num  = swap32(rst_ack);
                rst_hdr->hdrlen_flags = swap16((TCP_HEADER_LEN_MIN / 4) << 12 | rst_flags);
                rst_hdr->window   = 0;
                rst_hdr->checksum = 0;
                {
                    uint16_t cs = tcp_checksum(rst_hdr, TCP_HEADER_LEN_MIN, dst_ip, src_ip);
                    rst_hdr->checksum = swap16(cs);
                }

                struct mbuf *rst_m = mbuf_alloc();
                if (rst_m != NULL) {
                    mbuf_append(rst_m, TCP_HEADER_LEN_MIN);
                    mbuf_copy_from(rst_m, rst_buf, TCP_HEADER_LEN_MIN);
                    ip_output(src_ip, IP_PROTO_TCP, rst_m);
                }
            }
        }
        mbuf_free(m);
        return -1;
    }

    switch (pcb->state) {

    case TCP_STATE_SYN_SENT:
        if (flags & TCP_FLAG_RST) {
            if (pcb->err_callback) pcb->err_callback(pcb, -1);
            tcp_pcb_free(pcb);
        } else if ((flags & (TCP_FLAG_SYN | TCP_FLAG_ACK)) == (TCP_FLAG_SYN | TCP_FLAG_ACK) &&
            ack == pcb->snd_nxt) {
            pcb->rcv_nxt = seq + 1;
            pcb->snd_una = ack;
            pcb->state   = TCP_STATE_ESTABLISHED;
            timer_stop(pcb->retransmit_timer);

            tcp_send_segment(pcb, TCP_FLAG_ACK, pcb->snd_nxt, pcb->rcv_nxt, NULL);

            if (pcb->accept_callback) pcb->accept_callback(pcb);
            tcp_output(pcb); /* flush 排队数据 */
        }
        mbuf_free(m);
        break;

    case TCP_STATE_SYN_RECV:
        if (flags & TCP_FLAG_RST) {
            tcp_pcb_free(pcb);
        } else if (flags & TCP_FLAG_SYN) {
            /* 客户端重传 SYN，重发 SYN-ACK */
            tcp_send_segment(pcb, TCP_FLAG_SYN | TCP_FLAG_ACK,
                             pcb->snd_una, pcb->rcv_nxt, NULL);
            timer_start(pcb->retransmit_timer);
        } else if ((flags & TCP_FLAG_ACK) && ack == pcb->snd_nxt) {
            pcb->state = TCP_STATE_ESTABLISHED;
            timer_stop(pcb->retransmit_timer);

            /* 加入 accept 队列 */
            pcb->next = tcp_accept_queue;
            tcp_accept_queue = pcb;

            if (pcb->accept_callback) {
                pcb->accept_callback(pcb);
            }
            tcp_output(pcb); /* flush 排队数据 */
        }
        mbuf_free(m);
        break;

    case TCP_STATE_ESTABLISHED:
    case TCP_STATE_CLOSE_WAIT:
        if (flags & TCP_FLAG_RST) {
            tcp_pcb_free(pcb);
            mbuf_free(m);
            return 0;
        }

        if (flags & TCP_FLAG_FIN) {
            pcb->rcv_nxt = seq + 1;
            if (pcb->state == TCP_STATE_ESTABLISHED) {
                pcb->state = TCP_STATE_CLOSE_WAIT;
                tcp_send_segment(pcb, TCP_FLAG_ACK, pcb->snd_nxt, pcb->rcv_nxt, NULL);
            }
            /* 通知上层连接关闭 */
            if (pcb->recv_callback) pcb->recv_callback(pcb, NULL);
            mbuf_free(m);
            break;
        }

        /* 处理 ACK */
        if (flags & TCP_FLAG_ACK) {
            /* 选择性清理已确认的 unacked 数据 */
            struct mbuf *prev = NULL;
            struct mbuf *cur = pcb->unacked_queue;
            uint32_t packet_seq = pcb->snd_una;

            while (cur != NULL) {
                uint32_t end_seq = packet_seq + cur->total_len;
                if (end_seq <= ack) {
                    /* 此包已被完全确认 */
                    struct mbuf *next = cur->next_pkt;
                    if (prev == NULL)
                        pcb->unacked_queue = next;
                    else
                        prev->next_pkt = next;
                    packet_seq = end_seq;
                    mbuf_free(cur);
                    cur = next;
                } else {
                    /* 此包及后续包尚未被确认 */
                    break;
                }
            }

            pcb->snd_una = ack;

            if (pcb->unacked_queue == NULL) {
                timer_stop(pcb->retransmit_timer);
                pcb->retransmit_count = 0;
            }

            if (pcb->sent_callback) pcb->sent_callback(pcb, 0);
        }

        /* 有数据段 */
        {
            uint16_t data_len = m->total_len - hdr_len;
            if (data_len > 0) {
                /* 检查序号 */
                if (seq == pcb->rcv_nxt) {
                    mbuf_pull(m, hdr_len);
                    pcb->rcv_nxt += data_len;

                    /* 发 ACK */
                    tcp_send_segment(pcb, TCP_FLAG_ACK, pcb->snd_nxt, pcb->rcv_nxt, NULL);

                    /* 投递给上层 */
                    if (pcb->recv_callback != NULL) {
                        pcb->recv_callback(pcb, m);
                        return 0;
                    }
                }
            }
        }
        mbuf_free(m);
        break;

    case TCP_STATE_FIN_WAIT_1:
        if ((flags & TCP_FLAG_FIN) && (flags & TCP_FLAG_ACK)) {
            /* 同时关闭：对方确认了我们的 FIN 并发送了 FIN */
            pcb->rcv_nxt = seq + 1;
            tcp_send_segment(pcb, TCP_FLAG_ACK, pcb->snd_nxt, pcb->rcv_nxt, NULL);
            pcb->state = TCP_STATE_TIME_WAIT;
            if (pcb->timewait_timer == NULL)
                pcb->timewait_timer = timer_create(60000, TIMER_TYPE_ONCE,
                                                    tcp_timewait_callback, pcb);
            timer_start(pcb->timewait_timer);
        } else if (flags & TCP_FLAG_FIN) {
            /* 同时关闭：对方先发 FIN，但还没确认我们的 FIN */
            pcb->rcv_nxt = seq + 1;
            tcp_send_segment(pcb, TCP_FLAG_ACK, pcb->snd_nxt, pcb->rcv_nxt, NULL);
            pcb->state = TCP_STATE_CLOSING;
        } else if ((flags & TCP_FLAG_ACK) && ack == pcb->snd_nxt) {
            /* 对方确认了我们的 FIN → FIN_WAIT_2 */
            pcb->state = TCP_STATE_FIN_WAIT_2;
        }
        mbuf_free(m);
        break;

    case TCP_STATE_FIN_WAIT_2:
        if (flags & TCP_FLAG_FIN) {
            pcb->rcv_nxt = seq + 1;
            tcp_send_segment(pcb, TCP_FLAG_ACK, pcb->snd_nxt, pcb->rcv_nxt, NULL);
            pcb->state = TCP_STATE_TIME_WAIT;
            pcb->timewait_timer = timer_create(60000, TIMER_TYPE_ONCE,
                                                tcp_timewait_callback, pcb);
            timer_start(pcb->timewait_timer);
        } else {
            /* 对端可能还在发数据 */
            uint16_t data_len = m->total_len - hdr_len;
            if (data_len > 0 && seq == pcb->rcv_nxt) {
                mbuf_pull(m, hdr_len);
                pcb->rcv_nxt += data_len;
                tcp_send_segment(pcb, TCP_FLAG_ACK, pcb->snd_nxt, pcb->rcv_nxt, NULL);
                if (pcb->recv_callback != NULL) {
                    pcb->recv_callback(pcb, m);
                    return 0;
                }
            }
        }
        mbuf_free(m);
        break;

    case TCP_STATE_CLOSING:
        if ((flags & TCP_FLAG_ACK) && ack == pcb->snd_nxt) {
            /* 对方确认了我们的 FIN → TIME_WAIT */
            pcb->state = TCP_STATE_TIME_WAIT;
            if (pcb->timewait_timer == NULL)
                pcb->timewait_timer = timer_create(60000, TIMER_TYPE_ONCE,
                                                    tcp_timewait_callback, pcb);
            timer_start(pcb->timewait_timer);
        }
        mbuf_free(m);
        break;

    case TCP_STATE_LAST_ACK:
        if ((flags & TCP_FLAG_ACK) && ack == pcb->snd_nxt) {
            tcp_pcb_free(pcb);
        }
        mbuf_free(m);
        break;

    case TCP_STATE_TIME_WAIT:
        mbuf_free(m);
        break;

    default:
        mbuf_free(m);
        break;
    }

    return 0;
}

void tcp_slow_tick(void)
{
    int i;
    for (i = 0; i < TCP_MAX_PCB; i++) {
        struct tcp_pcb *pcb = &tcp_pcb_pool[i];
        if (pcb->state == TCP_STATE_CLOSED) continue;
        /* 重传检查由 timer 模块处理 */
    }
}

void tcp_fast_tick(void)
{
    /* 延迟 ACK 等 (简化: 不做) */
}

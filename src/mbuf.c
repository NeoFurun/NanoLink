/**
 * @file    mbuf.c
 * @brief   缓冲区管理模块实现
 *
 * mbuf 是整个协议栈的数据载体。一个数据包在协议栈各层之间传递时，
 * 每层只在数据前后添加/剥离自己的协议头，完全不拷贝数据本身。
 *
 * 核心概念：
 *   next      -- 串起同一个包的多个片段（纵向链），用于分片/零拷贝拼接
 *   next_pkt  -- 串起队列中的不同包（横向链），用于排队等待
 *   data      -- 始终指向"当前有效数据"的起始位置，prepend/pull 靠移动它实现零拷贝
 *   headroom  -- data 前面预留的空间，供各层逐步添加协议头
 *
 * 静态池：启动时从 mbuf_pool[256] 预分配所有 mbuf 结构体，
 * 运行时在 free_list 和各个队列之间流转，不调用 malloc/free。
 */

#include "../include/mbuf.h"
#include "../include/osal.h"
#include <stddef.h> // NULL

#define MBUF_POOL_SIZE 256 /**< 静态池中 mbuf 数量 */
#define MBUF_RESERVE 64    /**< 为协议头预留的头部空间（字节）
                                ETH(14) + IP(20) + TCP(20) = 54，取 64 留余量 */

/* ==========================================================================
   静态内存池与空闲链表
   ========================================================================== */

static struct mbuf mbuf_pool[MBUF_POOL_SIZE];
static struct mbuf *free_list = NULL; /**< 单向空闲链表 */

/* ==========================================================================
   mbuf_init — 初始化 mbuf 模块（协议栈启动时调用一次）
   ========================================================================== */
/**
 * 把 mbuf_pool 里 256 个结构体用 next 指针串成单向链表，free_list 指向链表头。
 *
 * 例（简化为 4 个）：
 *   调用前: mbuf_pool = [m0] [m1] [m2] [m3]  (杂乱状态)
 *   调用后: free_list → m0 → m1 → m2 → m3 → NULL
 */
void mbuf_init(void)
{
    int i;
    for (i = 0; i < MBUF_POOL_SIZE - 1; i++) {
        mbuf_pool[i].next = &mbuf_pool[i + 1];
    }
    mbuf_pool[MBUF_POOL_SIZE - 1].next = NULL;
    free_list = &mbuf_pool[0];
}

/* ==========================================================================
   mbuf_alloc — 从池中取一个空 mbuf（SMALL 类型）
   ========================================================================== */
/**
 * 例:
 *   struct mbuf *m = mbuf_alloc();
 *   // m->type  = MBUF_TYPE_SMALL
 *   // m->data  = storage.small_buf + 64   (64字节 headroom)
 *   // m->len   = 0
 *   // m->total_len = 0
 *   // m->next  = NULL
 *   // m->next_pkt = NULL
 *   // m->ref_count = 1
 *
 * 此时 buffer 布局:
 *   [---- headroom 64 ----][............ 可用 64 ............]
 *   ↑ small_buf            ↑ data                          ↑ small_buf+128
 */
struct mbuf *mbuf_alloc(void)
{
    struct mbuf *m;

    if (free_list == NULL) {
        return NULL; /* 池耗尽，256 个全部在外面 */
    }

    m = free_list;
    free_list = m->next;

    m->next      = NULL;
    m->next_pkt  = NULL;
    m->type      = MBUF_TYPE_SMALL;
    m->len       = 0;
    m->total_len = 0;
    m->ref_count = 1;
    m->data      = m->storage.small_buf + MBUF_RESERVE;

    return m;
}

/* ==========================================================================
   mbuf_free — 回收一个 mbuf（引用计数归零后归还池）
   ========================================================================== */
/**
 * 例（ARP 挂起队列场景）:
 *   发送路径创建了 m，ref_count=1。
 *   arp_query 把 m 挂进挂起队列，调 mbuf_ref_inc，ref_count=2。
 *   发送路径调 mbuf_free(m) → ref_count 减为 1，不回收。
 *   ARP 解析完成，取出发送后调 mbuf_free(m) → ref_count 减为 0，
 *     连带释放 next 链上的所有片段，归还到 free_list。
 */
void mbuf_free(struct mbuf *m)
{
    if (m == NULL) return;

    m->ref_count--;
    if (m->ref_count > 0) return;

    if (m->next != NULL) {
        mbuf_free(m->next);
        m->next = NULL;
    }

    if (m->type == MBUF_TYPE_LARGE && m->storage.large.large_buf != NULL) {
        osal_free(m->storage.large.large_buf);
        m->storage.large.large_buf = NULL;
    }

    m->next = free_list;
    free_list = m;
}

/* ==========================================================================
   mbuf_prepend — 在数据前面预留 len 字节（零拷贝，仅移动 data 指针）
   ========================================================================== */
/**
 * 例（IP 层发送前添加 IP 头）:
 *   当前 m 里装着 UDP 数据报 [ UDP头 | 100字节数据 ]，m->data 指向 UDP 头。
 *   IP 层需要在前面加 20 字节 IP 头:
 *     mbuf_prepend(m, 20);
 *   结果:
 *     [---- headroom ----][ IP头20 ][ UDP头 | 100字节数据 ]
 *     ↑ buf_start         ↑ data (前移了20)   ↑ 数据原封未动
 *   m->len 和 m->total_len 各加 20。
 */
int mbuf_prepend(struct mbuf *m, uint16_t len)
{
    uint8_t *buf_start;

    if (m == NULL) return -1;

    if (m->type == MBUF_TYPE_SMALL) {
        buf_start = m->storage.small_buf;
    } else {
        buf_start = (uint8_t *)m->storage.large.large_buf;
    }

    if (m->data - buf_start < len) return -1;

    m->data -= len;
    m->len += len;
    m->total_len += len;

    return 0;
}

/* ==========================================================================
   mbuf_append — 在数据尾部扩展 len 字节（零拷贝，仅增加 len 计数）
   ========================================================================== */
/**
 * 例（UDP 层构造数据报）:
 *   m 刚从 alloc 拿到，len=0。
 *   应用层有 200 字节数据要发:
 *     mbuf_append(m, 200);
 *   结果:
 *     [ headroom 64 ][.............. 200 字节可用 ..............]
 *     ↑ buf_start    ↑ data                                  ↑ data+200
 *   m->len = 200, m->total_len = 200。
 *   然后应用层把 200 字节写进 data[0..199]。
 */
int mbuf_append(struct mbuf *m, uint16_t len)
{
    uint8_t *buf_start;
    uint16_t capacity;
    uint16_t used;

    if (m == NULL) return -1;

    if (m->type == MBUF_TYPE_SMALL) {
        buf_start = m->storage.small_buf;
        capacity  = MBUF_SMALL_BUF_SIZE;
    } else {
        buf_start = (uint8_t *)m->storage.large.large_buf;
        capacity  = m->storage.large.large_size;
    }

    used = (uint16_t)(m->data - buf_start) + m->len;

    if (capacity - used < len) return -1;

    m->len += len;
    m->total_len += len;

    return 0;
}

/* ==========================================================================
   mbuf_pull — 从数据头部移除 len 字节（零拷贝，仅移动 data 指针）
   ========================================================================== */
/**
 * 例（接收路径逐层剥离协议头）:
 *   网卡收到一个帧，m->data 指向以太网帧头:
 *     [ ETH头14 ][ IP头20 ][ TCP头20 ][ 数据 ]
 *     ↑ data
 *   链路层处理完毕，不再需要 ETH 头:
 *     mbuf_pull(m, 14);
 *   结果:
 *     [ ETH头14 ][ IP头20 ][ TCP头20 ][ 数据 ]
 *                ↑ data (后移14)  m->len 减 14
 *   接下来 IP 层看到的直接就是 IP 头，数据原地不动。
 */
int mbuf_pull(struct mbuf *m, uint16_t len)
{
    if (m == NULL) return -1;

    if (m->len < len) return -1;

    m->data += len;
    m->len -= len;
    m->total_len -= len;

    return 0;
}

/* ==========================================================================
   mbuf_ref_inc — 增加引用计数
   ========================================================================== */
/**
 * 例:
 *   IP 层把 mbuf m 传给 ARP 模块挂起: arp_query 调 mbuf_ref_inc(m)。
 *   此时 m->ref_count 从 1 变 2——IP 层和 ARP 模块同时持有。
 */
void mbuf_ref_inc(struct mbuf *m)
{
    if (m != NULL) {
        m->ref_count++;
    }
}

/* ==========================================================================
   mbuf_chain_len — 遍历 next 链，返回所有片段的总长度
   ========================================================================== */
/**
 * 例:
 *   m: [ 80B ] ──next──→ [ 60B ] ──next──→ [ 30B ]
 *   mbuf_chain_len(m) → 170
 *
 * 注意: 通常直接读 m->total_len 即可，此函数仅在 total_len 不可信时用。
 */
uint16_t mbuf_chain_len(struct mbuf *m)
{
    uint16_t total = 0;
    while (m != NULL) {
        total += m->len;
        m = m->next;
    }
    return total;
}

/* ==========================================================================
   mbuf_chain — 把 m2 链拼接在 m1 链尾部
   ========================================================================== */
/**
 * 例（IP 分片重组）:
 *   收到了两个分片:
 *     m1: [ IP头 | 片段1(200B) ]  len=220, total_len=220
 *     m2: [ 片段2(150B) ]        len=150, total_len=150
 *   mbuf_chain(m1, m2):
 *     m1: [ IP头 | 片段1 ] ──next──→ [ 片段2 ]
 *          len=220, total_len=370    len=150
 *   之后 mbuf_copy_to(m1, buf, 370) 就能拿到完整数据。
 */
void mbuf_chain(struct mbuf *m1, struct mbuf *m2)
{
    struct mbuf *p;

    if (m1 == NULL || m2 == NULL) return;

    p = m1;
    while (p->next != NULL) {
        p = p->next;
    }

    p->next = m2;
    m1->total_len += m2->total_len;
}

/* ==========================================================================
   mbuf_split — 从链上切下前 len 字节，返回剩余部分的新链头
   ========================================================================== */
/**
 * 例（接收路径拆出 IP 负载交给上层）:
 *   m: [ IP头20B ] ──next──→ [ TCP头20B ] ──next──→ [ 数据100B ]
 *       total_len=140
 *   切掉 IP 头:
 *     struct mbuf *payload = mbuf_split(m, 20);
 *   m 变成: [ IP头20B ]  total_len=20
 *   payload: [ TCP头20B ] ──next──→ [ 数据100B ]  total_len=120
 *
 *   如果切割点恰好在片段边界 → 零拷贝，只断开 next 指针。
 *   如果切割点在片段内部 → 拷贝拆分该片段。
 */
struct mbuf *mbuf_split(struct mbuf *m, uint16_t len)
{
    struct mbuf *p, *prev;
    uint16_t walked;

    if (m == NULL || len == 0) return NULL;

    walked = 0;
    prev = NULL;
    p = m;
    while (p != NULL && walked + p->len < len) {
        walked += p->len;
        prev = p;
        p = p->next;
    }

    if (p == NULL) return NULL;

    if (walked + p->len == len) {
        struct mbuf *rest = p->next;
        p->next = NULL;
        m->total_len = len;
        if (rest != NULL) {
            rest->total_len = mbuf_chain_len(rest);
        }
        return rest;
    } else {
        uint16_t first_len = len - walked;
        struct mbuf *new_m = mbuf_alloc();
        if (new_m == NULL) return NULL;

        mbuf_copy_from(new_m, p->data + first_len, p->len - first_len);

        p->len = first_len;
        new_m->next = p->next;
        p->next = NULL;

        m->total_len = len;
        new_m->total_len = mbuf_chain_len(new_m);

        return new_m;
    }
}

/* ==========================================================================
   mbuf_copy_to — 把 mbuf 链的数据拷到连续内存 buf
   ========================================================================== */
/**
 * 例（应用层 socket_recv）:
 *   TCP 收到三个链在一起的片段:
 *     m: [ "Hello" ] ──next──→ [ ", Wor" ] ──next──→ [ "ld!" ]
 *   char buf[64];
 *   uint16_t n = mbuf_copy_to(m, buf, 64);
 *   // buf = "Hello, World!", n = 13
 *   应用层拿到连续字符串，完全不知道底层有 3 个片段。
 */
uint16_t mbuf_copy_to(struct mbuf *m, void *buf, uint16_t len)
{
    uint8_t *dst = (uint8_t *)buf;
    uint16_t copied = 0;

    while (m != NULL && copied < len) {
        uint16_t chunk = m->len;
        if (chunk > len - copied) {
            chunk = len - copied;
        }

        uint16_t i;
        for (i = 0; i < chunk; i++) {
            dst[copied + i] = m->data[i];
        }
        copied += chunk;
        m = m->next;
    }

    return copied;
}

/* ==========================================================================
   mbuf_copy_from — 把连续内存 buf 的数据拷到 mbuf 链
   ========================================================================== */
/**
 * 例（socket_send 把应用数据装进 mbuf）:
 *   应用层传来 char *data = "Hello"，len=5。
 *   协议栈有一个 m: [ headroom 64 ][ ...... 空 ...... ] (len=100 已 append)
 *   mbuf_copy_from(m, "Hello", 5):
 *     m->data[0..4] = 'H','e','l','l','o'
 *   如果 m 是条链，就逐片段填满，链空间不够则返回 -1。
 */
int mbuf_copy_from(struct mbuf *m, const void *buf, uint16_t len)
{
    const uint8_t *src = (const uint8_t *)buf;
    uint16_t written = 0;

    while (m != NULL && written < len) {
        uint16_t chunk = m->len;
        if (chunk > len - written) {
            chunk = len - written;
        }

        uint16_t i;
        for (i = 0; i < chunk; i++) {
            m->data[i] = src[written + i];
        }
        written += chunk;
        m = m->next;
    }

    if (written < len) return -1;

    return 0;
}

/* ==========================================================================
   mbuf_enqueue — 把一个包（链头）加到队列尾部
   ========================================================================== */
/**
 * 例（ARP 挂起队列）:
 *   ARP 表项挂起队列初始为空: q = { head=NULL, tail=NULL, count=0 }
 *   IP 层有个包 pkt1 要发给未解析 IP:
 *     mbuf_enqueue(&q, pkt1);
 *     结果: head→pkt1←tail, count=1
 *   又来一个 pkt2:
 *     mbuf_enqueue(&q, pkt2);
 *     结果: head→pkt1 ──next_pkt──→ pkt2 ←tail, count=2
 *     注意操作的是 next_pkt，与片段链 next 无关。
 */
void mbuf_enqueue(struct mbuf_list *q, struct mbuf *m)
{
    if (q == NULL || m == NULL) return;

    m->next_pkt = NULL;

    if (q->head == NULL) {
        q->head = m;
    } else {
        q->tail->next_pkt = m;
    }
    q->tail = m;
    q->count++;
}

/* ==========================================================================
   mbuf_dequeue — 从队列头部取出一个包
   ========================================================================== */
/**
 * 例（ARP 解析完成，发送挂起的包）:
 *   ARP 回复到达，遍历挂起队列逐一取出:
 *     struct mbuf *pkt = mbuf_dequeue(&pending_queue);
 *   pkt 被摘下，队列 head 后移，count 减 1。
 *   然后给 pkt 填上 MAC 地址，通过 netif->send 发送。
 */
struct mbuf *mbuf_dequeue(struct mbuf_list *q)
{
    struct mbuf *m;

    if (q == NULL || q->head == NULL) return NULL;

    m = q->head;
    q->head = m->next_pkt;
    m->next_pkt = NULL;

    if (q->head == NULL) {
        q->tail = NULL;
    }
    q->count--;

    return m;
}

/* ==========================================================================
   mbuf_remove — 从队列中移除一个指定包（非必须头部）
   ========================================================================== */
/**
 * 例（TCP 重传队列收到累积 ACK 后释放已确认的包）:
 *   unacked_queue: head→pkt1─→pkt2─→pkt3←tail
 *   对方 ACK 确认了 pkt1 和 pkt2 的数据，需要从队列移除并释放:
 *     mbuf_remove(&unacked_queue, pkt1);  mbuf_free(pkt1);
 *     mbuf_remove(&unacked_queue, pkt2);  mbuf_free(pkt2);
 *   队列变为: head→pkt3←tail
 *
 *   注意: 与 dequeue 不同，remove 可以摘任意位置的包，
 *   但需要 O(n) 遍历找到前驱。只用于 TCP 乱序/累积确认等异常路径。
 */
void mbuf_remove(struct mbuf_list *q, struct mbuf *m)
{
    struct mbuf *p, *prev;

    if (q == NULL || m == NULL || q->head == NULL) return;

    prev = NULL;
    p = q->head;
    while (p != NULL && p != m) {
        prev = p;
        p = p->next_pkt;
    }

    if (p == NULL) return;

    if (prev == NULL) {
        q->head = m->next_pkt;
    } else {
        prev->next_pkt = m->next_pkt;
    }

    if (q->tail == m) {
        q->tail = prev;
    }

    m->next_pkt = NULL;
    q->count--;
}

/* ==========================================================================
   mbuf_alloc_large — 分配带外部大缓冲的 mbuf，用于超长帧
   ========================================================================== */
/**
 * 例（接收一个 1500 字节的以太网帧）:
 *   SMALL 只有 128 字节装不下，驱动层用 large:
 *     struct mbuf *m = mbuf_alloc_large(2048);
 *   m->type = MBUF_TYPE_LARGE
 *   m->data = m->storage.large.large_buf  (指向外部 2048 字节缓冲)
 *   m->storage.large.large_size = 2048
 *
 *   注意: LARGE 的头部空间按需操作，不预留 headroom。
 */
struct mbuf *mbuf_alloc_large(uint16_t size)
{
    struct mbuf *m;

    if (free_list == NULL) return NULL;
    m = free_list;
    free_list = m->next;

    m->storage.large.large_buf = osal_malloc(size);
    if (m->storage.large.large_buf == NULL) {
        m->next = free_list;
        free_list = m;
        return NULL;
    }

    m->next = NULL;
    m->next_pkt = NULL;
    m->type = MBUF_TYPE_LARGE;
    m->len = 0;
    m->total_len = 0;
    m->ref_count = 1;
    m->storage.large.large_size = size;
    m->data = m->storage.large.large_buf;

    return m;
}

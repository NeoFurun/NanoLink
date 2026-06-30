#include "../include/mbuf.h"
#include "../include/osal.h"
#include <stddef.h> // NULL

#define MBUF_POOL_SIZE 256 //静态池中mbuf的数量
#define MBUF_RESERVE 64    //协议头预留空间（headroom），用于在数据前面添加协议头


static struct mbuf mbuf_pool[MBUF_POOL_SIZE];//静态 mbuf 池
static struct mbuf *free_list = NULL; //空闲链表

// 初始化 mbuf 池，建立空闲链表
void mbuf_init(void)
{
    int i;
    for (i = 0; i < MBUF_POOL_SIZE - 1; i++) {
        mbuf_pool[i].next = &mbuf_pool[i + 1];
    }
    mbuf_pool[MBUF_POOL_SIZE - 1].next = NULL;
    free_list = &mbuf_pool[0];
}

//申请小缓冲区 mbuf
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

// 释放 mbuf，引用计数减1，归零时回收缓冲
void mbuf_free(struct mbuf *m)
{
    struct mbuf *cur, *next_node;

    if (m == NULL) return;

    m->ref_count--;
    if (m->ref_count > 0) return;

    cur = m;
    while (cur != NULL) {
        next_node = cur->next;

        if (cur->type == MBUF_TYPE_LARGE && cur->storage.large.large_buf != NULL) {
            osal_free(cur->storage.large.large_buf);
            cur->storage.large.large_buf = NULL;
        }

        cur->next = free_list;
        free_list = cur;

        cur = next_node;
    }
}

//在头部增加空间，用来加协议头
int mbuf_prepend(struct mbuf *m, uint16_t len)
{
    uint8_t *buf_start;

    if (m == NULL) return -1;

    if (m->type == MBUF_TYPE_SMALL) {
        buf_start = m->storage.small_buf;
    } else {
        buf_start = (uint8_t *)m->storage.large.large_buf;
    }

    if (m->data - buf_start < len) return -1;//没有足够的空间来增加头部

    m->data -= len;
    m->len += len;
    m->total_len += len;

    return 0;
}

//在尾部增加空间，用来装数据
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

    if (used > capacity) return -1;
    if (capacity - used < len) return -1;

    m->len += len;
    m->total_len += len;

    return 0;
}

//从头部移除数据，用来剥协议头
int mbuf_pull(struct mbuf *m, uint16_t len)
{
    if (m == NULL) return -1;

    if (m->len < len) return -1;

    m->data += len;
    m->len -= len;
    m->total_len -= len;

    return 0;
}

// 增加 mbuf 的引用计数
void mbuf_ref_inc(struct mbuf *m)
{
    if (m != NULL) {
        m->ref_count++;
    }
}

// 计算整个 mbuf 链的总长度
uint16_t mbuf_chain_len(struct mbuf *m)
{
    uint16_t total = 0;
    while (m != NULL) {
        total += m->len;
        m = m->next;
    }
    return total;
}

// 将两个 mbuf 链连接起来，更新总长度
void mbuf_chain(struct mbuf *m1, struct mbuf *m2)
{
    struct mbuf *p;

    if (m1 == NULL || m2 == NULL) return;
    if (m1 == m2) return; 

    p = m1;
    while (p->next != NULL) {
        p = p->next;
    }

    p->next = m2;
    m1->total_len += m2->total_len;
}

// 从 mbuf 链中拆分出前 len 字节，返回新链
struct mbuf *mbuf_split(struct mbuf *m, uint16_t len)
{
    struct mbuf *p;
    uint16_t walked;

    if (m == NULL || len == 0) return NULL;

    walked = 0;
    p = m;
    while (p != NULL && walked + p->len < len) {
        walked += p->len;
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
        uint16_t tail_len  = p->len - first_len;
        struct mbuf *new_m;

        if (tail_len > MBUF_SMALL_BUF_SIZE - MBUF_RESERVE) {
            new_m = mbuf_alloc_large(tail_len);
        } else {
            new_m = mbuf_alloc();
        }
        if (new_m == NULL) return NULL;

        mbuf_append(new_m, tail_len);
        mbuf_copy_from(new_m, p->data + first_len, tail_len);

        p->len = first_len;
        new_m->next = p->next;
        p->next = NULL;

        m->total_len = len;
        new_m->total_len = mbuf_chain_len(new_m);

        return new_m;
    }
}

// 将 mbuf 链中的数据拷贝到连续内存 buf
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

// 将连续内存 buf 中的数据拷贝到 mbuf 链中
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

//入队
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

//出队
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

// 从队列中移除一个指定的包
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

// 分配一个带外部大缓冲区的 mbuf
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

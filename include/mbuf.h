/**
 * @file    mbuf.h
 * @brief   缓冲区管理模块公开接口
 *
 * 这是整个协议栈唯一被所有模块依赖的头文件，接口需保持稳定。
 * 支持零拷贝的 mbuf 链表操作、引用计数以及内外置缓冲。
 */

#ifndef MBUF_H
#define MBUF_H

#include <stdint.h>
#include <stddef.h>

/* -------------------------------------------------------------------------- */
/*  预定义常量                                                                */
/* -------------------------------------------------------------------------- */

#define MBUF_SMALL_BUF_SIZE 128 /**< 内嵌小缓冲区的默认大小 */

/* -------------------------------------------------------------------------- */
/*  类型定义                                                                  */
/* -------------------------------------------------------------------------- */

/** mbuf 缓冲区类型 */
typedef enum
{
    MBUF_TYPE_SMALL, /**< 使用内嵌 small_buf */
    MBUF_TYPE_LARGE  /**< 使用外部大缓冲区 */
} mbuf_type_t;

/* 前置声明 */
struct mbuf;

/** mbuf 队列 / 链表管理结构 */
struct mbuf_list
{
    struct mbuf *head; /**< 队列头 */
    struct mbuf *tail; /**< 队列尾 */
    uint16_t count;    /**< 队列中的包数量 */
};

/** mbuf 核心结构体 */
struct mbuf
{
    struct mbuf *next;     /**< 同一数据包的下一个片段 */
    struct mbuf *next_pkt; /**< 包队列中的下一个包 */

    uint8_t *data;      /**< 当前有效数据的起始指针 */
    uint16_t len;       /**< 当前片段的长度 */
    uint16_t total_len; /**< 整个 mbuf 链的总长度（仅首节点有效） */

    mbuf_type_t type;   /**< 缓冲类型 :指定storage用小缓冲还是大缓冲*/
    uint16_t ref_count; /**< 引用计数 */

    union
    {
        uint8_t small_buf[MBUF_SMALL_BUF_SIZE]; /**< 内嵌小缓冲 */
        struct
        {
            void *large_buf;     /**< 指向外部大缓冲 */
            uint16_t large_size; /**< 外部大缓冲的大小 */
        } large;
    } storage;
};

//初始化
void mbuf_init(void);

/* -------------------------------------------------------------------------- */
/*  分配与释放                                                                */
/* -------------------------------------------------------------------------- */

/**
 * @brief 分配一个 mbuf，让mbuf->data使用small_buf。
 * @return 分配成功返回 mbuf 指针，失败返回 NULL。
 */
struct mbuf *mbuf_alloc(void);

/**
 * @brief 分配一个带外部大缓冲区的 mbuf。让mbuf->data 使用large_buf
 * @param size 外部缓冲区大小（字节）。
 * @return 成功返回 mbuf 指针，失败返回 NULL。
 */
struct mbuf *mbuf_alloc_large(uint16_t size);

/**
 * @brief 释放 mbuf（引用计数减1，归零时回收缓冲）,释放data
 * @param m mbuf 指针。
 */
void mbuf_free(struct mbuf *m);

/**
 * @brief 增加 mbuf 的引用计数。
 * @param m mbuf 指针。
 */
void mbuf_ref_inc(struct mbuf *m);

/* -------------------------------------------------------------------------- */
/*  零拷贝数据操作                                                            */
/* -------------------------------------------------------------------------- */

/**
 * @brief 在当前数据前预留空间（用于添加协议头），data 指针前移。
 * [ 空闲20      ] [          UDP数据 100字节          ]
   ↑ data (新位置) ↑ data (旧位置)
 * @param m   mbuf 指针（必须为首节点或独立包）。
 * @param len 需要预留的字节数。
 * @return 0 成功，-1 剩余空间不足。
 */
int mbuf_prepend(struct mbuf *m, uint16_t len);

/**
 * @brief 从数据头部移除 len 字节（跳过协议头），data 后移，len 减小。
 * [ ETH头 14字节 ] [ IP头 20字节 ] [ TCP数据 ]
                   ↑ data (新位置)
 * @param m   mbuf 指针。
 * @param len 需要移除的字节数。
 * @return 0 成功，-1 长度不足。
 */
int mbuf_pull(struct mbuf *m, uint16_t len);

/**
 * @brief 在数据尾部追加 len 字节可用空间，返回后可使用 data+原len 写入。
 * [ 预留 8 ] [ .............. 100字节可用空间 .............. ]
 ↑ data                                                    ↑ data+len (新)

 * @param m   mbuf 指针。
 * @param len 追加的字节数。
 * @return 0 成功，-1 剩余空间不足。
 */
int mbuf_append(struct mbuf *m, uint16_t len);

/* -------------------------------------------------------------------------- */
/*  链表操作（多片段拼接 / 拆分，零拷贝）                                    */
/* -------------------------------------------------------------------------- */

/**
 * @brief 将 mbuf 链 m2 拼接到 m1 之后，组成一个逻辑包。
 * m1: [ IP头 | 片段1 ] ──next──→ m2: [ 片段2 ]
    len=520                            len=300
    total_len=820                      total_len=不管

 * @param m1 首条链。
 * @param m2 被拼接的链。
 */
void mbuf_chain(struct mbuf *m1, struct mbuf *m2);

/**
 * @brief 从 mbuf 链中拆下前 len 字节，生成一条新链。
 * 返回的 m_new: [ 前20字节 ]              len=20, total_len=20, next=NULL
               (原来的 m 还在，但 data 前移了，还是指向片段1剩余部分)

   原来的 m:    [ 片段1剩余500B ] ──next──→ [ 片段2 (300B) ]
               len=500, total_len=600
 * @param m   原链头节点。
 * @param len 拆分的字节数。
 * @return 包含前 len 字节的新链头节点，原链剩余部分（可能为 NULL）。
 */
struct mbuf *mbuf_split(struct mbuf *m, uint16_t len);

/**
 * @brief 返回整个 mbuf 链的总长度。
 * m: [ 200字节 ] ──next──→ [ 150字节 ] ──next──→ [ 80字节 ]
       len=200                 len=150                 len=80
       total_len=430

 * @param m 链的头节点。
 * @return 总长度（字节）。
 */
uint16_t mbuf_chain_len(struct mbuf *m);

/* -------------------------------------------------------------------------- */
/*  数据拷贝（仅在必须连续内存时使用）                                        */
/* -------------------------------------------------------------------------- */

/**
 * @brief 将 mbuf 链中的数据拷贝到连续内存 buf。
 * buf:  [ 80字节 | 60字节 | 30字节 ]  ← 连续内存
         ↑ 来自片段1  ↑ 来自片段2  ↑ 来自片段3

 * @param m   源 mbuf 链头。
 * @param buf 目标内存指针。
 * @param len 最多拷贝的字节数。
 * @return 实际拷贝的字节数。
 */
uint16_t mbuf_copy_to(struct mbuf *m, void *buf, uint16_t len);

/**
 * @brief 将连续内存 buf 中的数据拷贝到 mbuf 链。
 * @param m   目标 mbuf 链头。
 * @param buf 源内存指针。
 * @param len 拷贝字节数。
 * @return 0 成功，-1 空间不足。
 */
int mbuf_copy_from(struct mbuf *m, const void *buf, uint16_t len);

/* -------------------------------------------------------------------------- */
/*  队列管理                                                                  */
/* -------------------------------------------------------------------------- */

/**
 * @brief 将一个包加入队列末尾。
 * @param q 目标队列。
 * @param m 要入队的包（链头）。
 */
void mbuf_enqueue(struct mbuf_list *q, struct mbuf *m);

/**
 * @brief 从队列头部取出一个包。
 * @param q 队列。
 * @return 取出的包（链头），若队列为空则返回 NULL。
 */
struct mbuf *mbuf_dequeue(struct mbuf_list *q);

/**
 * @brief 从队列中移除一个指定的包。
 * @param q 队列。
 * @param m 要移除的包（链头，必须在队列中）。
 */
void mbuf_remove(struct mbuf_list *q, struct mbuf *m);

#endif /* MBUF_H */
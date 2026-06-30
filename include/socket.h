#ifndef SOCKET_H
#define SOCKET_H

#include <stdint.h>
#include "mbuf.h"

/* 地址族 */
#define AF_INET 2 /**< IPv4 地址族 */

/* Socket 类型 */
#define SOCK_STREAM 1 /**< TCP 流式套接字 */
#define SOCK_DGRAM 2  /**< UDP 数据报套接字 */

/* IPPROTO 等价定义（便于应用层使用） */
#define IPPROTO_TCP 6
#define IPPROTO_UDP 17

/* 最大 socket 数量 */
#define SOCKET_MAX 16

/* shutdown 类型 */
#define SHUT_RD 0   /**< 关闭读 */
#define SHUT_WR 1   /**< 关闭写 */
#define SHUT_RDWR 2 /**< 关闭读写 */


/** IPv4 地址结构（兼容 BSD sockaddr_in） */
struct sockaddr_in
{
    uint16_t sin_family; /**< 地址族（AF_INET） */
    uint16_t sin_port;   /**< 端口号（网络字节序） */
    uint32_t sin_addr;   /**< IPv4 地址（网络字节序） */
    uint8_t sin_zero[8]; /**< 填充 */
};

/** 通用 socket 地址（用于参数传递） */
struct sockaddr
{
    uint16_t sa_family;  /**< 地址族 */
    uint8_t sa_data[14]; /**< 地址数据 */
};

/* 前置声明 */
struct socket;

/** 传输协议操作表：每个协议实现一组函数指针，socket 层通过它们操作 */
struct transport_ops
{
    /** 创建新的协议控制块 */
    void *(*new_pcb)(void);

    /** 绑定到本地地址 */
    int (*bind)(void *pcb, uint32_t local_ip, uint16_t port);

    /** 连接远端（UDP 可只设置默认目标，TCP 发起握手） */
    int (*connect)(void *pcb, uint32_t remote_ip, uint16_t remote_port);

    /** 开始监听（仅 TCP 支持，UDP 返回错误） */
    int (*listen)(void *pcb, uint8_t backlog);

    /** 接受新连接（仅 TCP 支持） */
    void *(*accept)(void *listen_pcb);

    /** 发送数据 */
    int (*send)(void *pcb, struct mbuf *m, uint32_t dst_ip, uint16_t dst_port);

    /** 关闭连接 */
    void (*close)(void *pcb);

    /** 释放控制块 */
    void (*remove)(void *pcb);

    /** 设置接收回调 */
    void (*set_recv_callback)(void *pcb, void (*cb)(void *, struct mbuf *));

    /** 设置 accept 回调（仅 TCP） */
    void (*set_accept_callback)(void *pcb, void (*cb)(void *));

    /** 设置发送完成回调 */
    void (*set_sent_callback)(void *pcb, void (*cb)(void *, uint16_t));

    /** 设置错误回调 */
    void (*set_err_callback)(void *pcb, void (*cb)(void *, int));
};

/** Socket 结构体（对外不透明，句柄） */
struct socket
{
    uint8_t type;   /**< SOCK_STREAM / SOCK_DGRAM */
    uint8_t state;  /**< socket 状态 */
    uint16_t flags; /**< 标志位 */

    void *pcb;                       /**< 指向 TCP_PCB 或 UDP_PCB */
    const struct transport_ops *ops; /**< 协议操作表 */

    /* 接收缓冲区（应用层未读取的数据） */
    struct mbuf *recv_queue;
    uint32_t recv_src_ip;   /**< 最近收到的源 IP */
    uint16_t recv_src_port; /**< 最近收到的源端口 */

    /* 等待 accept 的连接队列（仅 listen socket） */
    struct socket *accept_queue_head;
    struct socket *accept_queue_tail;
    uint8_t accept_queue_len;
    uint8_t backlog;

    void *priv; /**< 应用层私有数据 */

    struct socket *next; /**< 全局链表指针 */
};

/* Socket 状态 */
#define SOCK_STATE_FREE 0       /**< 未分配 */
#define SOCK_STATE_ALLOC 1      /**< 已分配（未绑定） */
#define SOCK_STATE_BOUND 2      /**< 已绑定 */
#define SOCK_STATE_LISTEN 3     /**< 正在监听 */
#define SOCK_STATE_CONNECTING 4 /**< 正在连接 */
#define SOCK_STATE_CONNECTED 5  /**< 已连接 */
#define SOCK_STATE_CLOSED 6     /**< 已关闭 */

//创建一个socket
struct socket *socket_create(int type);

//关闭socket
void socket_close(struct socket *sock);

//绑定到本地地址和端口。
int socket_bind(struct socket *sock, const struct sockaddr_in *addr);

/**
 * @brief 连接远端地址（TCP 发起握手，UDP 设置默认目标）。
 * @param sock    socket 指针。
 * @param addr    远端地址结构体指针。
 * @return 0 成功，-1 失败。
 */
int socket_connect(struct socket *sock, const struct sockaddr_in *addr);

//监听
int socket_listen(struct socket *sock, uint8_t backlog);

//接受连接（仅 TCP 支持）。
struct socket *socket_accept(struct socket *listen_sock);

//发送数据
int socket_send(struct socket *sock, const void *data, uint16_t len,
                const struct sockaddr_in *addr);

//接受数据
int socket_recv(struct socket *sock, void *buf, uint16_t len,
                struct sockaddr_in *addr);


//关闭 socket 的读端/写端/全部
void socket_shutdown(struct socket *sock, int how);

//初始化socket
void socket_init(void);

#endif /* SOCKET_H */
/**
 * @file    udp.h
 * @brief   UDP 用户数据报协议模块公开接口
 *
 * 提供基于端口的数据报收发，通过 IP 层注册接收，
 * 通过控制块向上层投递数据。
 */

#ifndef UDP_H
#define UDP_H

#include <stdint.h>
#include "mbuf.h"
#include "ip.h"

/* -------------------------------------------------------------------------- */
/*  预定义常量                                                                */
/* -------------------------------------------------------------------------- */

#define UDP_HEADER_LEN 8    /**< UDP 头部长度（固定） */
#define UDP_MAX_PORTS 65535 /**< 最大端口号 */
#define UDP_MAX_PCB 16      /**< 最大 UDP 控制块数量 */

/* -------------------------------------------------------------------------- */
/*  类型定义                                                                  */
/* -------------------------------------------------------------------------- */

/** UDP 头部结构体 */
struct udp_header
{
    uint16_t src_port; /**< 源端口 */
    uint16_t dst_port; /**< 目标端口 */
    uint16_t length;   /**< 长度（头部 + 数据） */
    uint16_t checksum; /**< 校验和（可选，填0表示不校验） */
} __attribute__((packed));

/* 前置声明 */
struct udp_pcb;

/** UDP 数据接收回调函数类型 */
typedef void (*udp_recv_fn)(struct udp_pcb *pcb, struct mbuf *m,
                            uint32_t src_ip, uint16_t src_port);

/** UDP 控制块 */
struct udp_pcb
{
    uint32_t local_ip;         /**< 本地 IP（网络字节序） */
    uint16_t local_port;       /**< 本地端口（主机字节序） */
    uint32_t remote_ip;        /**< 远端 IP（网络字节序，0 表示不限制） */
    uint16_t remote_port;      /**< 远端端口（主机字节序，0 表示不限制） */
    uint8_t flags;             /**< 状态标志 */
    udp_recv_fn recv_callback; /**< 数据接收回调函数 */
    void *priv;                /**< 上层私有数据（如指向 socket 结构体） */
    struct udp_pcb *next;      /**< 链表指针（内部使用） */
};

/* UDP 控制块标志 */
#define UDP_FLAG_BOUND 0x01 /**< 已绑定本地端口 */

/* -------------------------------------------------------------------------- */
/*  公开函数接口                                                              */
/* -------------------------------------------------------------------------- */

/**
 * @brief 初始化 UDP 模块，注册到 IP 层。
 */
void udp_init(void);

/**
 * @brief 创建一个 UDP 控制块。
 * @return 控制块指针，失败返回 NULL。
 */
struct udp_pcb *udp_new(void);

/**
 * @brief 绑定控制块到本地 IP 和端口。
 * @param pcb       控制块。
 * @param local_ip  本地 IP 地址，填 0 表示绑定所有接口。
 * @param port      本地端口号（主机字节序）。
 * @return 0 成功，-1 端口已被占用。
 */
int udp_bind(struct udp_pcb *pcb, uint32_t local_ip, uint16_t port);

/**
 * @brief 设置远端 IP 和端口（connect 风格，只接收该远端的数据）。
 * @param pcb       控制块。
 * @param remote_ip   远端 IP 地址（网络字节序）。
 * @param remote_port 远端端口（主机字节序）。
 */
void udp_connect(struct udp_pcb *pcb, uint32_t remote_ip, uint16_t remote_port);

/**
 * @brief 设置接收回调函数。
 * @param pcb      控制块。
 * @param callback 接收回调函数。
 */
void udp_set_recv_callback(struct udp_pcb *pcb, udp_recv_fn callback);

/**
 * @brief 发送 UDP 数据报。
 * @param pcb        控制块（需已设置 remote 或传入目标地址）。
 * @param m          包含应用数据的 mbuf（UDP 头部由本函数添加）。
 * @param dst_ip     目标 IP（网络字节序），若 pcb 已 connect 可填 0 使用默认。
 * @param dst_port   目标端口（主机字节序），若 pcb 已 connect 可填 0 使用默认。
 * @return 0 成功，-1 失败。
 */
int udp_send(struct udp_pcb *pcb, struct mbuf *m, uint32_t dst_ip, uint16_t dst_port);

/**
 * @brief 释放 UDP 控制块。
 * @param pcb 控制块。
 */
void udp_remove(struct udp_pcb *pcb);

/**
 * @brief UDP 输入处理函数（注册到 IP 层的 IP_PROTO_UDP）。
 * @param ni       收到包的接口。
 * @param m        包含 UDP 报文的 mbuf（IP 头已移除）。
 * @param src_ip   源 IPv4 地址。
 * @param dst_ip   目标 IPv4 地址。
 * @return 0 成功，-1 无匹配端口。
 */
int udp_input(struct netif *ni, struct mbuf *m, uint32_t src_ip, uint32_t dst_ip);

#endif /* UDP_H */
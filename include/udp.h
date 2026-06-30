#ifndef UDP_H
#define UDP_H

#include <stdint.h>
#include "mbuf.h"
#include "ip.h"


#define UDP_HEADER_LEN 8    /**< UDP 头部长度（固定） */
#define UDP_MAX_PORTS 65535 /**< 最大端口号 */
#define UDP_MAX_PCB 16      /**< 最大 UDP 控制块数量 */


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

//初始化UDP协议栈
void udp_init(void);

//创建一个UDP控制块
struct udp_pcb *udp_new(void);

//绑定控制块到本地 IP 和端口
int udp_bind(struct udp_pcb *pcb, uint32_t local_ip, uint16_t port);

//设置远端 IP 和端口（connect 风格，只接收该远端的数据）
void udp_connect(struct udp_pcb *pcb, uint32_t remote_ip, uint16_t remote_port);

//设置接收回调函数
void udp_set_recv_callback(struct udp_pcb *pcb, udp_recv_fn callback);

//发送 UDP 数据包
int udp_send(struct udp_pcb *pcb, struct mbuf *m, uint32_t dst_ip, uint16_t dst_port);

//从链表中移除并释放 UDP 控制块
void udp_remove(struct udp_pcb *pcb);

//处理接收到的 UDP 数据包
int udp_input(struct netif *ni, struct mbuf *m, uint32_t src_ip, uint32_t dst_ip);

#endif /* UDP_H */
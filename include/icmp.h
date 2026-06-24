/**
 * @file    icmp.h
 * @brief   ICMP 控制消息协议模块公开接口
 *
 * 提供 Ping 回复、差错报告等功能。
 * 通过 IP 层注册接收，发送时调用 ip_output。
 */

#ifndef ICMP_H
#define ICMP_H

#include <stdint.h>
#include "mbuf.h"
#include "ip.h"

/* -------------------------------------------------------------------------- */
/*  预定义常量                                                                */
/* -------------------------------------------------------------------------- */

/* ICMP 消息类型 */
#define ICMP_TYPE_ECHO_REPLY 0       /**< Echo 应答 */
#define ICMP_TYPE_DEST_UNREACHABLE 3 /**< 目标不可达 */
#define ICMP_TYPE_ECHO_REQUEST 8     /**< Echo 请求 */

/* 目标不可达代码 */
#define ICMP_CODE_NET_UNREACHABLE 0   /**< 网络不可达 */
#define ICMP_CODE_HOST_UNREACHABLE 1  /**< 主机不可达 */
#define ICMP_CODE_PROTO_UNREACHABLE 2 /**< 协议不可达 */
#define ICMP_CODE_PORT_UNREACHABLE 3  /**< 端口不可达 */
#define ICMP_CODE_FRAG_NEEDED 4       /**< 需要分片但设置了 DF */

/* ICMP 头部长度（类型+代码+校验和） */
#define ICMP_HEADER_LEN 4

/* -------------------------------------------------------------------------- */
/*  类型定义                                                                  */
/* -------------------------------------------------------------------------- */

/** ICMP 头部结构体 */
struct icmp_header
{
    uint8_t type;      /**< 消息类型 */
    uint8_t code;      /**< 代码 */
    uint16_t checksum; /**< 校验和 */
    union
    {
        struct
        {
            uint16_t id;  /**< 标识符（Echo 用） */
            uint16_t seq; /**< 序号（Echo 用） */
        } echo;
        uint32_t rest; /**< 其他消息的剩余字段 */
    } data;
} __attribute__((packed));

/* -------------------------------------------------------------------------- */
/*  公开函数接口                                                              */
/* -------------------------------------------------------------------------- */

/**
 * @brief 初始化 ICMP 模块，注册到 IP 层。
 */
void icmp_init(void);

/**
 * @brief ICMP 输入处理函数（注册到 IP 层的 IP_PROTO_ICMP）。
 * @param ni       收到包的接口。
 * @param m        包含 ICMP 消息的 mbuf（IP 头已移除）。
 * @param src_ip   源 IPv4 地址。
 * @param dst_ip   目标 IPv4 地址（本机）。
 * @return 0 成功，-1 错误。
 */
int icmp_input(struct netif *ni, struct mbuf *m, uint32_t src_ip, uint32_t dst_ip);

/**
 * @brief 发送一个 ICMP 差错报文（目标不可达等）。
 * @param type      ICMP 类型。
 * @param code      代码。
 * @param info      附加信息（如 MTU for frag needed），视类型而定。
 * @param original  触发差错的原始 IP 包（mbuf，至少包含 IP 头+8字节传输层头）。
 * @return 0 成功，-1 失败。
 */
int icmp_send_error(uint8_t type, uint8_t code, uint32_t info, struct mbuf *original);

#endif /* ICMP_H */
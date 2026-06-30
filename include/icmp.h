#ifndef ICMP_H
#define ICMP_H

#include <stdint.h>
#include "mbuf.h"
#include "ip.h"

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


//初始化ICMP模块
void icmp_init(void);

//处理接收到的ICMP报文
int icmp_input(struct netif *ni, struct mbuf *m, uint32_t src_ip, uint32_t dst_ip);

//发送ICMP错误报文
int icmp_send_error(uint8_t type, uint8_t code, uint32_t info, struct mbuf *original);

#endif
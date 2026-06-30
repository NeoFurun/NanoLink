#ifndef IP_H
#define IP_H

#include <stdint.h>
#include "mbuf.h"
#include "netif.h"

/* IPv4 协议号（常见） */
#define IP_PROTO_ICMP 1 /**< ICMP */
#define IP_PROTO_TCP 6  /**< TCP */
#define IP_PROTO_UDP 17 /**< UDP */

#define IP_VERSION_4 4           /**< IPv4 版本号 */
#define IP_HEADER_MIN_LEN 20     /**< 最小 IP 头长度（无选项） */
#define IP_HEADER_MAX_LEN 60     /**< 最大 IP 头长度（含选项） */
#define IP_MAX_PACKET_SIZE 65535 /**< 最大 IP 包大小 */
#define IP_DEFAULT_TTL 64        /**< 默认生存时间 */
#define IP_DEFAULT_FLAGS 0       /**< 默认标志（DF=0, MF=0） */

/* 分片相关 */
#define IP_FRAG_DF 0x4000          /**< Don't Fragment 标志 */
#define IP_FRAG_MF 0x2000          /**< More Fragments 标志 */
#define IP_FRAG_OFFSET_MASK 0x1FFF /**< 片偏移掩码 */
#define IP_FRAG_MAX_ENTRIES 8      /**< 同时重组的最大分片组数 */

/* 上层协议分发器最大注册数 */
#define IP_PROTO_MAX 8

/** IPv4 头部结构体 */
struct ip_header
{
    uint8_t ver_ihl;      /**< 版本(4bit) + 头部长度(4bit, 以4字节为单位) */
    uint8_t tos;          /**< 服务类型 */
    uint16_t total_len;   /**< 总长度（头+数据） */
    uint16_t id;          /**< 标识符（分片用） */
    uint16_t frag_offset; /**< 标志(3bit) + 片偏移(13bit) */
    uint8_t ttl;          /**< 生存时间 */
    uint8_t proto;        /**< 上层协议号 */
    uint16_t checksum;    /**< 头部校验和 */
    uint32_t src_addr;    /**< 源 IPv4 地址 */
    uint32_t dst_addr;    /**< 目标 IPv4 地址 */
    /* 选项字段可选，通常不处理 */
} __attribute__((packed));

typedef int (*ip_proto_handler_fn)(struct netif *ni, struct mbuf *m,
                                   uint32_t src_ip, uint32_t dst_ip);
// 分片重组条目结构体
struct ip_frag_entry
{
    uint32_t src_addr;
    uint32_t dst_addr;
    uint16_t id;
    uint8_t proto;
    uint32_t timeout;   /**< 重组超时时间戳 */
    struct mbuf *chain; /**< 已到达的分片链 */
    uint16_t total_len; /**< 预期总长度（首分片确定） */
    uint8_t active;
};

//初始化
void ip_init(void);

//发送
int ip_output(uint32_t dst_addr, uint8_t proto, struct mbuf *m);

//接收
int ip_input(struct netif *ni, struct mbuf *m);

//注册上层协议处理函数
int ip_register_proto(uint8_t proto, ip_proto_handler_fn handler);

//注销上层协议处理函数
void ip_unregister_proto(uint8_t proto);

//设置IP地址和子网掩码
void ip_set_address(struct netif *ni, uint32_t ip_addr, uint32_t netmask);

//获取IP地址
uint32_t ip_get_address(struct netif *ni);

//判断IP地址是否为本地网卡地址
int ip_is_local(uint32_t ip_addr);

// IP分片重组定时器处理函数
void ip_frag_tick(void);

#endif
/**
 * @file    ip.h
 * @brief   IPv4 网络层模块公开接口
 *
 * 提供 IPv4 数据包的收发、分片重组、校验以及向上层协议分发。
 * 通过 ll_dispatch 接收，通过 netif 发送，传输层协议通过注册回调接收。
 */

#ifndef IP_H
#define IP_H

#include <stdint.h>
#include "mbuf.h"
#include "netif.h"

/* -------------------------------------------------------------------------- */
/*  预定义常量                                                                */
/* -------------------------------------------------------------------------- */

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

/* -------------------------------------------------------------------------- */
/*  类型定义                                                                  */
/* -------------------------------------------------------------------------- */

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

/** IP 层上层协议处理回调类型 */
typedef int (*ip_proto_handler_fn)(struct netif *ni, struct mbuf *m,
                                   uint32_t src_ip, uint32_t dst_ip);

/** 分片重装条目（内部使用，不对外暴露细节） */
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

/* -------------------------------------------------------------------------- */
/*  公开函数接口                                                              */
/* -------------------------------------------------------------------------- */

/* ---------- 初始化 ---------- */

/**
 * @brief 初始化 IP 模块。
 */
void ip_init(void);

/* ---------- 发送 ---------- */

/**
 * @brief 发送一个 IPv4 数据包（供传输层调用）。
 *        内部会查找路由、调用 ARP、分片（如需要）、通过 netif 发出。
 * @param dst_addr  目标 IPv4 地址（网络字节序）。
 * @param proto     上层协议号（IP_PROTO_TCP 等）。
 * @param m         包含上层数据的 mbuf，IP 头部由本函数添加。
 * @return 0 成功，-1 失败。
 */
int ip_output(uint32_t dst_addr, uint8_t proto, struct mbuf *m);

/* ---------- 接收 ---------- */

/**
 * @brief IPv4 输入处理函数，注册到 ll_dispatch（ETHERTYPE_IPV4）。
 *        处理完成后，若目标为本机，根据协议号分发给注册的上层处理函数。
 * @param ni 收到包的接口。
 * @param m  包含 IP 包的 mbuf（已跳过以太网头）。
 * @return 0 成功，非 0 错误。
 */
int ip_input(struct netif *ni, struct mbuf *m);

/* ---------- 上层协议注册 ---------- */

/**
 * @brief 注册上层协议处理函数（TCP、UDP、ICMP 等模块调用）。
 * @param proto   协议号（IP_PROTO_TCP 等）。
 * @param handler 处理函数指针。
 * @return 0 成功，-1 协议号已被占用或表满。
 */
int ip_register_proto(uint8_t proto, ip_proto_handler_fn handler);

/**
 * @brief 注销上层协议处理函数。
 * @param proto 协议号。
 */
void ip_unregister_proto(uint8_t proto);

/* ---------- 地址与状态 ---------- */

/**
 * @brief 为指定接口设置 IPv4 地址。
 * @param ni      网络接口。
 * @param ip_addr IPv4 地址（网络字节序）。
 * @param netmask 子网掩码（网络字节序）。
 */
void ip_set_address(struct netif *ni, uint32_t ip_addr, uint32_t netmask);

/**
 * @brief 获取接口的 IPv4 地址。
 * @param ni 网络接口。
 * @return IPv4 地址（网络字节序），未配置返回 0。
 */
uint32_t ip_get_address(struct netif *ni);

/**
 * @brief 检查目标 IP 是否为本机地址。
 * @param ip_addr IPv4 地址（网络字节序）。
 * @return 1 是本机，0 不是。
 */
int ip_is_local(uint32_t ip_addr);

/* ---------- 分片控制 ---------- */

/**
 * @brief 定期清理超时的分片重装条目，由定时器或主循环调用。
 */
void ip_frag_tick(void);

#endif /* IP_H */
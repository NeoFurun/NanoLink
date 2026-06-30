#ifndef NETIF_H
#define NETIF_H

#include <stdint.h>
#include "mbuf.h"

#define NETIF_NAME_LEN 8   //网卡名称长度
#define NETIF_HWADDR_LEN 6 //mac地址长度

/* 接口状态标志 */
#define NETIF_FLAG_UP 0x01        //网卡已启用
#define NETIF_FLAG_BROADCAST 0x02 //支持广播
#define NETIF_FLAG_LOOPBACK 0x04  //回环接口

struct netif;
struct driver_ops;

/** 输入回调函数类型：接收包时调用，返回 0 表示成功消费，非 0 表示未处理 */
typedef int (*netif_input_fn)(struct netif *ni, struct mbuf *m);

/** 发送回调函数类型：驱动层实现的发送函数 */
typedef int (*netif_send_fn)(struct netif *ni, struct mbuf *m);


struct netif
{
    /*网卡基本信息*/
    char name[NETIF_NAME_LEN]; //网卡名称:"tap0"
    uint8_t hwaddr[NETIF_HWADDR_LEN]; //MAC地址
    uint16_t hwaddr_len;//MAC地址长度 一般是48字节 故长度一般为6
    uint16_t mtu; //最大传输单元
    uint16_t flags;  //网卡状态
    
    /*IP配置信息*/
    uint32_t ip_addr; //网卡IP地址
    uint32_t netmask; //网卡子网掩码
    
    /*驱动相关信息*/
    const struct driver_ops *ops; //指向具体驱动的操作表
    void *priv; //驱动私有数据，比如 TAP 的 fd
    
    /*收发回调*/
    netif_send_fn send; //发包函数
    netif_input_fn input; //收包函数
    
    /*链表指针*/
    struct netif *next; //把多张网卡串起来
};

//初始化一张网卡结构体。
void netif_init(struct netif *ni, const char *name,
                netif_send_fn send, netif_input_fn input);

//把一张网卡挂到全局 netif_list 链表里。
int netif_register(struct netif *ni);

//把一张网卡从 netif_list 链表里摘掉。
void netif_unregister(struct netif *ni);

//根据目标 IP 找一张合适的网卡。
struct netif *netif_find_by_ip(uint32_t ip_addr);

//返回默认网卡，也就是 netif_list 的头节点。
struct netif *netif_get_default(void);

//驱动收到包后，通过这个函数把 mbuf 交给协议栈
int netif_input(struct netif *ni, struct mbuf *m);

//修改 flags 里的 NETIF_FLAG_UP 位。
void netif_set_up(struct netif *ni);

//修改 flags 里的 NETIF_FLAG_UP 位。
void netif_set_down(struct netif *ni);

#endif
/**
 * @file    ll_dispatch.h
 * @brief   链路层协议分发器公开接口
 *
 * 根据以太网帧类型（EtherType）将接收到的数据包分发给注册的协议处理函数。
 * 这是一个独立胶水层，netif 只调用 dispatch，不感知具体协议。
 */

#ifndef LL_DISPATCH_H
#define LL_DISPATCH_H

#include <stdint.h>
#include "mbuf.h"
#include "netif.h"

/* -------------------------------------------------------------------------- */
/*  预定义常量                                                                */
/* -------------------------------------------------------------------------- */

#define LL_DISPATCH_MAX_PROTO 16 /**< 最大注册协议数量 */

/* 常见以太网类型（可扩展） */
#define ETHERTYPE_IPV4 0x0800
#define ETHERTYPE_ARP 0x0806
#define ETHERTYPE_IPV6 0x86DD

/* -------------------------------------------------------------------------- */
/*  类型定义                                                                  */
/* -------------------------------------------------------------------------- */

/** 链路层协议处理函数指针类型 */
typedef int (*ll_handler_fn)(struct netif *ni, struct mbuf *m);

/* -------------------------------------------------------------------------- */
/*  公开函数接口                                                              */
/* -------------------------------------------------------------------------- */

/**
 * @brief 注册一个链路层协议处理函数。
 * @param ether_type  以太网帧类型（如 ETHERTYPE_IPV4）。
 * @param handler     处理函数指针。
 * @return 0 成功，-1 注册表满或类型已被占用。
 */
int ll_dispatch_register(uint16_t ether_type, ll_handler_fn handler);

/**
 * @brief 注销一个链路层协议处理函数。
 * @param ether_type  以太网帧类型。
 */
void ll_dispatch_unregister(uint16_t ether_type);

/**
 * @brief 分发入口：由 netif 在 input 回调中调用。
 *        根据 mbuf 中链路层头部的类型字段找到对应注册函数并调用。
 *        如未注册，则释放 mbuf 并返回 -1。
 * @param ni   收到包的 netif。
 * @param m    收到的包（包含链路层头）。
 * @return 处理函数的返回值，或 -1 表示无法分发。
 */
int ll_dispatch_input(struct netif *ni, struct mbuf *m);

#endif /* LL_DISPATCH_H */
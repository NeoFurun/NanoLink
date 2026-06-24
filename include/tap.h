/**
 * @file    tap.h
 * @brief   TAP 虚拟以太网接口驱动
 *
 * 基于 Linux TUN/TAP，创建一张内核可见的虚拟网卡。
 * 程序通过读写文件描述符收发原始以太网帧。
 *
 * 用法:
 *   driver_register("tap0", &tap_ops, NULL);
 *   driver_init_all();  // 内部调 tap_init，打开 /dev/net/tun
 *
 * Wireshark 抓 tap0 即可观察 NanoLink 发出的帧。
 */

#ifndef TAP_H
#define TAP_H

#include "driver.h"

/** TAP 驱动操作接口（供 driver_register 使用） */
extern const struct driver_ops tap_ops;

#endif /* TAP_H */

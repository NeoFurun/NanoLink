# NanoLink

[![Language](https://img.shields.io/badge/language-C11-blue.svg)](https://en.wikipedia.org/wiki/C11_(C_standard_revision))
[![Lines](https://img.shields.io/badge/lines-~4800-orange.svg)]()
[![License](https://img.shields.io/badge/license-MIT-green.svg)](LICENSE)

**从零实现的轻量级 TCP/IP 协议栈**，纯 C11，~4800 行代码。

用 Linux TAP 虚拟网卡收发真实以太网帧——系统的 `ping`、`nc`、Wireshark 可以直接跟它通信，就像跟一台真实的网络设备对话。

---

## 能做什么

| 协议 | 功能 | 验证方式 |
|------|------|---------|
| **ICMP** | Ping 回复 | `ping 10.0.0.1` |
| **ARP** | IP→MAC 解析 + 缓存 + 超时重试 | Wireshark 抓包 |
| **UDP** | 端口绑定 + 数据报收发 + Echo | `nc -u 10.0.0.1 8888` |
| **TCP** | 三次握手 + 数据收发 + 四次挥手 + 重传 | `nc 10.0.0.1 9999` |
| **IP** | 校验/分片识别/路由查找 | 上层依赖 |

---

## 架构

```
┌──────────────────────────────────────────┐
│  socket        BSD Socket API            │
├──────────────────────────────────────────┤
│  tcp / udp     传输层 (端口 + 连接)      │
├──────────────────────────────────────────┤
│  ip / icmp     网络层 (路由 + Ping)      │
├──────────────────────────────────────────┤
│  arp           地址解析 (IP→MAC)         │
├──────────────────────────────────────────┤
│  ll_dispatch   链路层分发 (EtherType→协议)│
├──────────────────────────────────────────┤
│  driver / tap  驱动适配 + TAP 虚拟网卡    │
├──────────────────────────────────────────┤
│  netif         网卡抽象 (全局链表)        │
├──────────────────────────────────────────┤
│  mbuf          零拷贝缓冲区池 (256个)     │
├──────────────────────────────────────────┤
│  timer         定时器服务 (32个, 静态池)  │
├──────────────────────────────────────────┤
│  osal          操作系统适配层             │
└──────────────────────────────────────────┘
```

**设计原则：**
- **静态池** — mbuf / timer / ARP 表 / TCP PCB 全部预分配，不依赖 malloc
- **零拷贝** — 各层协议头通过 mbuf_prepend/pull 移动指针，数据不搬移
- **跨平台** — osal 层隔离操作系统，移植仅需替换一个文件
- **回调注入** — netif→ll_dispatch→ip→上层，每层通过函数指针解耦

---

## 快速开始

### 编译

```bash
git clone https://github.com/furunze/NanoLink.git
cd NanoLink
make
```

### 运行

```bash
# 终端 1：启动协议栈
sudo ./bin/nanolink tap0 10.0.0.1 255.255.255.0

# 终端 2：配网 + 测试
sudo ip link set tap0 up
sudo ip addr add 10.0.0.2/24 dev tap0

# ICMP
ping 10.0.0.1

# UDP Echo（端口 8888）
echo "Hello UDP" | nc -u -w1 10.0.0.1 8888

# TCP Echo（端口 9999）
echo "Hello TCP" | nc -w2 10.0.0.1 9999
```

### Wireshark 抓包

```bash
sudo wireshark -i tap0 -k &
```

然后 ping / nc，可以看到完整的 ARP → TCP 三次握手 → 数据 → 四次挥手。

---

## 文件结构

```
NanoLink/
├── main.c                    # 入口 + 内嵌 echo server
├── include/                  # 15 个头文件
│   ├── osal.h               #   操作系统适配
│   ├── mbuf.h               #   缓冲区管理
│   ├── timer.h              #   定时器服务
│   ├── netif.h              #   网卡抽象
│   ├── driver.h             #   驱动框架
│   ├── tap.h                #   TAP 驱动
│   ├── ll_dispatch.h        #   链路分发
│   ├── arp.h                #   地址解析
│   ├── ip.h                 #   IPv4
│   ├── icmp.h               #   ICMP (Ping)
│   ├── route.h              #   路由表
│   ├── udp.h                #   UDP
│   ├── tcp.h                #   TCP
│   ├── socket.h             #   Socket API
│   └── event_bus.h          #   事件总线
├── src/                     # 15 个实现文件
│   └── *.c
├── Makefile
├── API_REFERENCE.md         # API 参考手册（中文）
└── README.md
```

---

## Wireshark 验证截图示例

ping 10.0.0.1 时，Wireshark 抓 tap0 可以看到：

```
No.  Time      Source       Destination   Protocol  Info
1    0.000000  10.0.0.2     10.0.0.1      ARP       Who has 10.0.0.1?
2    0.000123  10.0.0.1     10.0.0.2      ARP       10.0.0.1 is at 00:11:22:33:44:00
3    0.000234  10.0.0.2     10.0.0.1      ICMP      Echo (ping) request
4    0.000345  10.0.0.1     10.0.0.2      ICMP      Echo (ping) reply
```

TCP 交互可以看到完整的 10+ 帧序列：SYN → SYN-ACK → ACK → PSH → ACK → FIN → ACK ...

---

## 关键设计

### mbuf 零拷贝

```
收包路径（逐层剥头，数据不动）:
  [ETH 14B|IP 20B|TCP 20B|数据]
      ↑ data
  mbuf_pull(m, 14):
  [ETH|IP 20B|TCP 20B|数据]
          ↑ data               ← IP 层直接看到 IP 头
  mbuf_pull(m, 20):
  [ETH|IP|TCP 20B|数据]
              ↑ data           ← TCP 层直接看到 TCP 头

发包路径（逐层加头）:
  mbuf_prepend(m, 20)          ← IP 层在前面加头
  mbuf_prepend(m, 14)          ← ETH 层在前面加头
  全程只移动指针，数据从未拷贝
```

### TCP 状态机

完整的 TCP 状态转换（CLOSED ↔ LISTEN ↔ SYN_RECV ↔ ESTABLISHED ↔ FIN_WAIT_1/2 ↔ TIME_WAIT），支持：
- 三次握手 SYN→SYN-ACK→ACK
- 数据分段、序列号确认、滑动窗口
- 超时重传（500ms 间隔，最多 3 次）
- 同时关闭 (CLOSING 状态)
- RST 强制终止

### 跨平台设计

`osal` 层封装所有平台差异。Linux 实现用 `malloc` / `clock_gettime`。移植只需替换 `osal.c`，FreeRTOS 用 `pvPortMalloc` / `xTaskGetTickCount` 即可。

---

## 技术亮点

- **零依赖** — 只依赖 Linux 内核头文件和 libc，无第三方库
- **无动态分配** — mbuf/timer/PCB/ARP 全部静态预分配
- **Wireshark 可验证** — 每个协议层的行为都可以抓包证实
- **完整文档** — 每个函数都有中文注释和示例

---

## 项目定位

适合 **嵌入式网络开发** 学习和求职展示。涵盖了嵌入式工程师需要掌握的：

- C 语言底层编程（指针、位运算、字节序）
- 静态内存管理（池化设计）
- TCP/IP 协议原理（动手实现理解最深）
- 驱动适配（回调注入、跨平台抽象）
- 调试工具链（GDB、Wireshark、tcpdump）

---

## License

MIT

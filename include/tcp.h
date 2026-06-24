/**
 * @file    tcp.h
 * @brief   TCP 传输控制协议模块公开接口
 *
 * 面向连接、可靠的字节流传输。
 * 通过 IP 层注册接收，依赖定时器模块处理重传。
 * 对上层只暴露控制块操作接口。
 */

#ifndef TCP_H
#define TCP_H

#include <stdint.h>
#include "mbuf.h"
#include "ip.h"

/* -------------------------------------------------------------------------- */
/*  预定义常量                                                                */
/* -------------------------------------------------------------------------- */

#define TCP_HEADER_LEN_MIN 20   /**< 最小 TCP 头长度（无选项） */
#define TCP_HEADER_LEN_MAX 60   /**< 最大 TCP 头长度（含选项） */
#define TCP_DEFAULT_MSS 536     /**< 默认最大报文段长度 */
#define TCP_DEFAULT_WINDOW 8192 /**< 默认接收窗口大小 */
#define TCP_DEFAULT_TTL 64      /**< 默认 TTL */

/* TCP 状态 */
#define TCP_STATE_CLOSED 0      /**< 关闭 */
#define TCP_STATE_LISTEN 1      /**< 监听 */
#define TCP_STATE_SYN_SENT 2    /**< SYN 已发送 */
#define TCP_STATE_SYN_RECV 3    /**< SYN 已接收 */
#define TCP_STATE_ESTABLISHED 4 /**< 连接建立 */
#define TCP_STATE_FIN_WAIT_1 5  /**< 主动关闭第一次等待 */
#define TCP_STATE_FIN_WAIT_2 6  /**< 主动关闭第二次等待 */
#define TCP_STATE_CLOSE_WAIT 7  /**< 被动关闭等待应用关闭 */
#define TCP_STATE_CLOSING 8     /**< 双方同时关闭 */
#define TCP_STATE_LAST_ACK 9    /**< 被动关闭最后 ACK 等待 */
#define TCP_STATE_TIME_WAIT 10  /**< 主动关闭 2MSL 等待 */

/* TCP 标志位 */
#define TCP_FLAG_FIN 0x01
#define TCP_FLAG_SYN 0x02
#define TCP_FLAG_RST 0x04
#define TCP_FLAG_PSH 0x08
#define TCP_FLAG_ACK 0x10
#define TCP_FLAG_URG 0x20

/* 控制块标志 */
#define TCP_PCB_FLAG_BOUND 0x01   /**< 已绑定本地端口 */
#define TCP_PCB_FLAG_LISTEN 0x02  /**< 处于监听态 */
#define TCP_PCB_FLAG_CLOSING 0x04 /**< 正在关闭 */

/* 最大控制块数量 */
#define TCP_MAX_PCB 16

/* -------------------------------------------------------------------------- */
/*  类型定义                                                                  */
/* -------------------------------------------------------------------------- */

/** TCP 头部结构体 */
struct tcp_header
{
    uint16_t src_port;     /**< 源端口 */
    uint16_t dst_port;     /**< 目标端口 */
    uint32_t seq_num;      /**< 序号 */
    uint32_t ack_num;      /**< 确认号 */
    uint16_t hdrlen_flags; /**< 头部长度(4bit) + 保留(6bit) + 标志(6bit) */
    uint16_t window;       /**< 窗口大小 */
    uint16_t checksum;     /**< 校验和 */
    uint16_t urg_ptr;      /**< 紧急指针 */
} __attribute__((packed));

/* 前置声明 */
struct tcp_pcb;

/** 上层回调函数类型 */
typedef void (*tcp_recv_fn)(struct tcp_pcb *pcb, struct mbuf *m);
typedef void (*tcp_accept_fn)(struct tcp_pcb *new_pcb);
typedef void (*tcp_sent_fn)(struct tcp_pcb *pcb, uint16_t len);
typedef void (*tcp_err_fn)(struct tcp_pcb *pcb, int err);

/** TCP 控制块（对外不完全暴露，仅指针） */
struct tcp_pcb
{
    /* 本地和远端信息 */
    uint32_t local_ip;
    uint16_t local_port;
    uint32_t remote_ip;
    uint16_t remote_port;
    uint8_t state; /**< 当前状态 */
    uint8_t flags; /**< 控制块标志 */

    /* 序列号与确认 */
    uint32_t snd_una; /**< 已发送但未确认的起始序号 */
    uint32_t snd_nxt; /**< 下一个发送序号 */
    uint32_t rcv_nxt; /**< 期望接收的下一个序号 */
    uint16_t rcv_wnd; /**< 接收窗口 */
    uint16_t snd_wnd; /**< 发送窗口（对方通告） */

    /* MSS 与 MTU */
    uint16_t mss; /**< 最大报文段长度 */

    /* 重传与定时器 */
    void *retransmit_timer;   /**< 重传定时器句柄 */
    void *timewait_timer;     /**< TIME-WAIT 定时器句柄 */
    uint8_t retransmit_count; /**< 重传次数 */
    uint32_t rto;             /**< 重传超时时间（ms） */

    /* 数据缓冲 */
    struct mbuf *unsent_queue;  /**< 待发送数据队列 */
    struct mbuf *unacked_queue; /**< 已发送未确认队列 */
    struct mbuf *out_of_order;  /**< 乱序队列（接收方） */
    struct mbuf *recv_queue;    /**< 已排好序待应用读取 */

    /* 回调 */
    tcp_recv_fn recv_callback;
    tcp_accept_fn accept_callback;
    tcp_sent_fn sent_callback;
    tcp_err_fn err_callback;
    void *priv; /**< 上层私有数据（如 socket） */

    struct tcp_pcb *next; /**< 链表指针（内部管理） */
};

/* -------------------------------------------------------------------------- */
/*  公开函数接口                                                              */
/* -------------------------------------------------------------------------- */

/* ---------- 初始化 ---------- */
/**
 * @brief 初始化 TCP 模块，注册到 IP 层，初始化定时器依赖等。
 */
void tcp_init(void);

/* ---------- 控制块管理 ---------- */
/**
 * @brief 创建一个 TCP 控制块。
 * @return 新控制块指针，失败返回 NULL。
 */
struct tcp_pcb *tcp_new(void);

/**
 * @brief 绑定控制块到本地 IP 和端口。
 * @param pcb       控制块。
 * @param local_ip  本地 IP（网络字节序），可填 0。
 * @param port      本地端口（主机字节序）。
 * @return 0 成功，-1 端口被占用。
 */
int tcp_bind(struct tcp_pcb *pcb, uint32_t local_ip, uint16_t port);

/**
 * @brief 开始监听（进入 LISTEN 状态）。
 * @param pcb       控制块。
 * @param backlog   未 accept 队列最大长度。
 */
void tcp_listen(struct tcp_pcb *pcb, uint8_t backlog);

/**
 * @brief 主动连接远端（发起三次握手）。
 * @param pcb         控制块（需先绑定本地信息）。
 * @param remote_ip   远端 IP（网络字节序）。
 * @param remote_port 远端端口（主机字节序）。
 * @return 0 已发送 SYN，-1 失败。
 */
int tcp_connect(struct tcp_pcb *pcb, uint32_t remote_ip, uint16_t remote_port);

/**
 * @brief 接受一个已完成的连接（由 accept 回调通知后调用）。
 * @param listen_pcb  监听控制块。
 * @return 新连接的控制块指针，若没有则返回 NULL。
 */
struct tcp_pcb *tcp_accept(struct tcp_pcb *listen_pcb);

/**
 * @brief 发送数据（将数据加入待发送队列，受窗口和拥塞控制）。
 * @param pcb  控制块。
 * @param m    包含应用数据的 mbuf（协议栈负责释放）。
 * @return 0 成功入队，-1 发送窗口满或连接未建立。
 */
int tcp_send(struct tcp_pcb *pcb, struct mbuf *m);

/**
 * @brief 主动关闭连接（发送 FIN）。
 * @param pcb 控制块。
 */
void tcp_close(struct tcp_pcb *pcb);

/**
 * @brief 立即终止连接（发送 RST，释放资源）。
 * @param pcb 控制块。
 */
void tcp_abort(struct tcp_pcb *pcb);

/* ---------- 回调设置 ---------- */
void tcp_set_recv_callback(struct tcp_pcb *pcb, tcp_recv_fn cb);
void tcp_set_accept_callback(struct tcp_pcb *pcb, tcp_accept_fn cb);
void tcp_set_sent_callback(struct tcp_pcb *pcb, tcp_sent_fn cb);
void tcp_set_err_callback(struct tcp_pcb *pcb, tcp_err_fn cb);

/* ---------- 定时器驱动 ---------- */
/**
 * @brief TCP 慢定时器，每 500ms 或 1s 调用一次（驱动重传、保活等）。
 *        由全局定时器模块或主循环触发。
 */
void tcp_slow_tick(void);

/**
 * @brief TCP 快定时器，每 200ms 调用一次（驱动延迟 ACK 等）。
 *        由全局定时器模块或主循环触发。
 */
void tcp_fast_tick(void);

/* ---------- 输入处理 ---------- */
/**
 * @brief TCP 输入处理函数（注册到 IP 层 IP_PROTO_TCP）。
 * @param ni       收到包的接口。
 * @param m        包含 TCP 段的 mbuf（IP 头已移除）。
 * @param src_ip   源 IP 地址。
 * @param dst_ip   目的 IP 地址。
 * @return 0 成功，-1 无匹配控制块或错误。
 */
int tcp_input(struct netif *ni, struct mbuf *m, uint32_t src_ip, uint32_t dst_ip);

#endif /* TCP_H */
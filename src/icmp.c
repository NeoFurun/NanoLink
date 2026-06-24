/**
 * @file    icmp.c
 * @brief   ICMP 控制消息协议模块实现
 *
 * 实现 Ping (Echo Request/Reply) 和差错报告。
 * 通过 ip_register_proto(IP_PROTO_ICMP) 注册收包回调。
 *
 * 例（ping 过程）:
 *   内核 ping 10.0.0.1:
 *     内核 → TAP → NanoLink 收到 ICMP Echo Request
 *     icmp_input → 改 type=ECHO_REPLY → ip_output 发回去
 *     内核收到 Echo Reply → ping 成功
 */

#include "../include/icmp.h"
#include "../include/ip.h"
#include "../include/osal.h"
#include <string.h>

/* ICMP 校验和（覆盖 ICMP 头 + 数据）。
 * mode=0: 验证，返回 0xFFFF 表示正确
 * mode=1: 计算并填入 checksum 字段
 *
 * 注意：本函数假设 ICMP 数据在内存中连续存放（从 icmp 指针起 len 字节）。
 * 如果 mbuf 链跨节点分片，icmp 指针指向的缓冲区不足以覆盖整个 len 时会
 * 越界读取。当前 ICMP 消息体量小（ping 载荷通常 < 64 字节），总是位于
 * 单个 mbuf 节点内，此假设安全。若未来支持 ICMP 大消息（如 PMTUD），
 * 需改用 mbuf_copy_to 先将数据拷入连续缓冲区再计算校验和。 */
static uint16_t icmp_checksum(struct icmp_header *icmp, uint16_t len, int mode)
{
    uint32_t sum = 0;
    uint16_t *p = (uint16_t *)icmp;
    int i;

    if (mode == 1) icmp->checksum = 0;

    for (i = 0; i < len / 2; i++) {
        sum += p[i];
    }
    if (len & 1) {
        sum += ((uint8_t *)icmp)[len - 1] << 8;
    }

    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }

    if (mode == 1) {
        icmp->checksum = (uint16_t)(~sum);
        return icmp->checksum;
    }

    return (uint16_t)sum;
}

/* ==========================================================================
   icmp_init — 初始化 ICMP 模块
   ========================================================================== */
/**
 * 向 IP 层注册：proto=1 的包交给 icmp_input。
 *
 * 例（协议栈启动）:
 *   icmp_init();
 *   // ip_proto_table: proto=1 → icmp_input
 *   // 之后 ping 本机就能收到回复
 */
void icmp_init(void)
{
    ip_register_proto(IP_PROTO_ICMP, icmp_input);
}

/* ==========================================================================
   icmp_input — 处理收到的 ICMP 包
   ========================================================================== */
/**
 * 目前只处理 Echo Request (ping)，改为 Echo Reply 发回。
 *
 * 例（收到 ping 请求）:
 *   mbuf m: [ type=8(ECHO_REQUEST) | code=0 | checksum | id=0x1234 | seq=1 | 56字节数据 ]
 *   src_ip=10.0.0.2, dst_ip=10.0.0.1(本机)
 *
 *   icmp_input(&tap0, m, 0x0A000002, 0x0A000001):
 *     1. 校验 checksum ✓
 *     2. type==8 → ECHO_REQUEST
 *     3. 原地改: type=0(ECHO_REPLY)
 *     4. 重新算 checksum
 *     5. ip_output(m, dst=10.0.0.2, proto=IP_PROTO_ICMP)
 *     6. 内核收到 Echo Reply → ping 成功
 */
int icmp_input(struct netif *ni, struct mbuf *m, uint32_t src_ip, uint32_t dst_ip)
{
    (void)dst_ip;
    struct icmp_header *icmp;
    uint16_t total;
    uint16_t calc_cs;

    if (ni == NULL || m == NULL) return -1;
    if (m->len < ICMP_HEADER_LEN) {
        mbuf_free(m);
        return -1;
    }

    icmp  = (struct icmp_header *)m->data;
    total = m->total_len;

    /* 校验 checksum */
    calc_cs = icmp_checksum(icmp, total, 0);
    if (calc_cs != 0xFFFF) {

        mbuf_free(m);
        return -1;
    }

    switch (icmp->type) {
    case ICMP_TYPE_ECHO_REQUEST:
        /* Ping 请求 → 改为 Echo Reply 发回去 */
        icmp->type = ICMP_TYPE_ECHO_REPLY;
        icmp_checksum(icmp, total, 1); /* 重算校验和 */
        ip_output(src_ip, IP_PROTO_ICMP, m);
        return 0;

    default:
        /* 未知类型，丢弃 */
        mbuf_free(m);
        return -1;
    }
}

/* ==========================================================================
   icmp_send_error — 发送 ICMP 差错报文
   ========================================================================== */
/**
 * 发送目标不可达等差错。携带触发差错的原始 IP 包头部。
 *
 * 例（UDP 端口不可达）:
 *   mbuf *orig = 收到的 UDP 包（IP头+UDP头）
 *   icmp_send_error(ICMP_TYPE_DEST_UNREACHABLE,
 *                   ICMP_CODE_PORT_UNREACHABLE, 0, orig);
 *   // 构造 ICMP 差错报文发给源 IP
 *   // 对方收到 → "端口不可达"
 */
int icmp_send_error(uint8_t type, uint8_t code, uint32_t info, struct mbuf *original)
{
    struct icmp_header *icmp;
    struct ip_header *orig_iph;
    struct mbuf *m;
    uint16_t payload_len;
    uint32_t dst_ip;
    (void)info;

    if (original == NULL) return -1;

    orig_iph = (struct ip_header *)original->data;
    dst_ip = orig_iph->src_addr;

    /* ICMP 差错报文负载：原始 IP 头 + 至少 8 字节传输层头 */
    payload_len = IP_HEADER_MIN_LEN + 8;
    if (original->total_len < payload_len) {
        payload_len = original->total_len;
    }

    m = mbuf_alloc();
    if (m == NULL) return -1;

    mbuf_append(m, ICMP_HEADER_LEN + payload_len);

    icmp = (struct icmp_header *)m->data;
    icmp->type    = type;
    icmp->code    = code;
    icmp->checksum = 0;
    icmp->data.rest = 0;

    /* 拷贝原始包头部到负载 */
    mbuf_copy_to(original, m->data + ICMP_HEADER_LEN, payload_len);

    icmp_checksum(icmp, m->total_len, 1);

    return ip_output(dst_ip, IP_PROTO_ICMP, m);
}

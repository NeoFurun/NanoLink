#include "../include/icmp.h"
#include "../include/ip.h"
#include "../include/osal.h"
#include <string.h>

static uint16_t icmp_checksum(struct icmp_header *icmp, uint16_t len, int mode)
{
    uint32_t sum = 0;
    uint16_t *p = (uint16_t *)icmp;
    int i;

    if (mode == 1)
        icmp->checksum = 0;

    for (i = 0; i < len / 2; i++)
    {
        sum += p[i];
    }
    if (len & 1)
    {
        sum += ((uint8_t *)icmp)[len - 1] << 8;
    }

    while (sum >> 16)
    {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }

    if (mode == 1)
    {
        icmp->checksum = (uint16_t)(~sum);
        return icmp->checksum;
    }

    return (uint16_t)sum;
}

void icmp_init(void)
{
    ip_register_proto(IP_PROTO_ICMP, icmp_input);
}

int icmp_input(struct netif *ni, struct mbuf *m, uint32_t src_ip, uint32_t dst_ip)
{
    (void)dst_ip;
    struct icmp_header *icmp;
    uint16_t total;
    uint16_t calc_cs;

    if (ni == NULL || m == NULL)
        return -1;
    if (m->len < ICMP_HEADER_LEN)
    {
        mbuf_free(m);
        return -1;
    }

    icmp = (struct icmp_header *)m->data;
    total = m->total_len;

    /* 校验 checksum */
    calc_cs = icmp_checksum(icmp, total, 0);
    if (calc_cs != 0xFFFF)
    {

        mbuf_free(m);
        return -1;
    }

    switch (icmp->type)
    {
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

int icmp_send_error(uint8_t type, uint8_t code, uint32_t info, struct mbuf *original)
{
    struct icmp_header *icmp;
    struct ip_header *orig_iph;
    struct mbuf *m;
    uint16_t payload_len;
    uint32_t dst_ip;
    (void)info;

    if (original == NULL)
        return -1;

    orig_iph = (struct ip_header *)original->data;
    dst_ip = orig_iph->src_addr;

    /* ICMP 差错报文负载：原始 IP 头 + 至少 8 字节传输层头 */
    payload_len = IP_HEADER_MIN_LEN + 8;
    if (original->total_len < payload_len)
    {
        payload_len = original->total_len;
    }

    m = mbuf_alloc();
    if (m == NULL)
        return -1;

    mbuf_append(m, ICMP_HEADER_LEN + payload_len);

    icmp = (struct icmp_header *)m->data;
    icmp->type = type;
    icmp->code = code;
    icmp->checksum = 0;
    icmp->data.rest = 0;

    /* 拷贝原始包头部到负载 */
    mbuf_copy_to(original, m->data + ICMP_HEADER_LEN, payload_len);

    icmp_checksum(icmp, m->total_len, 1);

    return ip_output(dst_ip, IP_PROTO_ICMP, m);
}

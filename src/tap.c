#include "../include/tap.h"
#include "../include/netif.h"
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/if.h>
#include <linux/if_tun.h>

/* 每个 TAP 接口一个 fd，存 ni->priv */
static int tap_fds[DRIVER_MAX_COUNT];

/* ==========================================================================
   tap_init — 打开 /dev/net/tun，创建虚拟以太网卡
   ========================================================================== */
static int tap_init(struct netif *ni)
{
    struct ifreq ifr;
    int fd;
    int i;

    if (ni == NULL) return -1;

    fd = open("/dev/net/tun", O_RDWR);
    if (fd < 0) return -1;

    memset(&ifr, 0, sizeof(ifr));
    ifr.ifr_flags = IFF_TAP | IFF_NO_PI; /* 以太网帧，不带额外包头 */
    strncpy(ifr.ifr_name, ni->name, IFNAMSIZ - 1);
    ifr.ifr_name[IFNAMSIZ - 1] = '\0';

    if (ioctl(fd, TUNSETIFF, &ifr) < 0) {
        close(fd);
        return -1;
    }

    ni->mtu = ETH_MTU;

    /* 默认 MAC: 00:11:22:33:44:XX，末尾用池索引区分 */
    for (i = 0; i < DRIVER_MAX_COUNT; i++) {
        if (tap_fds[i] == 0) {
            tap_fds[i] = fd;
            break;
        }
    }
    if (i >= DRIVER_MAX_COUNT) {
        /* tap_fds 数组已满，无法再注册新 TAP 设备 */
        close(fd);
        return -1;
    }

    memset(ni->hwaddr, 0, ETH_ADDR_LEN);
    ni->hwaddr[0] = 0x00;
    ni->hwaddr[1] = 0x11;
    ni->hwaddr[2] = 0x22;
    ni->hwaddr[3] = 0x33;
    ni->hwaddr[4] = 0x44;
    ni->hwaddr[5] = (uint8_t)(i & 0xFF);

    ni->priv = &tap_fds[i];

    return 0;
}

/* ==========================================================================
   tap_send — 把以太网帧写入 TAP 设备
   ========================================================================== */
static int tap_send(struct netif *ni, struct mbuf *m)
{
    uint8_t buf[ETH_MTU + ETH_HEADER_LEN + 64]; /* 够装一个完整帧 */
    int fd;
    uint16_t total;
    ssize_t n;

    if (ni == NULL || m == NULL) return -1;

    fd = *(int *)ni->priv;

    total = m->total_len;
    if (total > sizeof(buf)) return -1;

    {
        uint16_t copied = mbuf_copy_to(m, buf, total);
        if (copied != total) return -1;
    }

    n = write(fd, buf, total);

    if (n < 0) return -1;

    return 0;
}

/* ==========================================================================
   tap_close — 关闭 TAP 设备
   ========================================================================== */
static void tap_close(struct netif *ni)
{
    int fd;
    int i;

    if (ni == NULL) return;
    if (ni->priv == NULL) return;

    fd = *(int *)ni->priv;

    /* 清理 fd 槽位 */
    for (i = 0; i < DRIVER_MAX_COUNT; i++) {
        if (tap_fds[i] == fd) {
            tap_fds[i] = 0;
            break;
        }
    }

    close(fd);
}

/* ==========================================================================
   tap_ops — 供 driver_register 使用的驱动操作接口
   ========================================================================== */
const struct driver_ops tap_ops = {
    .init       = tap_init,
    .send       = tap_send,
    .close      = tap_close,
    .set_mac    = NULL,  /* 暂不支持 MAC 修改 */
    .get_status = NULL,
};

/* SPDX-License-Identifier: GPL-2.0-only */
/* Register definitions derived from MediaTek's GPL-2.0 wusb3801.h
 * (Copyright (c) 2019 MediaTek Inc.). See ../许可证与来源.md.
 * Shared by the kernel adapter and the offline C tests. No hidden test writes.
 */
#ifndef WUSB3801_A25_CORE_H
#define WUSB3801_A25_CORE_H
#ifdef __KERNEL__
#include <linux/errno.h>
#else
#include <errno.h>
#endif
#define WA_ID 0x01
#define WA_CTRL 0x02
#define WA_INT 0x03
#define WA_STATUS 0x04
#define WA_SINK_MASKED 0x81 /* SNK, accessories disabled, IRQ masked */
#define WA_SINK_ACTIVE 0x80
#define WA_PEER_SOURCE 0x08
#define WA_PEER_SINK 0x04
struct wa_io {
 void *ctx;
 int (*read)(void *ctx, unsigned char reg);
 int (*write)(void *ctx, unsigned char reg, unsigned char value);
};
struct wa_state {
 int attached; /* Only detached=0 or local sink=1; NEVER source. */
 int cc;       /* 0 unknown/detached, 1 CC1, 2 CC2 */
 int rp;       /* Advertised peer current code; NOT a charging setpoint. */
 int vbus;
 int valid;
 int error;
};
static inline int wa_binding(int bus, int addr, unsigned int mode,
                             unsigned int host_current, int approved)
{
 if (bus != 1 || addr != 0x60) return -ENODEV;
 if (mode != 0x20 || host_current != 1) return -EINVAL;
 return approved ? 0 : -EPERM;
}
static inline int wa_source_vbus(int mv)
{
 /* There is deliberately no approval switch for source power. */
 return mv ? -EPERM : 0;
}
static inline int wa_program(struct wa_io *io, int active)
{
 int ret, value = active ? WA_SINK_ACTIVE : WA_SINK_MASKED;
 ret = io->write(io->ctx, WA_CTRL, value);
 if (ret < 0) return ret;
 ret = io->read(io->ctx, WA_CTRL);
 if (ret < 0) return ret;
 return ret == value ? 0 : -EIO;
}
static inline int wa_start(struct wa_io *io)
{
 int ret = io->read(io->ctx, WA_ID);
 if (ret < 0) return ret;
 if ((ret & 7) != 6) return -ENODEV;
 /* Do not restore a possibly-source bootloader mode on failure. */
 return wa_program(io, 0);
}
static inline int wa_decode(int status, struct wa_state *s)
{
 int type = status & 0x1c;
 s->attached = 0; s->cc = 0; s->rp = 0;
 s->valid = 0; s->vbus = 0; s->error = 0;
 if (status < 0) return s->error = status;
 s->vbus = !!(status & 0x80);
 if (!type) { s->valid = 1; return 0; }
 if (type == WA_PEER_SINK) return s->error = -EPERM;
 if (type != WA_PEER_SOURCE) return s->error = -EOPNOTSUPP;
 /* CC=0 requires the vendor test-register workaround; not audited. */
 if ((status & 3) != 1 && (status & 3) != 2)
  return s->error = -EAGAIN;
 if (!s->vbus) return s->error = -EAGAIN;
 s->cc = status & 3; s->rp = (status >> 5) & 3;
 /* Stock can report 0x89/0x8a. Report a conservative default Rp then,
  * without claiming this establishes a charger current limit. */
 if (!s->rp) s->rp = 1;
 s->attached = 1; s->valid = 1;
 return 0;
}
static inline int wa_sample(struct wa_io *io, struct wa_state *s, int *irq)
{
 int ret = io->read(io->ctx, WA_INT); /* read-to-clear, exactly once */
 *irq = 0;
 if (ret < 0) return wa_decode(ret, s);
 *irq = ret & 3;
 ret = io->read(io->ctx, WA_STATUS);
 return wa_decode(ret, s);
}
#endif

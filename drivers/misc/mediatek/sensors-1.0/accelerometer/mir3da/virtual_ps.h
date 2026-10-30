/* SPDX-License-Identifier: GPL-2.0 */
#ifndef A25_VIRTUAL_PS_H
#define A25_VIRTUAL_PS_H
#include <linux/types.h>
struct a25_ps_ops {
	int (*enable)(int en);
	int (*batch)(int flag, s64 period, s64 latency);
	int (*flush)(void);
	int (*cali)(const char *buf, size_t count);
};
int a25_ps_register(const struct a25_ps_ops *ops);
void a25_ps_unregister(void);
int a25_ps_event(int value, s64 timestamp, bool flush);
#endif

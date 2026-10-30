/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _A25_LCM_POWER_H
#define _A25_LCM_POWER_H
#include <linux/types.h>
/* Sleepable built-in-only interface. begin holds the client lifetime lock. */
int a25_bias_ready(void);
int a25_bias_begin(bool need_client);
void a25_bias_end(void);
int a25_bias_write_locked(unsigned char reg, unsigned char value);
/* Implemented by the TPD resource owner, NOT by the incomplete axs_ts client. */
int tpd_a25_reset_ready(void);
int tpd_a25_reset_20(void);
/* Cold probe only: high/100ms followed by the stock reset_20 pulse. */
int tpd_a25_probe_reset(void);
#endif

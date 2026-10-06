/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _A25_TOUCH_H
#define _A25_TOUCH_H
#include <linux/types.h>
/* Paired sleepable calls; begin holds lifecycle lock until end. */
void a25_touch_boot_handoff(void);
void a25_touch_panel_begin(void);
void a25_touch_panel_end(bool ready);
#endif

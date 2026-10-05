// SPDX-License-Identifier: GPL-2.0
/*
 * axs15260_dsi_6580_hd.c
 * AXS15260 (AxsTech / 爱协生) TDDI LCD driver for MTK LCM framework.
 *
 * Rebuilt from stock-kernel binary evidence (A25 backup boot.img):
 * 384x854, DSI 3-lane, SYNC_EVENT_VDO_MODE, RGB888, PLL 210,
 * VSA/VBP/VFP=2/20/170, HSA/HBP/HFP=20/180/180.
 * Init table = stock 8-entry table (BB/B2/B2/11/delay120/29/delay10/end).
 * Evidence: review_axs_boot/{结论.md,evidence.json,verified-disassembly.txt}
 */

#include <linux/string.h>
#include <linux/gpio.h>
#include "lcm_drv.h"

/* 内核 delay.h 的 mdelay/udelay 是宏,会破坏 lcm_util.<member> 访问 */
#ifdef mdelay
#undef mdelay
#endif
#ifdef udelay
#undef udelay
#endif

#define MDELAY(n)	(lcm_util.mdelay(n))
#define UDELAY(n)	(lcm_util.udelay(n))

/* stock calls util->dsi_set_cmdq_V2 (synchronous, no cmdq), not V22 */
#define dsi_set_cmdq_V2(cmd, count, ppara, force_update) \
	lcm_util.dsi_set_cmdq_V2(cmd, count, ppara, force_update)

#define FRAME_WIDTH  (384)
#define FRAME_HEIGHT (854)

/* marker values per stock evidence: 0xffe = delay, 0xfff = end */
#define REGFLAG_DELAY		0xFFE
#define REGFLAG_UDELAY		0xFFD
#define REGFLAG_END_OF_TABLE	0xFFF

struct LCM_setting_table {
	unsigned int cmd;
	unsigned char count;
	unsigned char para_list[64];
};

static struct LCM_UTIL_FUNCS lcm_util;

static struct LCM_setting_table lcm_initialization_setting[] = {
	/* stock table (8 entries), field layout: cmd u32 @0, count u8 @4, payload @5 */
	{0xBB, 8, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x5a, 0xa5} },
	{0xB2, 11, {0x01, 0x00, 0x00, 0x06, 0x00, 0x0a, 0x04, 0x21, 0x9c, 0x00, 0x1e} },
	{0xB2, 1, {0x41} },
	{0x11, 0, {0} },
	{REGFLAG_DELAY, 120, {0} },
	{0x29, 0, {0} },
	{REGFLAG_DELAY, 10, {0} },
	{REGFLAG_END_OF_TABLE, 0, {0} },
};

static struct LCM_setting_table lcm_suspend_setting[] = {
	/* stock: 28 00 -> delay120 -> 10 00 -> delay30 -> end */
	{0x28, 0, {0} },
	{REGFLAG_DELAY, 120, {0} },
	{0x10, 0, {0} },
	{REGFLAG_DELAY, 30, {0} },
	{REGFLAG_END_OF_TABLE, 0, {0} },
};

static void push_table(void *cmdq, struct LCM_setting_table *table,
	unsigned int count, unsigned char force_update)
{
	unsigned int i;
	unsigned int cmd;

	(void)cmdq;	/* stock path sends synchronously via V2 */

	for (i = 0; i < count; i++) {
		cmd = table[i].cmd;

		switch (cmd) {
		case REGFLAG_DELAY:
			MDELAY(table[i].count);
			break;
		case REGFLAG_UDELAY:
			UDELAY(table[i].count);
			break;
		case REGFLAG_END_OF_TABLE:
			break;
		default:
			dsi_set_cmdq_V2(cmd, table[i].count,
				table[i].para_list, force_update);
		}
	}
}

static void lcm_set_util_funcs(const struct LCM_UTIL_FUNCS *util)
{
	memcpy(&lcm_util, util, sizeof(struct LCM_UTIL_FUNCS));
}

static void lcm_get_params(struct LCM_PARAMS *params)
{
	memset(params, 0, sizeof(struct LCM_PARAMS));

	params->type = LCM_TYPE_DSI;

	params->width = FRAME_WIDTH;
	params->height = FRAME_HEIGHT;

	params->dsi.mode = SYNC_EVENT_VDO_MODE;
	params->dsi.LANE_NUM = LCM_THREE_LANE;
	params->dsi.data_format.format = LCM_DSI_FORMAT_RGB888;
	params->dsi.PS = LCM_PACKED_PS_24BIT_RGB888;
	params->dsi.intermediat_buffer_num = 2;

	params->dsi.vertical_sync_active = 2;
	params->dsi.vertical_backporch = 20;
	params->dsi.vertical_frontporch = 170;
	params->dsi.vertical_active_line = FRAME_HEIGHT;

	params->dsi.horizontal_sync_active = 20;
	params->dsi.horizontal_backporch = 180;
	params->dsi.horizontal_frontporch = 180;
	params->dsi.horizontal_active_pixel = FRAME_WIDTH;

	params->dsi.packet_size = 256;
	params->dsi.PLL_CLOCK = 210;
	params->dsi.clk_lp_per_line_enable = 0;
	/* stock leaves both ESD fields unwritten (=0) */
	params->dsi.esd_check_enable = 0;
	params->dsi.customization_esd_check_enable = 0;
}

static void lcm_init(void)
{
	push_table(NULL, lcm_initialization_setting,
		sizeof(lcm_initialization_setting) /
		sizeof(struct LCM_setting_table), 1);
}

static void lcm_suspend(void)
{
	push_table(NULL, lcm_suspend_setting,
		sizeof(lcm_suspend_setting) /
		sizeof(struct LCM_setting_table), 1);
}

static void lcm_resume(void)
{
	push_table(NULL, lcm_initialization_setting,
		sizeof(lcm_initialization_setting) /
		sizeof(struct LCM_setting_table), 1);
}

static unsigned int lcm_compare_id(void)
{
	/* stock reads DA/DB/DC over DSI and compares against 0x005260.
	 * Single-panel device: always-match keeps one candidate valid.
	 * Not yet implemented (see review_axs_boot/结论.md). */
	return 1;
}

struct LCM_DRIVER axs15260_dsi_6580_hd_lcm_drv = {
	.name = "axs15260_dsi_6580_hd",
	.set_util_funcs = lcm_set_util_funcs,
	.get_params = lcm_get_params,
	.init = lcm_init,
	.suspend = lcm_suspend,
	.resume = lcm_resume,
	.compare_id = lcm_compare_id,
};

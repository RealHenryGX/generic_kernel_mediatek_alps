/* SPDX-License-Identifier: GPL-2.0 */
/*
 * PSC5425E charger driver — iBRIT A25 (MT6765), faithful stock replication.
 *
 * Register map and request->code mappings are reverse-engineered from the
 * A25 stock.Image (see fix/charger_safety_audit/报告.md) and cross-checked
 * against the official PSC5425E Rev.06.5 datasheet (fix/charger_specs_research/).
 *
 * Safety status (see fix/charger_functional/结论.md):
 *  - ICE = 0: confirmed. The stock driver never accesses the SPR register
 *    (no 0x51/0x91 access in its disassembly), so ICE stays at power-on
 *    default 0. This driver likewise never touches SPR.
 *  - Rsense (50mOhm vs 56mOhm) is UNKNOWN. It does NOT change the
 *    request->code mapping below (that mapping is fixed in stock); it only
 *    changes the physical mA each code produces. No physical current value
 *    is asserted anywhere in this driver.
 *  - Hardware safety timers (90-min wake / 12-hour) and the MTK framework's
 *    temperature monitoring are independent of this driver and remain in
 *    effect regardless of the software callbacks below.
 *
 * Differences from stock (all error-path hardening, no behavior change):
 *  - Strict i2c read/write error checks with proper -errno returns (stock
 *    swallowed read errors and still wrote, and returned 1/0 instead of
 *    errno).
 *  - Per-device data via charger_get_data() instead of a file-scope global
 *    i2c_client.
 *  - Probe requires a successful, non-zero chip-ID read (0x0b) and fails
 *    cleanly otherwise; stock ignored the read failure and proceeded.
 *  - kick_wdt() only reads R0 (the 32s OTG watchdog is kept alive by ANY
 *    i2c access per the datasheet; R0[5:4] is read-only STAT, so writing it
 *    back — which stock did — is not a defined register action).
 *
 * NOT validated on hardware. Keep default-n until an on-device charge test
 * passes.
 */

#include <linux/err.h>
#include <linux/errno.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/types.h>
#include "charger_class.h"

/* Register addresses (PSC5425E, 7-bit I2C addr 0x6a) */
#define PSC5425E_REG_STATUS	0x00	/* [5:4] STAT (RO): 00 ready, 01 chg, 10 done */
#define PSC5425E_REG_CTRL	0x01	/* [7:6] AICR, [5] OTG_CFG, [2] CE, [0] OTG */
#define PSC5425E_REG_VOREG	0x02	/* [7:2] CV */
#define PSC5425E_REG_ICHG	0x04	/* [6:4] ICHG */
#define PSC5425E_REG_MISC	0x05	/* [5:4] WDT, [3] EN_TIMER, [7] ADD20MV */
#define PSC5425E_REG_ID		0x0b	/* non-zero = chip present */

/* Bit fields (from stock disassembly + datasheet) */
#define PSC5425E_AICR_MASK	0x03
#define PSC5425E_AICR_SHIFT	6
#define PSC5425E_CE_MASK	0x01
#define PSC5425E_CE_SHIFT	2
#define PSC5425E_CV_MASK	0x3f
#define PSC5425E_CV_SHIFT	2
#define PSC5425E_ICHG_MASK	0x07
#define PSC5425E_ICHG_SHIFT	4
#define PSC5425E_STAT_MASK	0x03
#define PSC5425E_STAT_SHIFT	4

struct psc5425e_info {
	struct i2c_client *client;
	struct charger_device *chg_dev;
	const char *chg_dev_name;
	struct charger_properties chg_props;
};

static int psc5425e_config_interface(struct i2c_client *client, u8 reg,
				     u8 value, u8 mask, u8 shift)
{
	s32 ret;
	u8 cur;

	ret = i2c_smbus_read_byte_data(client, reg);
	if (ret < 0)
		return ret;
	cur = (u8)ret;
	cur &= (u8)~(mask << shift);
	cur |= (value & mask) << shift;
	ret = i2c_smbus_write_byte_data(client, reg, cur);
	if (ret < 0)
		return ret;
	return 0;
}

/* --- stock request->code mappings (faithful; do not reinterpret) --- */
static u8 psc5425e_ichg_code(u32 request_uA)
{
	if (request_uA <= 656000U)
		return 0;
	if (request_uA <= 786000U)
		return 1;
	if (request_uA <= 1048000U)
		return 2;
	if (request_uA <= 1180000U)
		return 3;
	if (request_uA <= 1442000U)
		return 4;
	if (request_uA <= 1574000U)
		return 5;
	if (request_uA <= 1836000U)
		return 6;
	return 7;
}

static u8 psc5425e_cv_code(u32 request_uV)
{
	if (request_uV <= 4100000U)
		return 0;
	if (request_uV <= 4200000U)
		return 2;
	if (request_uV <= 4350000U)
		return 44;
	return 45;
}

static u8 psc5425e_aicr_code(u32 request_uA)
{
	return request_uA <= 50000U ? 1 : 3;
}

/* --- charger_ops callbacks --- */

static int psc5425e_enable_charging(struct charger_device *dev, bool en)
{
	struct psc5425e_info *info = charger_get_data(dev);
	u8 v = en ? 0 : 1;	/* CE: 0 = charge enabled, 1 = charge disabled */

	return psc5425e_config_interface(info->client, PSC5425E_REG_CTRL, v,
					 PSC5425E_CE_MASK, PSC5425E_CE_SHIFT);
}

static int psc5425e_set_current(struct charger_device *dev, u32 uA)
{
	struct psc5425e_info *info = charger_get_data(dev);
	u8 code = psc5425e_ichg_code(uA);

	return psc5425e_config_interface(info->client, PSC5425E_REG_ICHG, code,
					 PSC5425E_ICHG_MASK, PSC5425E_ICHG_SHIFT);
}

static int psc5425e_set_cv_voltage(struct charger_device *dev, u32 uV)
{
	struct psc5425e_info *info = charger_get_data(dev);
	u8 code = psc5425e_cv_code(uV);

	return psc5425e_config_interface(info->client, PSC5425E_REG_VOREG, code,
					 PSC5425E_CV_MASK, PSC5425E_CV_SHIFT);
}

static int psc5425e_set_input_current(struct charger_device *dev, u32 uA)
{
	struct psc5425e_info *info = charger_get_data(dev);
	u8 code = psc5425e_aicr_code(uA);

	return psc5425e_config_interface(info->client, PSC5425E_REG_CTRL, code,
					 PSC5425E_AICR_MASK, PSC5425E_AICR_SHIFT);
}

static int psc5425e_kick_wdt(struct charger_device *dev)
{
	struct psc5425e_info *info = charger_get_data(dev);
	s32 ret;

	/* Datasheet: 32s watchdog is OTG-only and any i2c access keeps it
	 * alive; R0[5:4] is read-only STAT, so a plain read is sufficient. */
	ret = i2c_smbus_read_byte_data(info->client, PSC5425E_REG_STATUS);

	return ret < 0 ? ret : 0;
}

static int psc5425e_get_charging_status(struct charger_device *dev, bool *done)
{
	struct psc5425e_info *info = charger_get_data(dev);
	s32 ret;
	u8 stat;

	ret = i2c_smbus_read_byte_data(info->client, PSC5425E_REG_STATUS);
	if (ret < 0)
		return ret;
	stat = ((u8)ret >> PSC5425E_STAT_SHIFT) & PSC5425E_STAT_MASK;
	*done = (stat == 0x2);	/* STAT=10 -> charge done */
	return 0;
}

static const struct charger_ops psc5425e_chg_ops = {
	.enable = psc5425e_enable_charging,
	.set_charging_current = psc5425e_set_current,
	.set_constant_voltage = psc5425e_set_cv_voltage,
	.set_input_current = psc5425e_set_input_current,
	.kick_wdt = psc5425e_kick_wdt,
	.is_charging_done = psc5425e_get_charging_status,
};

/* --- probe --- */

static int psc5425e_driver_probe(struct i2c_client *client,
				 const struct i2c_device_id *id)
{
	struct psc5425e_info *info;
	s32 chip_id;
	int ret;

	info = devm_kzalloc(&client->dev, sizeof(*info), GFP_KERNEL);
	if (!info)
		return -ENOMEM;
	info->client = client;

	/* Chip-ID: non-zero = present. Fail cleanly on read error or absent. */
	chip_id = i2c_smbus_read_byte_data(client, PSC5425E_REG_ID);
	if (chip_id < 0)
		return chip_id;
	if (chip_id == 0)
		return -ENODEV;

	info->chg_dev_name = "primary_chg";
	of_property_read_string(client->dev.of_node, "charger_name",
				&info->chg_dev_name);
	info->chg_props.alias_name = "psc5425e";
	of_property_read_string(client->dev.of_node, "alias_name",
				&info->chg_props.alias_name);

	/* Faithful stock init sequence (RMW params from disassembly). */
	ret = psc5425e_config_interface(client, PSC5425E_REG_CTRL, 0xf8, 0xff, 0);
	if (ret)
		return ret;
	ret = psc5425e_config_interface(client, PSC5425E_REG_VOREG, 0x24,
					PSC5425E_CV_MASK, PSC5425E_CV_SHIFT);
	if (ret)
		return ret;
	ret = psc5425e_config_interface(client, PSC5425E_REG_ICHG, 0x76, 0xff, 0);
	if (ret)
		return ret;
	ret = psc5425e_config_interface(client, PSC5425E_REG_MISC, 0x24, 0xff, 0);
	if (ret)
		return ret;

	info->chg_dev = charger_device_register(info->chg_dev_name, &client->dev,
						info, &psc5425e_chg_ops,
						&info->chg_props);
	if (IS_ERR_OR_NULL(info->chg_dev))
		return info->chg_dev ? PTR_ERR(info->chg_dev) : -ENOMEM;

	i2c_set_clientdata(client, info);
	return 0;
}

static const struct of_device_id psc5425e_of_match[] = {
	{ .compatible = "mediatek,psc5425e" },
	{ }
};
MODULE_DEVICE_TABLE(of, psc5425e_of_match);

static const struct i2c_device_id psc5425e_i2c_id[] = {
	{ "psc5425e", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, psc5425e_i2c_id);

static struct i2c_driver psc5425e_driver = {
	.driver = {
		.name = "psc5425e-a25",
		.of_match_table = psc5425e_of_match,
	},
	.probe = psc5425e_driver_probe,
	.id_table = psc5425e_i2c_id,
};
module_i2c_driver(psc5425e_driver);

MODULE_DESCRIPTION("A25 PSC5425E charger (stock-equivalent)");
MODULE_LICENSE("GPL v2");

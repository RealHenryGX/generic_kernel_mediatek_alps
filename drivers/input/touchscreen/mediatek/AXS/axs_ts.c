// SPDX-License-Identifier: GPL-2.0
/*
 * AXS15260 (AxsTech / 爱协生) TDDI touch driver
 * MediaTek TPD framework driver.
 *
 * Protocol verified against stock zImage (axs_read_bytes disasm):
 *   point[0]>>6 = event, point[2]>>4 = id, 6 bytes per point
 *   x = (point[0]&0xf)<<8|point[1], y = (point[2]&0xf)<<8|point[3]
 *   I2C read of 32 bytes returns header + up to 5 points.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/interrupt.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/slab.h>
#include <linux/gpio.h>
#include <linux/regulator/consumer.h>

#include "tpd.h"

#ifndef TPD_RES_X
#define TPD_RES_X 384
#endif
#ifndef TPD_RES_Y
#define TPD_RES_Y 854
#endif

#define AXS_DEVICE		"axs_ts"
#define AXS_DRIVER_VERSION	"1.0.0"

#define AXS15260_MAX_POINTS	5
#define AXS15260_TOUCH_HEADER_LEN	2
#define AXS15260_TOUCH_POINT_LEN	6
#define AXS15260_TOUCH_DATA_LEN	(AXS15260_TOUCH_HEADER_LEN + \
				 AXS15260_MAX_POINTS * AXS15260_TOUCH_POINT_LEN)
#define AXS15260_REG_VERSION	0x0c

#define AXS15260_EVENT_UP	1
#define AXS15260_EVENT_CONTACT	2

static struct i2c_client *axs_client;
static struct input_dev *axs_input_dev;
static int axs_irq_gpio = -1;

static int axs_ts_init_flag;

static void axs_ts_reset(void)
{

	if (axs_irq_gpio >= 0) {
		gpio_direction_output(axs_irq_gpio, 0);
		mdelay(5);
		gpio_direction_output(axs_irq_gpio, 1);
		mdelay(20);
		gpio_direction_output(axs_irq_gpio, 0);
		mdelay(150);
	}
}

static int axs_read_version(struct i2c_client *client, u16 *version)
{
	u8 reg = AXS15260_REG_VERSION;
	u8 data[2];
	struct i2c_msg msgs[] = {
		{ .addr = client->addr, .len = 1, .buf = &reg },
		{ .addr = client->addr, .flags = I2C_M_RD, .len = sizeof(data), .buf = data },
	};
	int error;

	error = i2c_transfer(client->adapter, msgs, ARRAY_SIZE(msgs));
	if (error != ARRAY_SIZE(msgs))
		return error < 0 ? error : -EIO;
	*version = (data[0] << 8) | data[1];
	return 0;
}

static void axs_ts_report(u8 *data)
{
	u8 point_count = data[1] & 0x0f;
	u8 esd_flag = data[1] >> 4;
	unsigned long active_slots = 0;
	unsigned int index;

	if (data[0] > 0x0f || point_count > AXS15260_MAX_POINTS)
		goto report;
	if (esd_flag && esd_flag != 0x08 && esd_flag != 0x04)
		goto report;

	for (index = 0; index < point_count; index++) {
		u8 *point = &data[AXS15260_TOUCH_HEADER_LEN +
				 index * AXS15260_TOUCH_POINT_LEN];
		u8 event = point[0] >> 6;
		u8 id = point[2] >> 4;
		unsigned int x, y;

		if (id >= AXS15260_MAX_POINTS || event > AXS15260_EVENT_CONTACT)
			continue;

		input_mt_slot(axs_input_dev, id);
		if (event == AXS15260_EVENT_UP) {
			input_mt_report_slot_state(axs_input_dev, MT_TOOL_FINGER, false);
			continue;
		}
		if (active_slots & BIT(id))
			continue;

		x = ((point[0] & 0x0f) << 8) | point[1];
		y = ((point[2] & 0x0f) << 8) | point[3];
		input_mt_report_slot_state(axs_input_dev, MT_TOOL_FINGER, true);
		input_report_abs(axs_input_dev, ABS_MT_POSITION_X, x);
		input_report_abs(axs_input_dev, ABS_MT_POSITION_Y, y);
		input_report_abs(axs_input_dev, ABS_MT_PRESSURE, point[4]);
		input_report_abs(axs_input_dev, ABS_MT_TOUCH_MAJOR, point[5] >> 4);
		active_slots |= BIT(id);
	}

report:
	input_mt_sync_frame(axs_input_dev);
	input_mt_report_pointer_emulation(axs_input_dev, true);
	input_sync(axs_input_dev);
}

static irqreturn_t axs_ts_irq_handler(int irq, void *dev_id)
{
	u8 data[AXS15260_TOUCH_DATA_LEN];
	int error;

	if (!axs_client)
		return IRQ_HANDLED;

	error = i2c_master_recv(axs_client, data, sizeof(data));
	if (error != sizeof(data))
		return IRQ_HANDLED;

	axs_ts_report(data);
	return IRQ_HANDLED;
}

static void axs_ts_irq_enable(void)
{
	if (axs_irq_gpio >= 0)
		enable_irq(gpio_to_irq(axs_irq_gpio));
}

static void axs_ts_irq_disable(void)
{
	if (axs_irq_gpio >= 0)
		disable_irq_nosync(gpio_to_irq(axs_irq_gpio));
}

static int axs_ts_tpd_local_init(void)
{
	struct i2c_adapter *adapter;
	u16 version;
	int ret;

	adapter = i2c_get_adapter(0);
	if (!adapter) {
		TPD_DMESG("no i2c adapter\n");
		return -ENODEV;
	}

	/* read version to confirm presence */
	{
		struct i2c_client *tmp = kzalloc(sizeof(*tmp), GFP_KERNEL);
		if (!tmp) {
			i2c_put_adapter(adapter);
			return -ENOMEM;
		}
		tmp->adapter = adapter;
		tmp->addr = 0x38; /* 实机 i2c-0@0x38(原厂 dtbo overlay 实锤)*/
		ret = axs_read_version(tmp, &version);
		if (ret == 0)
			TPD_DMESG("probe: fw version 0x%04x\n", version);
		kfree(tmp);
	}
	i2c_put_adapter(adapter);

	/* input device */
	axs_input_dev = input_allocate_device();
	if (!axs_input_dev)
		return -ENOMEM;

	axs_input_dev->name = "AXS15260 Touchscreen";
	axs_input_dev->id.bustype = BUS_I2C;

	__set_bit(EV_ABS, axs_input_dev->evbit);
	__set_bit(EV_KEY, axs_input_dev->evbit);
	__set_bit(BTN_TOUCH, axs_input_dev->keybit);

	input_set_abs_params(axs_input_dev, ABS_MT_POSITION_X, 0, TPD_RES_X, 0, 0);
	input_set_abs_params(axs_input_dev, ABS_MT_POSITION_Y, 0, TPD_RES_Y, 0, 0);
	input_set_abs_params(axs_input_dev, ABS_MT_PRESSURE, 0, 0xff, 0, 0);
	input_set_abs_params(axs_input_dev, ABS_MT_TOUCH_MAJOR, 0, 0x0f, 0, 0);

	ret = input_mt_init_slots(axs_input_dev, AXS15260_MAX_POINTS,
				  INPUT_MT_DIRECT | INPUT_MT_DROP_UNUSED);
	if (ret) {
		input_free_device(axs_input_dev);
		return ret;
	}

	ret = input_register_device(axs_input_dev);
	if (ret) {
		input_free_device(axs_input_dev);
		return ret;
	}

	/* request IRQ */
	if (axs_irq_gpio >= 0) {
		ret = request_threaded_irq(gpio_to_irq(axs_irq_gpio), NULL,
					   axs_ts_irq_handler,
					   IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
					   AXS_DEVICE, NULL);
		if (ret) {
			TPD_DMESG("request_irq failed %d\n", ret);
			input_unregister_device(axs_input_dev);
			return ret;
		}
	}

	axs_ts_init_flag = 1;
	TPD_DMESG("local init ok\n");
	return 0;
}

static void axs_ts_suspend(struct device *h)
{
	TPD_DMESG("suspend\n");
	axs_ts_irq_disable();
}

static void axs_ts_resume(struct device *h)
{
	TPD_DMESG("resume\n");
	axs_ts_reset();
	axs_ts_irq_enable();
}

static struct tpd_driver_t axs_tpd_driver = {
	.tpd_device_name = "axs_ts",
	.tpd_local_init = axs_ts_tpd_local_init,
	.suspend = axs_ts_suspend,
	.resume = axs_ts_resume,
};

static int __init axs_ts_init(void)
{
	int ret;

	TPD_DMESG("AXS15260 init (v%s)\n", AXS_DRIVER_VERSION);
	ret = tpd_driver_add(&axs_tpd_driver);
	if (ret)
		TPD_DMESG("tpd_driver_add failed %d\n", ret);
	return ret;
}

static void __exit axs_ts_exit(void)
{
	tpd_driver_remove(&axs_tpd_driver);
	if (axs_input_dev)
		input_unregister_device(axs_input_dev);
}

module_init(axs_ts_init);
module_exit(axs_ts_exit);

MODULE_AUTHOR("Gravity");
MODULE_DESCRIPTION("AXS15260 TDDI touch driver for MTK TPD");
MODULE_LICENSE("GPL v2");
MODULE_VERSION(AXS_DRIVER_VERSION);

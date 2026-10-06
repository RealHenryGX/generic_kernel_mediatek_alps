/* For MTK android platform.
 *
 * mir3da.c - Linux kernel modules for 3-Axis Accelerometer
 *
 * Copyright (C) 2011-2013 MiraMEMS Sensing Technology Co., Ltd.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * A25 candidate: reworked transport/framework glue from fixed Teracube
 * da218 source. No claim of A25 chip or calibration equivalence.
 */
#include "cust_acc.h"
#include "accel.h"
#include "mir3da_core.h"
#include "mir3da_cust.h"
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/math64.h>
#ifdef CONFIG_MTK_MIR3DA_A25_VIRTUAL_PS
#include <linux/workqueue.h>
#include <linux/hrtimer.h>
#include <linux/pm_wakeup.h>
#include <linux/timekeeping.h>
#include "stock_ps.h"
#include "virtual_ps.h"
#include <linux/miscdevice.h>
#ifdef CONFIG_COMPAT
#include <linux/compat.h>
#endif
#endif

/* Deliberately no automatic activation, even if someone selects Kconfig.
 * Lab-only opt-in, read-only after boot. Review report before overriding.
 */
static bool allow_unverified_hw;
module_param(allow_unverified_hw, bool, 0400);
MODULE_PARM_DESC(allow_unverified_hw,
	"LAB ONLY: permit unverified Teracube register profile on A25");

struct mir3da_state {
	struct i2c_client *client;
	struct acc_hw hw;
	struct hwmsen_convert cvt;
	bool ready;
	bool enabled;
	bool suspended;
#ifdef CONFIG_MTK_MIR3DA_A25_VIRTUAL_PS
	struct work_struct sample_work;
	struct hrtimer sample_timer;
	struct wakeup_source *ps_wake;
	struct stock_ps_state ps_state;
	bool acc_on, ps_on, cache_valid, recovering;
	bool acc_data_on, acc_nodata_on, factory_on;
	u64 hal_ns, nodata_ns, factory_ns;
	u64 acc_ns, ps_ns, sample_due, acc_due, sequence;
	int cache[3], acc_error, ps_error;
#endif
};
static struct mir3da_state *mir;
static DEFINE_MUTEX(mir_lock);
static int mir3da_init_flag = -ENODEV;

/* Do not truncate negative SMBus errno to a byte and return success. */
static int mir_read(PLAT_HANDLE handle, u8 reg, u8 *value)
{
	int ret = i2c_smbus_read_byte_data(handle, reg);

	if (ret < 0)
		return ret;
	*value = ret;
	return 0;
}
static int mir_write(PLAT_HANDLE handle, u8 reg, u8 value)
{
	return i2c_smbus_write_byte_data(handle, reg, value);
}
static int mir_read_block(PLAT_HANDLE handle, u8 reg, u8 count, u8 *value)
{
	/* Core expects byte count, not zero on success. */
	return i2c_smbus_read_i2c_block_data(handle, reg, count, value);
}
static int mir_address(PLAT_HANDLE handle)
{
	return ((struct i2c_client *)handle)->addr;
}
static void mir_delay(int ms)
{
	msleep(ms);
}
static MIR_GENERAL_OPS_DECLARE(mir_ops, mir_read, mir_read_block, mir_write,
	NULL, NULL, NULL, mir_address, NULL, mir_delay, printk, sprintf);

static int mir_available(void)
{
	if (!mir || !mir->ready)
		return -ENODEV;
	if (mir->suspended)
		return -EBUSY;
	return 0;
}

#ifdef CONFIG_MTK_MIR3DA_A25_VIRTUAL_PS
#include "runtime.inc"
#endif

static int mir3da_enable_nodata(int en)
{
#ifdef CONFIG_MTK_MIR3DA_A25_VIRTUAL_PS
	return mir_runtime_enable(false, en);
#else
	int ret;

	if (en != 0 && en != 1)
		return -EINVAL;
	mutex_lock(&mir_lock);
	ret = mir_available();
	if (!ret) {
		ret = mir3da_set_enable(mir->client, en);
		if (!ret)
			mir->enabled = en;
	}
	mutex_unlock(&mir_lock);
	return ret;
#endif
}

static int mir3da_set_delay(u64 ns)
{
#ifdef CONFIG_MTK_MIR3DA_A25_VIRTUAL_PS
	return mir_runtime_delay(false, ns);
#else
	u64 ms;
	int ret;

	if (!ns)
		return -EINVAL;
	ms = div_u64(ns, 1000000);
	if (ms > 2147483647ULL)
		return -ERANGE;
	mutex_lock(&mir_lock);
	ret = mir_available();
	if (!ret)
		ret = mir3da_set_odr(mir->client, (int)ms);
	mutex_unlock(&mir_lock);
	return ret;
#endif
}

static int mir3da_batch(int flag, int64_t ns, int64_t latency)
{
	/* No hardware FIFO contract established; never promise batching. */
	if (flag || latency)
		return -EOPNOTSUPP;
	if (ns <= 0)
		return -EINVAL;
	return mir3da_set_delay(ns);
}

static int mir3da_flush(void)
{
	int ret;

	mutex_lock(&mir_lock);
	ret = mir_available();
	/* Polling has no retained samples; report completion even disabled. */
	if (!ret) {
#ifdef CONFIG_MTK_MIR3DA_A25_VIRTUAL_PS
		ret = mir_runtime_flush_locked(false);
#else
		ret = acc_flush_report();
#endif
	}
	mutex_unlock(&mir_lock);
	return ret;
}

static int mir_read_sample(short raw[3])
{
	int ret = mir_available();

	if (ret)
		return ret;
	if (!mir->enabled)
		return -EAGAIN;
	/* Factory raw does not advance compensation or PS history. */
	return mir3da_read_raw_data(mir->client, &raw[0], &raw[1], &raw[2]);
}

static int mir3da_get_data(int *x, int *y, int *z, int *status)
{
#ifdef CONFIG_MTK_MIR3DA_A25_VIRTUAL_PS
	int ret;

	mutex_lock(&mir_lock);
	ret = mir_available();
	if (!ret && !mir->cache_valid)
		ret = -EAGAIN;
	if (!ret) {
		*x = mir->cache[0]; *y = mir->cache[1]; *z = mir->cache[2];
		*status = SENSOR_STATUS_UNRELIABLE;
	}
	mutex_unlock(&mir_lock);
	return ret;
#else
	short raw[3];
	int axes[3], i, ret;

	mutex_lock(&mir_lock);
	ret = mir_read_sample(raw);
	if (!ret) {
#ifdef CONFIG_MTK_MIR3DA_A25_STOCK_PIPELINE
		/* Exact A25 software stage; factory raw path remains unmodified.
		 * This does not register PS or claim physical sensor validation.
		 */
		mir3da_stock_compensate(raw);
#endif
		for (i = 0; i < 3; i++)
			axes[mir->cvt.map[i]] = mir->cvt.sign[i] *
				(int)raw[i] * GRAVITY_EARTH_1000 / 1024;
		*x = axes[0];
		*y = axes[1];
		*z = axes[2];
		/* No calibrated accuracy claim. */
		*status = SENSOR_STATUS_UNRELIABLE;
	}
	mutex_unlock(&mir_lock);
	return ret;
#endif
}
static int mir3da_factory_get_raw_data(int32_t data[3])
{
	short raw[3];
	int i, ret;

	mutex_lock(&mir_lock);
	ret = mir_read_sample(raw);
	if (!ret)
		for (i = 0; i < 3; i++)
			data[i] = raw[i];
	mutex_unlock(&mir_lock);
	return ret;
}
static int mir3da_factory_get_data(int32_t data[3], int *status)
{
	return mir3da_get_data(&data[0], &data[1], &data[2], status);
}
static int mir3da_factory_enable_sensor(bool enable, int64_t ms)
{
#ifdef CONFIG_MTK_MIR3DA_A25_VIRTUAL_PS
	int ret;

	mutex_lock(&mir_lock);
	ret = mir_available();
	if (!ret)
		ret = mir_factory_enable_locked(enable, ms);
	mutex_unlock(&mir_lock);
	return ret;
#else
	int ret;

	if (!enable)
		return mir3da_enable_nodata(0);
	if (ms <= 0 || ms > 2147483647LL)
		return -EINVAL;
	/* Set rate before enable so rate failure cannot leave sensor on. */
	ret = mir3da_batch(0, ms * 1000000LL, 0);
	return ret ? ret : mir3da_enable_nodata(1);
#endif
}
static int mir_unsupported(void)
{
	return -EOPNOTSUPP;
}
static int mir_unsupported_cali(int32_t data[3])
{
	return -EOPNOTSUPP;
}
static int mir_unsupported_blob(uint8_t *data, uint8_t count)
{
	return -EOPNOTSUPP;
}
static struct accel_factory_fops mir_factory_ops = {
	.enable_sensor = mir3da_factory_enable_sensor,
	.get_data = mir3da_factory_get_data,
	.get_raw_data = mir3da_factory_get_raw_data,
	.enable_calibration = mir_unsupported,
	.clear_cali = mir_unsupported,
	.set_cali = mir_unsupported_cali,
	.get_cali = mir_unsupported_cali,
	.do_self_test = mir_unsupported,
};
static struct accel_factory_public mir_factory = {
	.gain = 1, .sensitivity = 1, .fops = &mir_factory_ops,
};

#ifdef CONFIG_MTK_MIR3DA_A25_VIRTUAL_PS
#include "factory_owned.inc"
#endif

static int mir3da_probe(struct i2c_client *client,
	const struct i2c_device_id *id)
{
	struct mir3da_state *s;
	u32 direction;
	int ret;
	bool core_started = false;
	struct acc_control_path ctl = {
		.enable_nodata = mir3da_enable_nodata,
		.set_delay = mir3da_set_delay,
		.batch = mir3da_batch,
		.flush = mir3da_flush,
		.set_cali = mir_unsupported_blob,
		.is_report_input_direct = false,
		.is_support_batch = false,
		.is_use_common_factory = false,
#ifdef CONFIG_MTK_MIR3DA_A25_VIRTUAL_PS
		.a25_owned = true,
		.open_report_data = mir_acc_open_report_data,
#endif
	};
	struct acc_data_path data = {
		.get_data = mir3da_get_data, .vender_div = 1000,
	};

	if (!allow_unverified_hw) {
		dev_err(&client->dev, "blocked: unverified chip/calibration profile\n");
		return -EPERM;
	}
	if (client->addr != 0x26 || !client->dev.of_node)
		return -ENODEV;
	ret = of_property_read_u32(client->dev.of_node, "direction", &direction);
	if (ret)
		return ret;
	if (direction != 6)
		return -EINVAL;
	if (!i2c_check_functionality(client->adapter,
		I2C_FUNC_SMBUS_BYTE_DATA | I2C_FUNC_SMBUS_READ_I2C_BLOCK))
		return -EOPNOTSUPP;
	s = kzalloc(sizeof(*s), GFP_KERNEL);
	if (!s)
		return -ENOMEM;
	s->client = client;
#ifdef CONFIG_MTK_MIR3DA_A25_VIRTUAL_PS
	INIT_WORK(&s->sample_work, mir_sample_work);
	hrtimer_init(&s->sample_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	s->sample_timer.function = mir_sample_timer;
	s->acc_ns = s->ps_ns = MIR_DEFAULT_NS;
	s->hal_ns = s->nodata_ns = s->factory_ns = MIR_DEFAULT_NS;
	s->ps_wake = wakeup_source_register(NULL, "a25-virtual-ps");
	if (!s->ps_wake) {
		ret = -ENOMEM;
		goto free_state;
	}
	ctl.is_report_input_direct = true;
#endif
	ret = get_accel_dts_func(client->dev.of_node, &s->hw);
	if (ret)
		goto free_state;
#ifdef CONFIG_MTK_MIR3DA_A25_STOCK_DIRECTION
	/* A25 stock probe overwrites DT=6 with 4 before get_convert:
	 * stock.Image linked VA 0xffffff80086b0e40..0e50.
	 * This is board mounting, not a calibration/physical-pose claim.
	 */
	s->hw.direction = 4;
#endif
	ret = hwmsen_get_convert(s->hw.direction, &s->cvt);
	if (ret)
		goto free_state;
	mutex_lock(&mir_lock);
	if (mir) {
		ret = -EBUSY;
		goto unlock_free;
	}
	ret = mir3da_install_general_ops(&mir_ops);
	if (ret)
		goto unlock_free;
	/* Family detection happens before reset/init writes. Never scan 0x27. */
	ret = mir3da_module_detect(client);
	if (ret) {
		if (ret == -1)
			ret = -ENODEV;
		goto unlock_free;
	}
	core_started = true;
	if (!mir3da_core_init(client)) {
		ret = -EIO;
		goto poweroff;
	}
	ret = mir3da_set_enable(client, 0);
	if (ret)
		goto poweroff;
	mir = s;
	/* Publish only after the framework registration sequence completes.
	 * Callbacks retained by MTK on failed registration are static and
	 * return -ENODEV while ready=false; no freed state can be consumed.
	 */
	mutex_unlock(&mir_lock);
#ifdef CONFIG_MTK_MIR3DA_A25_VIRTUAL_PS
	ret = mir_factory_register();
#else
	ret = accel_factory_device_register(&mir_factory);
#endif
	if (ret)
		goto unpublish;
#ifdef CONFIG_MTK_MIR3DA_A25_VIRTUAL_PS
	ret = a25_ps_register(&mir_ps_ops);
	if (ret)
		goto factory_cleanup;
#endif
	ret = acc_register_data_path(&data);
	if (ret)
		goto factory_cleanup;
	ret = acc_register_control_path(&ctl);
	if (ret)
		goto factory_cleanup;
	mutex_lock(&mir_lock);
	s->ready = true;
	i2c_set_clientdata(client, s);
	mir3da_init_flag = 0;
	mutex_unlock(&mir_lock);
	return 0;

factory_cleanup:
#ifdef CONFIG_MTK_MIR3DA_A25_VIRTUAL_PS
	a25_ps_unregister();
	acc_unregister_a25_path();
#endif
#ifdef CONFIG_MTK_MIR3DA_A25_VIRTUAL_PS
	mir_factory_unregister();
#else
	accel_factory_device_deregister(&mir_factory);
#endif
unpublish:
	mutex_lock(&mir_lock);
	mir = NULL;
poweroff:
	if (core_started && gsensor_mod >= 0)
		mir3da_set_enable(client, 0); /* best effort, preserve original error */
unlock_free:
	mutex_unlock(&mir_lock);
free_state:
#ifdef CONFIG_MTK_MIR3DA_A25_VIRTUAL_PS
	if (s->ps_wake)
		wakeup_source_unregister(s->ps_wake);
#endif
	kfree(s);
	return ret;
}

static int mir3da_remove(struct i2c_client *client)
{
	struct mir3da_state *s = i2c_get_clientdata(client);

	mutex_lock(&mir_lock);
	if (s) {
		s->ready = false;
#ifdef CONFIG_MTK_MIR3DA_A25_VIRTUAL_PS
		hrtimer_cancel(&s->sample_timer);
#endif
		if (mir3da_set_enable(client, 0))
			dev_warn(&client->dev, "power-down failed during remove\n");
#ifndef CONFIG_MTK_MIR3DA_A25_VIRTUAL_PS
		mir = NULL;
#endif
	}
	mir3da_init_flag = -ENODEV;
	i2c_set_clientdata(client, NULL);
	mutex_unlock(&mir_lock);

#ifdef CONFIG_MTK_MIR3DA_A25_VIRTUAL_PS
	if (s) {
		cancel_work_sync(&s->sample_work);
		mir_factory_unregister();
		a25_ps_unregister();
		acc_unregister_a25_path();
		__pm_relax(s->ps_wake);
		wakeup_source_unregister(s->ps_wake);
		mutex_lock(&mir_lock);
		mir = NULL;
		mutex_unlock(&mir_lock);
	}
#endif
#ifndef CONFIG_MTK_MIR3DA_A25_VIRTUAL_PS
	accel_factory_device_deregister(&mir_factory);
#endif
	kfree(s);
	/* I2C core owns the client. Never i2c_unregister_device here. */
	return 0;
}

static int __maybe_unused mir3da_suspend(struct device *dev)
{
	int ret = 0;

	mutex_lock(&mir_lock);
	if (mir && mir->ready) {
#ifdef CONFIG_MTK_MIR3DA_A25_VIRTUAL_PS
		/* PS is a CPU-sampled wake sensor; no hardware wake IRQ exists.
		 * Match stock stay-awake ownership; never silently freeze PS.
		 */
		if (mir->ps_on) {
			mutex_unlock(&mir_lock);
			return -EBUSY;
		}
#endif
		ret = mir3da_set_enable(mir->client, 0);
		if (!ret)
			mir->suspended = true;
#ifdef CONFIG_MTK_MIR3DA_A25_VIRTUAL_PS
		if (!ret) {
			hrtimer_cancel(&mir->sample_timer);
			mir->cache_valid = false;
		}
#endif
	}
	mutex_unlock(&mir_lock);
	return ret;
}
static int __maybe_unused mir3da_resume(struct device *dev)
{
	int ret = 0;

	mutex_lock(&mir_lock);
	if (mir && mir->ready && mir->suspended) {
		/* Retained-supply assumption, no reset of offsets on resume. */
		ret = mir3da_set_enable(mir->client, mir->enabled);
		if (!ret)
			mir->suspended = false;
#ifdef CONFIG_MTK_MIR3DA_A25_VIRTUAL_PS
		if (!ret && mir->enabled) {
			mir->sample_due = 0;
			mir_arm_locked(1);
		}
#endif
	}
	mutex_unlock(&mir_lock);
	return ret;
}
static SIMPLE_DEV_PM_OPS(mir_pm, mir3da_suspend, mir3da_resume);
static const struct of_device_id mir_of_match[] = {
	{ .compatible = "mediatek,gsensor" }, { }
};
static const struct i2c_device_id mir_id[] = {
	{ "mir3da", 0 }, { }
};
static struct i2c_driver mir_driver = {
	.driver = {
		.name = "mir3da",
		.of_match_table = mir_of_match,
		.pm = &mir_pm,
		/* MTK framework has no public unregister API: built-in only. */
		.suppress_bind_attrs = true,
	},
	.probe = mir3da_probe, .remove = mir3da_remove, .id_table = mir_id,
};
static int mir_local_init(void)
{
	int ret = i2c_add_driver(&mir_driver);

	if (ret)
		return ret;
	if (mir3da_init_flag) {
		i2c_del_driver(&mir_driver);
		return mir3da_init_flag;
	}
	return 0;
}
static int mir_local_remove(void)
{
	i2c_del_driver(&mir_driver);
	return 0;
}
static struct acc_init_info mir_init_info = {
	.name = "mir3da", .init = mir_local_init, .uninit = mir_local_remove,
};
static int __init mir_init(void)
{
	return acc_driver_add(&mir_init_info);
}
module_init(mir_init);
MODULE_AUTHOR("MiraMEMS <lschen@miramems.com>");
MODULE_DESCRIPTION("MIR3DA sensors-1.0 gated A25 candidate");
MODULE_LICENSE("GPL");

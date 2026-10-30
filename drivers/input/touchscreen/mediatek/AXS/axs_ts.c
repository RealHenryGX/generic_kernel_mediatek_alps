// SPDX-License-Identifier: GPL-2.0
/* A25 only; requires the LCM power candidate and its single reset owner.
 * Reconstructed protocol, not vendor source. No firmware update/debug bus IO.
 */
#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/of_platform.h>
#include <linux/pinctrl/consumer.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/a25_touch.h>
#include <linux/a25_lcm_power.h>
#include "tpd.h"

#define AXS_POINTS 5
#define AXS_FRAME 32
#define AXS_ADDR 0x3b

struct axs_ts {
	struct i2c_client *client, *wire;
	struct input_dev *input;
	struct pinctrl *pinctrl;
	struct platform_device *pdev;
	struct mutex io_lock;
	int irq;
	bool irq_on, fb_suspended, sys_suspended;
};
/* Lock order: lifecycle -> disable_irq/synchronize -> io_lock.
 * IRQ thread takes ONLY io_lock, never lifecycle. LCM holds lifecycle over
 * its entire sequence (including bias mutex), not just the reset pulse.
 */
static DEFINE_MUTEX(axs_lifecycle);
static struct axs_ts *axs;
static bool panel_ready, panel_seen;

static void axs_release(struct axs_ts *ts)
{
	int i;

	for (i = 0; i < AXS_POINTS; i++) {
		input_mt_slot(ts->input, i);
		input_mt_report_slot_state(ts->input, MT_TOOL_FINGER, false);
	}
	input_mt_sync_frame(ts->input);
	input_mt_report_pointer_emulation(ts->input, true);
	input_sync(ts->input);
}

/* Validate the COMPLETE frame before publishing any event. The stock checks
 * byte0 == 0 and byte1 high nibble == 0, NOT <=0xf / ESD 4 or 8.
 * The bounded full-snapshot policy releases all slots on malformed frames.
 */
static int axs_report(struct axs_ts *ts, const u8 *data)
{
	unsigned int i, n = data[1] & 15, seen = 0, active = 0;
	unsigned int event, id, x, y;
	const u8 *p;

	if (data[0] || (data[1] & 0xf0) || n > AXS_POINTS)
		goto invalid;
	for (i = 0; i < n; i++) {
		p = data + 2 + 6 * i;
		event = p[0] >> 6;
		id = p[2] >> 4;
		x = ((p[0] & 15) << 8) | p[1];
		y = ((p[2] & 15) << 8) | p[3];
		if (id >= AXS_POINTS || event > 2 || (seen & BIT(id)))
			goto invalid;
		if (event != 1 && (x > 384 || y > 854))
			goto invalid;
		seen |= BIT(id);
	}
	for (i = 0; i < n; i++) {
		p = data + 2 + 6 * i;
		id = p[2] >> 4;
		if ((p[0] >> 6) == 1)
			continue;
		active |= BIT(id);
		input_mt_slot(ts->input, id);
		input_mt_report_slot_state(ts->input, MT_TOOL_FINGER, true);
		input_report_abs(ts->input, ABS_MT_POSITION_X,
				 ((p[0] & 15) << 8) | p[1]);
		input_report_abs(ts->input, ABS_MT_POSITION_Y,
				 ((p[2] & 15) << 8) | p[3]);
		input_report_abs(ts->input, ABS_MT_PRESSURE, p[4] ? p[4] : 0x3f);
		input_report_abs(ts->input, ABS_MT_TOUCH_MAJOR,
				 (p[5] >> 4) ? p[5] >> 4 : 9);
	}
	for (i = 0; i < AXS_POINTS; i++) {
		if (!(active & BIT(i))) {
			input_mt_slot(ts->input, i);
			input_mt_report_slot_state(ts->input, MT_TOOL_FINGER, false);
		}
	}
	input_mt_sync_frame(ts->input);
	input_mt_report_pointer_emulation(ts->input, true);
	input_sync(ts->input);
	return 0;
invalid:
	axs_release(ts);
	return -EINVAL;
}

static int axs_version(struct axs_ts *ts, u16 *version)
{
	u8 reg = 0x0c, data[2];
	struct i2c_msg msgs[] = {
		{ .addr = ts->wire->addr, .flags = 0, .len = 1, .buf = &reg },
		{ .addr = ts->wire->addr, .flags = I2C_M_RD, .len = 2, .buf = data },
	};
	int ret = i2c_transfer(ts->wire->adapter, msgs, ARRAY_SIZE(msgs));

	if (ret != ARRAY_SIZE(msgs))
		return ret < 0 ? ret : -EIO;
	*version = (data[0] << 8) | data[1];
	return 0;
}

static irqreturn_t axs_irq_thread(int irq, void *arg)
{
	struct axs_ts *ts = arg;
	u8 frame[AXS_FRAME];
	int ret;

	mutex_lock(&ts->io_lock);
	ret = i2c_master_recv(ts->wire, (char *)frame, sizeof(frame));
	if (ret == sizeof(frame))
		axs_report(ts, frame);
	else
		axs_release(ts); /* no stale contacts after short/failed transfer */
	mutex_unlock(&ts->io_lock);
	return IRQ_HANDLED;
}

/* lifecycle held. disable_irq waits for the threaded handler before release. */
static void axs_quiesce(struct axs_ts *ts)
{
	if (ts->irq_on) {
		disable_irq(ts->irq);
		ts->irq_on = false;
	}
	mutex_lock(&ts->io_lock);
	axs_release(ts);
	mutex_unlock(&ts->io_lock);
}
static void axs_activate(struct axs_ts *ts)
{
	if (panel_ready && !ts->fb_suspended && !ts->sys_suspended && !ts->irq_on) {
		ts->irq_on = true;
		enable_irq(ts->irq);
	}
}

void a25_touch_boot_handoff(void)
{
	mutex_lock(&axs_lifecycle);
	/* LK already initialized the panel; do not pulse reset just to probe.
	 * Never override a failure/suspend already observed from an LCM call.
	 */
	if (!panel_seen) {
		panel_ready = true;
		panel_seen = true;
	}
	mutex_unlock(&axs_lifecycle);
}

void a25_touch_panel_begin(void)
{
	mutex_lock(&axs_lifecycle);
	panel_seen = true;
	panel_ready = false;
	if (axs)
		axs_quiesce(axs);
}
void a25_touch_panel_end(bool ready)
{
	panel_ready = ready;
	if (axs)
		axs_activate(axs);
	mutex_unlock(&axs_lifecycle);
}

static int axs_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
	struct axs_ts *ts;
	struct device_node *np;
	struct pinctrl_state *state;
	u16 version;
	int ret;

	if (!IS_ENABLED(CONFIG_A25_LCM_POWER) || !client->dev.of_node ||
	    (client->flags & I2C_CLIENT_TEN) ||
	    (client->addr != 0x38 && client->addr != AXS_ADDR))
		return -ENODEV;
	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
		return -EOPNOTSUPP;
	mutex_lock(&axs_lifecycle);
	ret = -EBUSY;
	if (axs)
		goto unlock;
	ret = -EPROBE_DEFER;
	if (!panel_ready)
		goto unlock;
	ts = kzalloc(sizeof(*ts), GFP_KERNEL);
	ret = -ENOMEM;
	if (!ts)
		goto unlock;
	ts->client = client;
	mutex_init(&ts->io_lock);
	/* Keep the DT client at 0x38. Reserve the proven wire address with a real
	 * core-owned dummy client; do not fabricate a client or silently mutate
	 * core address accounting like stock. No address probing is performed.
	 */
	ts->wire = client;
	if (client->addr != AXS_ADDR) {
		ts->wire = i2c_new_dummy(client->adapter, AXS_ADDR);
		if (!ts->wire) {
			ret = -EBUSY; /* 4.19 i2c_new_dummy returns NULL, not ERR_PTR */
			goto free_ts;
		}
	}
	np = of_find_compatible_node(NULL, NULL, "goodix,touch");
	ret = -ENODEV;
	if (!np)
		goto free_wire;
	if (!of_device_is_available(np)) {
		of_node_put(np);
		goto free_wire;
	}
	ts->pdev = of_find_device_by_node(np);
	ts->irq = of_irq_get(np, 0);
	of_node_put(np);
	if (!ts->pdev) {
		ret = -EPROBE_DEFER;
		goto free_wire;
	}
	if (ts->irq <= 0) {
		ret = ts->irq ? ts->irq : -EINVAL;
		goto put_dev;
	}
	/* GPIO0 is EINT, GPIO174 is reset. Never request/output the IRQ as reset. */
	if (irq_get_trigger_type(ts->irq) != IRQ_TYPE_EDGE_FALLING) {
		ret = -EINVAL;
		goto put_dev;
	}
	ts->pinctrl = pinctrl_get(&ts->pdev->dev);
	if (IS_ERR(ts->pinctrl)) {
		ret = PTR_ERR(ts->pinctrl);
		goto put_dev;
	}
	state = pinctrl_lookup_state(ts->pinctrl, "state_eint_as_int");
	if (IS_ERR(state)) {
		ret = PTR_ERR(state);
		goto put_pin;
	}
	ret = pinctrl_select_state(ts->pinctrl, state);
	if (ret)
		goto put_pin;
	/* Stock probe: RST high/100ms, then axs_reset_proc(20), BEFORE 0x0c.
	 * LK panel handoff does not prove touch application readiness. Use the
	 * same TPD resource owner under lifecycle; never pulse IRQ as reset.
	 * This is probe-only: resume keeps the existing LCM-owned sequence.
	 */
	ret = tpd_a25_probe_reset();
	if (ret)
		goto put_pin;
	ret = axs_version(ts, &version);
	if (ret)
		goto put_pin;
	/* Stock auto-upgrade compares !=, not <, against embedded version 7.
	 * Do not hide that missing feature behind a default-off probe gate or
	 * silently flash an unvalidated image. Zero is also not an app-health
	 * certificate, though the stock upgrade decision skips it.
	 */
	if (version != 0x0007)
		dev_warn(&client->dev,
			 "AXS fw=0x%04x, stock blob=0x0007; auto-upgrade not implemented\n",
			 version);
	ts->input = input_allocate_device();
	ret = -ENOMEM;
	if (!ts->input)
		goto put_pin;
	ts->input->name = "axs_ts";
	ts->input->id.bustype = BUS_I2C;
	ts->input->dev.parent = &client->dev;
	input_set_capability(ts->input, EV_KEY, BTN_TOUCH);
	input_set_abs_params(ts->input, ABS_MT_POSITION_X, 0, 384, 0, 0);
	input_set_abs_params(ts->input, ABS_MT_POSITION_Y, 0, 854, 0, 0);
	input_set_abs_params(ts->input, ABS_MT_PRESSURE, 0, 255, 0, 0);
	input_set_abs_params(ts->input, ABS_MT_TOUCH_MAJOR, 0, 15, 0, 0);
	ret = input_mt_init_slots(ts->input, AXS_POINTS, INPUT_MT_DIRECT);
	if (ret)
		goto free_input;
	ret = input_register_device(ts->input);
	if (ret)
		goto free_input;
	/* Handler is safe immediately: wire/input are complete and registered. */
	ret = request_threaded_irq(ts->irq, NULL, axs_irq_thread,
			IRQF_TRIGGER_FALLING | IRQF_ONESHOT, "axs_ts", ts);
	if (ret)
		goto unregister_input;
	ts->irq_on = true;
	i2c_set_clientdata(client, ts);
	axs = ts;
	tpd_load_status = 1;
	dev_info(&client->dev, "AXS wire=0x3b fw=0x%04x irq=%d\n", version, ts->irq);
	mutex_unlock(&axs_lifecycle);
	return 0;
unregister_input:
	input_unregister_device(ts->input);
	goto put_pin;
free_input:
	input_free_device(ts->input);
put_pin:
	pinctrl_put(ts->pinctrl);
put_dev:
	put_device(&ts->pdev->dev);
free_wire:
	if (ts->wire != client)
		i2c_unregister_device(ts->wire);
free_ts:
	kfree(ts);
unlock:
	mutex_unlock(&axs_lifecycle);
	return ret;
}

static int axs_remove(struct i2c_client *client)
{
	struct axs_ts *ts = i2c_get_clientdata(client);

	mutex_lock(&axs_lifecycle);
	axs_quiesce(ts);
	free_irq(ts->irq, ts);
	axs = NULL;
	tpd_load_status = 0;
	i2c_set_clientdata(client, NULL);
	input_unregister_device(ts->input);
	pinctrl_put(ts->pinctrl);
	put_device(&ts->pdev->dev);
	if (ts->wire != client)
		i2c_unregister_device(ts->wire);
	kfree(ts);
	mutex_unlock(&axs_lifecycle);
	return 0;
}

static void axs_fb_suspend(struct device *unused)
{
	mutex_lock(&axs_lifecycle);
	if (axs) {
		axs->fb_suspended = true;
		axs_quiesce(axs);
	}
	mutex_unlock(&axs_lifecycle);
}
static void axs_fb_resume(struct device *unused)
{
	mutex_lock(&axs_lifecycle);
	if (axs) {
		axs->fb_suspended = false;
		axs_activate(axs);
	}
	mutex_unlock(&axs_lifecycle);
}
static int axs_pm_suspend(struct device *dev)
{
	struct axs_ts *ts = i2c_get_clientdata(to_i2c_client(dev));

	mutex_lock(&axs_lifecycle);
	ts->sys_suspended = true;
	axs_quiesce(ts);
	mutex_unlock(&axs_lifecycle);
	return 0;
}
static int axs_pm_resume(struct device *dev)
{
	struct axs_ts *ts = i2c_get_clientdata(to_i2c_client(dev));

	mutex_lock(&axs_lifecycle);
	ts->sys_suspended = false;
	axs_activate(ts);
	mutex_unlock(&axs_lifecycle);
	return 0;
}
static const struct dev_pm_ops axs_pm = {
	.suspend = axs_pm_suspend, .resume = axs_pm_resume,
	.freeze = axs_pm_suspend, .thaw = axs_pm_resume,
	.poweroff = axs_pm_suspend, .restore = axs_pm_resume,
};
static const struct of_device_id axs_of_match[] = {
	{ .compatible = "mediatek,cap_touch" }, { }
};
static const struct i2c_device_id axs_ids[] = { { "axs_ts", 0 }, { } };
MODULE_DEVICE_TABLE(of, axs_of_match);
MODULE_DEVICE_TABLE(i2c, axs_ids);
static struct i2c_driver axs_driver = {
	.driver = { .name = "axs_ts", .of_match_table = axs_of_match,
		.pm = &axs_pm, .suppress_bind_attrs = true },
	.probe = axs_probe, .remove = axs_remove, .id_table = axs_ids,
};
static int axs_local_init(void)
{
	int ret;

	/* TPD runs local_init synchronously. Failed/deferred probes must NOT
	 * leave a registered I2C driver behind when TPD releases its context.
	 */
	ret = i2c_add_driver(&axs_driver);
	if (ret)
		return ret;
	if (!tpd_load_status) {
		i2c_del_driver(&axs_driver);
		return -EPROBE_DEFER;
	}
	return 0;
}
static struct tpd_driver_t axs_tpd = {
	.tpd_device_name = "axs_ts", .tpd_local_init = axs_local_init,
	.suspend = axs_fb_suspend, .resume = axs_fb_resume,
};
static int __init axs_init(void)
{
	if (!IS_ENABLED(CONFIG_A25_LCM_POWER))
		return -ENODEV;
	tpd_get_dts_info();
	return tpd_driver_add(&axs_tpd);
}
/* Built-in-only: the target TPD remove-list implementation is unsafe for
 * unload and LCM directly calls our lifecycle API. No module_exit offered.
 */
module_init(axs_init);
MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("A25 AXS touch with coordinated LCM lifecycle");

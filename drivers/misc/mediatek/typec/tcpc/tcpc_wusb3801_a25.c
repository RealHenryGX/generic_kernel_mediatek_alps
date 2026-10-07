// SPDX-License-Identifier: GPL-2.0-only
/* Register/attach definitions: Copyright (C) 2017 MediaTek Inc.
 * A25 offline-reviewed candidate; NOT hardware-qualified.
 * Architecture: controller owns attach state, TCPC supplies notifications.
 * Based on the MediaTek/realme GPLv2 WUSB3801 register/attach definitions.
 */
#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/of_gpio.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include "inc/tcpci.h"
#include "wusb3801_a25_core.h"

static bool hardware_reviewed;
module_param(hardware_reviewed, bool, 0400);
MODULE_PARM_DESC(hardware_reviewed,
 "Explicit board sink-only review gate; default false; never permits OTG");

struct wa_chip {
 struct i2c_client *client;
 struct tcpc_device *tcpc;
 struct tcpc_desc desc;
 struct mutex lock;
 struct delayed_work poll;
 struct wa_io io;
 struct wa_state state;
 int irq;
 bool ready;
 bool stopping;
 bool suspended; /* Independent PM IRQ-disable reference, under lock. */
 bool quarantined;
 bool irq_paused;
};
static int wa_read(void *ctx, unsigned char reg)
{
 struct wa_chip *c = ctx;
 return i2c_smbus_read_byte_data(c->client, reg);
}
static int wa_write(void *ctx, unsigned char reg, unsigned char value)
{
 struct wa_chip *c = ctx;
 /* Defense in depth: only sink-only CONTROL0 writes are permitted. */
 if (reg != WA_CTRL || (value != WA_SINK_ACTIVE && value != WA_SINK_MASKED))
  return -EPERM;
 return i2c_smbus_write_byte_data(c->client, reg, value);
}
static int wa_init(struct tcpc_device *t, bool reset)
{
 /* Reject generic TCPC software state-machine ownership, including late sync.
  * Hardware initialization is performed and checked by probe, not faked here.
  */
 return -EOPNOTSUPP;
}
static int wa_get_power(struct tcpc_device *t, uint16_t *p)
{
 struct wa_chip *c = tcpc_get_dev_data(t);
 int ret;
 *p = 0;
 mutex_lock(&c->lock);
 ret = c->ready && !c->stopping && !c->suspended && !c->quarantined && c->state.valid ? 0 : -EAGAIN;
 if (!ret && c->state.vbus) *p = TCPC_REG_POWER_STATUS_VBUS_PRES;
 mutex_unlock(&c->lock);
 return ret;
}
static int wa_get_cc(struct tcpc_device *t, int *cc1, int *cc2)
{
 struct wa_chip *c = tcpc_get_dev_data(t);
 int ret, cc;
 *cc1 = *cc2 = TYPEC_CC_VOLT_OPEN;
 mutex_lock(&c->lock);
 ret = c->ready && !c->stopping && !c->suspended && !c->quarantined && c->state.valid ? 0 : -EAGAIN;
 if (!ret && c->state.attached) {
  cc = c->state.rp == 3 ? TYPEC_CC_VOLT_SNK_3_0 :
       c->state.rp == 2 ? TYPEC_CC_VOLT_SNK_1_5 : TYPEC_CC_VOLT_SNK_DFT;
  if (c->state.cc == 1) *cc1 = cc; else *cc2 = cc;
 }
 mutex_unlock(&c->lock);
 return ret;
}
static int wa_set_cc(struct tcpc_device *t, int pull)
{
 struct wa_chip *c = tcpc_get_dev_data(t);
 int ret;
 if (TYPEC_CC_PULL_GET_RES(pull) != TYPEC_CC_RD) return -EPERM;
 mutex_lock(&c->lock);
 ret = c->ready && !c->stopping && !c->suspended && !c->quarantined ? wa_program(&c->io, 1) : -EAGAIN;
 mutex_unlock(&c->lock);
 return ret;
}
static int wa_unsupported(struct tcpc_device *t) { return -EOPNOTSUPP; }
static int wa_u32_get(struct tcpc_device *t, uint32_t *v)
{ *v = 0; return -EOPNOTSUPP; }
static int wa_u8_get(struct tcpc_device *t, uint8_t *v)
{ *v = 0; return -EOPNOTSUPP; }
static int wa_u32_set(struct tcpc_device *t, uint32_t v) { return -EOPNOTSUPP; }
static int wa_u8_set(struct tcpc_device *t, uint8_t v) { return -EOPNOTSUPP; }
static int wa_int_set(struct tcpc_device *t, int v) { return -EOPNOTSUPP; }
static int wa_bool_set(struct tcpc_device *t, bool v) { return -EOPNOTSUPP; }
static struct tcpc_ops wa_ops = {
 .a25_sink_only = true,
 .init = wa_init, .init_alert_mask = wa_unsupported,
 .alert_status_clear = wa_u32_set, .fault_status_clear = wa_u8_set,
 .set_alert_mask = wa_u32_set, .get_alert_mask = wa_u32_get,
 .get_alert_status = wa_u32_get, .get_fault_status = wa_u8_get,
 .get_power_status = wa_get_power, .get_cc = wa_get_cc, .set_cc = wa_set_cc,
 .set_polarity = wa_int_set, .set_low_rp_duty = wa_bool_set,
 .set_vconn = wa_int_set, .deinit = wa_unsupported,
};
/* Caller holds typec_lock then chip lock. No sleeping-I2C in hard IRQ. */
static int wa_publish(struct wa_chip *c, int attached)
{
 struct tcpc_device *t = c->tcpc;
 int ret;
 t->typec_polarity = attached && c->state.cc == 2;
 t->typec_remote_rp_level = c->state.rp == 3 ? TYPEC_CC_VOLT_SNK_3_0 :
  c->state.rp == 2 ? TYPEC_CC_VOLT_SNK_1_5 : TYPEC_CC_VOLT_SNK_DFT;
 t->typec_attach_new = attached ? TYPEC_ATTACHED_SNK : TYPEC_UNATTACHED;
 /* Same-state publications refresh the complete late-subscriber snapshot.
  * The A25 notification layer suppresses broadcasts without an edge. */
 ret = tcpci_notify_typec_state(t);
 if (ret < 0) return ret;
 t->typec_attach_old = t->typec_attach_new;
 return 0;
}
static int wa_process(struct wa_chip *c)
{
 int ret = -EAGAIN, irq = 0, mask_ret;
 if (!READ_ONCE(c->ready)) return ret;
 tcpci_lock_typec(c->tcpc);
 mutex_lock(&c->lock);
 if (c->stopping || c->suspended || !c->ready) goto out;
 ret = wa_sample(&c->io, &c->state, &irq);
 if (ret == -EPERM || ret == -EOPNOTSUPP) {
  c->quarantined = true;
  mask_ret = wa_program(&c->io, 0);
  dev_err_ratelimited(&c->client->dev,
   "unsupported/source peer quarantined: %d; mask/readback=%d\n", ret, mask_ret);
 }
 if (ret < 0 && !c->irq_paused) {
  disable_irq_nosync(c->irq);
  c->irq_paused = true;
 }
 if (!ret && !c->quarantined && c->irq_paused) {
  c->irq_paused = false;
  enable_irq(c->irq);
 }
 /* Detach first if both edge bits are set, then reconcile current status.
  * Periodic reconciliation recovers lost read-to-clear IRQs / I2C failures.
  */
 if ((irq & 2) || ret < 0 || c->quarantined) {
  if (wa_publish(c, 0) < 0)
   dev_err_ratelimited(&c->client->dev, "detach notification failed\n");
 }
 if (!ret && !c->quarantined && wa_publish(c, c->state.attached) < 0)
  dev_err_ratelimited(&c->client->dev, "attach notification failed\n");
out:
 mutex_unlock(&c->lock);
 tcpci_unlock_typec(c->tcpc);
 return ret;
}
static irqreturn_t wa_irq(int irq, void *data)
{
 struct wa_chip *c = data;
 wa_process(c);
 return IRQ_HANDLED;
}
static void wa_poll(struct work_struct *work)
{
 struct wa_chip *c = container_of(to_delayed_work(work), struct wa_chip, poll);
 wa_process(c);
 mutex_lock(&c->lock);
 if (c->ready && !c->stopping && !c->suspended)
  schedule_delayed_work(&c->poll, msecs_to_jiffies(1000));
 mutex_unlock(&c->lock);
}
static int wa_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
 struct device_node *np = client->dev.of_node;
 struct wa_chip *c;
 u32 mode, host_current;
 int ret, gpio;
 if (!np || !of_device_is_compatible(np, "mediatek,usb_type_c")) return -ENODEV;
 ret = of_property_read_u32(np, "wusb3801,init-mode", &mode);
 if (ret) return ret;
 ret = of_property_read_u32(np, "wusb3801,host-current", &host_current);
 if (ret) return ret;
 ret = wa_binding(client->adapter->nr, client->addr, mode, host_current, hardware_reviewed);
 if (ret) return ret; /* BEFORE any I2C/GPIO/register access */
 if (!i2c_check_functionality(client->adapter, I2C_FUNC_SMBUS_BYTE_DATA)) return -EOPNOTSUPP;
 gpio = of_get_named_gpio(np, "wusb3801,irq-gpio", 0);
 if (gpio < 0) return gpio; /* includes -EPROBE_DEFER */
 ret = devm_gpio_request_one(&client->dev, gpio, GPIOF_IN, "wusb3801-a25");
 if (ret) return ret;
 c = devm_kzalloc(&client->dev, sizeof(*c), GFP_KERNEL);
 if (!c) return -ENOMEM;
 c->irq = gpio_to_irq(gpio);
 if (c->irq < 0) return c->irq;
 c->client = client;
 c->io.ctx = c; c->io.read = wa_read; c->io.write = wa_write;
 mutex_init(&c->lock); INIT_DELAYED_WORK(&c->poll, wa_poll);
 i2c_set_clientdata(client, c);
 ret = wa_start(&c->io);
 if (ret) { i2c_set_clientdata(client, NULL); return ret; }
 ret = devm_request_threaded_irq(&client->dev, c->irq, NULL, wa_irq,
  IRQF_ONESHOT | IRQF_TRIGGER_LOW, "wusb3801-a25", c);
 if (ret) { i2c_set_clientdata(client, NULL); return ret; }
 /* Hold a probe-owned IRQ-disable reference until publication is complete. */
 disable_irq(c->irq);
 /* Complete all fallible hardware activation before publishing TCPC. */
 ret = wa_program(&c->io, 1);
 if (ret) goto mask_fail;
 c->desc.name = "type_c_port0";
 c->desc.role_def = TYPEC_ROLE_SNK; c->desc.rp_lvl = TYPEC_RP_DFT;
 c->desc.notifier_supply_num = 0;
 c->tcpc = tcpc_device_register(&client->dev, &c->desc, &wa_ops, c);
 if (IS_ERR_OR_NULL(c->tcpc)) {
  ret = c->tcpc ? PTR_ERR(c->tcpc) : -ENOMEM;
  goto mask_fail;
 }
 c->tcpc->tcpc_flags |= TCPC_FLAGS_A25_SINK_ONLY;
 c->tcpc->typec_role = c->tcpc->typec_role_new = TYPEC_ROLE_SNK;
 WRITE_ONCE(c->ready, true);
 enable_irq(c->irq);
 schedule_delayed_work(&c->poll, 0);
 dev_warn(&client->dev, "UNQUALIFIED sink-only candidate; OTG/PD/accessories/wakeup/hibernation disabled\n");
 return 0;
mask_fail:
 /* Probe owns the disabled IRQ until devres releases it. */
 {
  int safe = wa_program(&c->io, 0);
  dev_err(&client->dev, "probe failed=%d, sink-mask=%d\n", ret, safe);
 }
 i2c_set_clientdata(client, NULL);
 return ret;
}
static void wa_shutdown(struct i2c_client *client)
{
 struct wa_chip *c = i2c_get_clientdata(client);
 int ret;
 if (!c || !READ_ONCE(c->ready)) return;
 mutex_lock(&c->lock);
 c->stopping = true;
 mutex_unlock(&c->lock);
 disable_irq(c->irq); cancel_delayed_work_sync(&c->poll);
 tcpci_lock_typec(c->tcpc); mutex_lock(&c->lock);
 ret = wa_program(&c->io, 0);
 if (ret) dev_err(&client->dev, "shutdown sink-mask failed: %d\n", ret);
 c->ready = false;
 wa_publish(c, 0);
 mutex_unlock(&c->lock); tcpci_unlock_typec(c->tcpc);
 flush_workqueue(c->tcpc->evt_wq);
}
/* Non-wakeup suspend: leave sink hardware untouched and prevent bus traffic.
 * No claim of retained hardware configuration or real board wake support. */
static int wa_suspend(struct device *dev)
{
 struct wa_chip *c = i2c_get_clientdata(to_i2c_client(dev));
 if (!c) return -ENODEV;
 mutex_lock(&c->lock);
 if (!c->ready || c->stopping) {
  mutex_unlock(&c->lock); return -ENODEV;
 }
 if (c->suspended) { mutex_unlock(&c->lock); return 0; }
 c->suspended = true;
 mutex_unlock(&c->lock);
 /* Do not hold a lock required by the threaded IRQ/worker while waiting. */
 disable_irq(c->irq);
 cancel_delayed_work_sync(&c->poll);
 flush_workqueue(c->tcpc->evt_wq);
 return 0;
}
static int wa_resume(struct device *dev)
{
 struct wa_chip *c = i2c_get_clientdata(to_i2c_client(dev));
 int ret, expected;
 if (!c) return -ENODEV;
 tcpci_lock_typec(c->tcpc);
 mutex_lock(&c->lock);
 if (!c->ready || c->stopping) {
  mutex_unlock(&c->lock); tcpci_unlock_typec(c->tcpc); return -ENODEV;
 }
 if (!c->suspended) { mutex_unlock(&c->lock); tcpci_unlock_typec(c->tcpc); return 0; }
 /* Read a known register only; no guessed reinit after power loss. */
 expected = c->quarantined ? WA_SINK_MASKED : WA_SINK_ACTIVE;
 ret = wa_read(c, WA_CTRL);
 c->state.valid = 0;
 if (ret < 0 || ret != expected) {
  if (wa_publish(c, 0) < 0)
   dev_err(&c->client->dev, "resume detach notification failed\n");
  mutex_unlock(&c->lock);
  tcpci_unlock_typec(c->tcpc);
  return ret < 0 ? ret : -EIO; /* remain bus/IRQ quiescent */
 }
 c->suspended = false;
 /* Poll before returning; the PM IRQ-disable reference still prevents IRQs.
  * A transport/status error uses the normal bounded polling retry path. */
 mutex_unlock(&c->lock);
 tcpci_unlock_typec(c->tcpc);
 ret = wa_process(c);
 mutex_lock(&c->lock);
 if (!c->stopping) schedule_delayed_work(&c->poll, msecs_to_jiffies(1000));
 enable_irq(c->irq); /* Only the PM reference; error pause is independent. */
 mutex_unlock(&c->lock);
 return ret;
}
static int wa_hibernate(struct device *dev) { return -EOPNOTSUPP; }
static const struct dev_pm_ops wa_pm = {
 .suspend = wa_suspend, .resume = wa_resume,
 .freeze = wa_hibernate, .poweroff = wa_hibernate,
};
static const struct of_device_id wa_of[] = {
 { .compatible = "mediatek,usb_type_c" }, { }
};
MODULE_DEVICE_TABLE(of, wa_of);
static const struct i2c_device_id wa_ids[] = { { "usb_type_c", 0 }, { } };
MODULE_DEVICE_TABLE(i2c, wa_ids);
static struct i2c_driver wa_driver = {
 .driver = { .name = "usb_type_c", .of_match_table = wa_of,
  .pm = &wa_pm, .suppress_bind_attrs = true },
 .probe = wa_probe, .shutdown = wa_shutdown, .id_table = wa_ids,
};
static int __init wa_driver_init(void) { return i2c_add_driver(&wa_driver); }
device_initcall(wa_driver_init);
MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("A25 WUSB3801 sink-only gated candidate");

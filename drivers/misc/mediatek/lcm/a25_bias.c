// SPDX-License-Identifier: GPL-2.0
/* A25 only: DT owns enumeration; never create a second client or change addr. */
#include <linux/errno.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/a25_lcm_power.h>

static DEFINE_MUTEX(a25_bias_lock);
static struct i2c_client *a25_bias_client;

int a25_bias_begin(bool need_client)
{
	mutex_lock(&a25_bias_lock);
	if (need_client && !a25_bias_client) {
		mutex_unlock(&a25_bias_lock);
		return -EPROBE_DEFER;
	}
	return 0;
}

void a25_bias_end(void)
{
	mutex_unlock(&a25_bias_lock);
}

int a25_bias_ready(void)
{
	int ret = a25_bias_begin(true);

	if (!ret)
		a25_bias_end();
	return ret;
}

int a25_bias_write_locked(unsigned char reg, unsigned char value)
{
	char data[2] = { reg, value };
	int ret;

	/* Do not expose a general-purpose voltage programmer. Stock bytes only. */
	if (reg > 1 || value != 0x12)
		return -EINVAL;
	if (!a25_bias_client)
		return -ENODEV;
	ret = i2c_master_send(a25_bias_client, data, sizeof(data));
	return ret == sizeof(data) ? 0 : (ret < 0 ? ret : -EIO);
}

static int a25_bias_probe(struct i2c_client *client,
			  const struct i2c_device_id *id)
{
	int ret = 0;

	if (!client->dev.of_node || client->addr != 0x3e ||
	    (client->flags & I2C_CLIENT_TEN))
		return -ENODEV;
	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
		return -EOPNOTSUPP;
	mutex_lock(&a25_bias_lock);
	if (a25_bias_client)
		ret = -EBUSY;
	else
		a25_bias_client = client;
	mutex_unlock(&a25_bias_lock);
	return ret;
}

static int a25_bias_remove(struct i2c_client *client)
{
	/* Serialize lifetime against the ENTIRE LCM sequence, not just each write. */
	mutex_lock(&a25_bias_lock);
	if (a25_bias_client == client)
		a25_bias_client = NULL;
	mutex_unlock(&a25_bias_lock);
	/* DT/I2C core owns this client. Never i2c_unregister_device() here. */
	return 0;
}

static const struct of_device_id a25_bias_of_match[] = {
	{ .compatible = "mediatek,I2C_LCD_BIAS" },
	{ }
};
static const struct i2c_device_id a25_bias_ids[] = {
	{ "I2C_LCD_BIAS", 0 },
	{ }
};
static struct i2c_driver a25_bias_driver = {
	.driver = {
		.name = "I2C_LCD_BIAS",
		.of_match_table = a25_bias_of_match,
		.suppress_bind_attrs = true,
	},
	.probe = a25_bias_probe,
	.remove = a25_bias_remove,
	.id_table = a25_bias_ids,
};

static int __init a25_bias_init(void)
{
	return i2c_add_driver(&a25_bias_driver);
}
/* A built-in supplier registered early; mtkfb still defers if adapter is late. */
subsys_initcall(a25_bias_init);
MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("A25 stock-byte LCD bias supplier");

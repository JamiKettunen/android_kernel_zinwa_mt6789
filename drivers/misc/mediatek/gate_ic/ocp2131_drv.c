// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2021 MediaTek Inc.
 */

#include <linux/gpio/consumer.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/of_gpio.h>

#include "gate_i2c.h"

#define OCP2131_VOL_UINT (100) //100mV
#define OCP2131_VOL_MIN_LEVEL (4000) //4000mV
#define OCP2131_VOL_MAX_LEVEL (6500) //6500mV
#define OCP2131_VOL_REG_VALUE(level) ((level - OCP2131_VOL_MIN_LEVEL) / OCP2131_VOL_UINT)
#define OCP2131_VOL_MAX_BRIGHT_LEVEL ((OCP2131_VOL_MAX_LEVEL - OCP2131_VOL_MIN_LEVEL)/OCP2131_VOL_UINT)

/*****************************************************************************
 * Define
 *****************************************************************************/
#define GATE_I2C_ID_NAME "gate_ic_i2c_ocp2131"

#undef pr_fmt
#define pr_fmt(fmt) KBUILD_MODNAME " %s(%d) :[GATE][I2C] " fmt, __func__, __LINE__

/*****************************************************************************
 * GLobal Variable
 *****************************************************************************/
static const struct of_device_id _gate_ic_i2c_of_match[] = {
	{
		.compatible = "mediatek,mtk-gateic-drv-ocp2131",
	 },
	{}
};

static struct i2c_client *_gate_ic_i2c_client;

struct gate_ic_client {
	struct i2c_client *i2c_client;
	struct gpio_desc *pinctrl;
	struct device *dev;
	atomic_t gate_ic_power_status;
	int last_brightness;
};

/*****************************************************************************
 * Extern Area
 *****************************************************************************/

static int ocp2131_set_voltage(unsigned int level)
{
	int ret = 0;

	if (level < OCP2131_VOL_MIN_LEVEL || level > OCP2131_VOL_MAX_LEVEL) {
		pr_err("%s invalid voltage level:%d\n", __func__, level);
		return -3;
	}

	pr_info("%s++ level:%d, id:0x%x\n",
		__func__, level, OCP2131_VOL_REG_VALUE(level));
	ret = _gate_ic_i2c_write_bytes(0, OCP2131_VOL_REG_VALUE(level));
	if (ret < 0)
		return ret;
	ret = _gate_ic_i2c_write_bytes(1, OCP2131_VOL_REG_VALUE(level));

	return ret;
}

int _gate_ic_backlight_set(unsigned int brightness)
{
	struct gate_ic_client *gate_client = i2c_get_clientdata(_gate_ic_i2c_client);
	
	if (gate_client == NULL) {
		return -1;
	}
	
	pr_notice("%s,brightness(last_brightness) =%d(%d),\n", __func__,brightness,gate_client->last_brightness);
	
	if (gate_client->last_brightness == 0 && brightness != 0) {
        gate_client->last_brightness = brightness;
        mdelay(300);
        _gate_ic_Power_on();
    }
    
    if (gate_client->last_brightness != 0 && brightness == 0) {
        gate_client->last_brightness = brightness;
        _gate_ic_Power_off();
    }
    
    if (brightness > 0) {
		gate_client->last_brightness = brightness;
		brightness = ((brightness <= OCP2131_VOL_MAX_BRIGHT_LEVEL) ? brightness : OCP2131_VOL_MAX_BRIGHT_LEVEL);
		
		ocp2131_set_voltage(OCP2131_VOL_MIN_LEVEL + brightness * OCP2131_VOL_UINT);
	}
	return 0;
}
EXPORT_SYMBOL_GPL(_gate_ic_backlight_set);

int _gate_ic_i2c_read_bytes(unsigned char addr, unsigned char *returnData)
{
	char cmd_buf[2] = { 0x00, 0x00 };
	char readData = 0;
	int ret = 0;
	struct i2c_client *client = _gate_ic_i2c_client;

	if (client == NULL) {
		pr_info("ERROR!! _gate_ic_i2c_client is null\n");
		return 0;
	}

	cmd_buf[0] = addr;
	ret = i2c_master_send(client, &cmd_buf[0], 1);
	ret = i2c_master_recv(client, &cmd_buf[1], 1);
	if (ret < 0)
		pr_info("ERROR %d!! i2c read data 0x%0x fail !!\n", ret, addr);

	readData = cmd_buf[1];
	*returnData = readData;

	return ret;
}
EXPORT_SYMBOL_GPL(_gate_ic_i2c_read_bytes);

int _gate_ic_i2c_write_bytes(unsigned char addr, unsigned char value)
{
	int ret = 0;
	struct i2c_client *client = _gate_ic_i2c_client;
	char write_data[2] = { 0 };

	if (client == NULL) {
		pr_info("ERROR!! _gate_ic_i2c_client is null\n");
		return 0;
	}

	write_data[0] = addr;
	write_data[1] = value;
	ret = i2c_master_send(client, write_data, 2);
	if (ret < 0)
		pr_info("ERROR %d!! i2c write data fail 0x%0x, 0x%0x !!\n",
				ret, addr, value);

	return ret;
}
EXPORT_SYMBOL_GPL(_gate_ic_i2c_write_bytes);

void _gate_ic_Power_on(void)
{
	struct gate_ic_client *gate_client = i2c_get_clientdata(_gate_ic_i2c_client);

	pr_info("%s+: status = (%d)\n", __func__,
		atomic_read(&gate_client->gate_ic_power_status));

	if (IS_ERR(gate_client->pinctrl)) {
		pr_info("ERROR!! pinctrl is error!\n");
	} else if (!atomic_read(&gate_client->gate_ic_power_status)) {
		gate_client->pinctrl = devm_gpiod_get(gate_client->dev, "gate-power",
				   GPIOD_OUT_HIGH);
		if (IS_ERR(gate_client->pinctrl)) {
			pr_info("ERROR!! Failed to get gpio: %d\n",
				PTR_ERR(gate_client->pinctrl));
			return;
		}
		gpiod_set_value(gate_client->pinctrl, 1);
		devm_gpiod_put(gate_client->dev, gate_client->pinctrl);

		atomic_set(&gate_client->gate_ic_power_status, 1);
	}
}
EXPORT_SYMBOL_GPL(_gate_ic_Power_on);

void _gate_ic_Power_off(void)
{
	struct gate_ic_client *gate_client = i2c_get_clientdata(_gate_ic_i2c_client);

	pr_info("%s+: status = (%d)\n", __func__,
		atomic_read(&gate_client->gate_ic_power_status));

	if (IS_ERR(gate_client->pinctrl)) {
		pr_info("ERROR!! pinctrl is error!\n");
	} else if (atomic_read(&gate_client->gate_ic_power_status)) {
		gate_client->pinctrl = devm_gpiod_get(gate_client->dev, "gate-power",
				   GPIOD_OUT_HIGH);
		if (IS_ERR(gate_client->pinctrl)) {
			pr_info("ERROR!! Failed to get gpio: %d\n",
				PTR_ERR(gate_client->pinctrl));
			return;
		}
		gpiod_set_value(gate_client->pinctrl, 0);
		devm_gpiod_put(gate_client->dev, gate_client->pinctrl);

		atomic_set(&gate_client->gate_ic_power_status, 0);
	}
}
EXPORT_SYMBOL_GPL(_gate_ic_Power_off);

void _gate_ic_i2c_panel_bias_enable(unsigned int power_status)
{
	pr_info("%s+\n", __func__);
}
EXPORT_SYMBOL_GPL(_gate_ic_i2c_panel_bias_enable);

/*****************************************************************************
 * Function
 *****************************************************************************/

static int _gate_ic_i2c_probe(struct i2c_client *client,
	const struct i2c_device_id *id)
{
	struct gate_ic_client *gate_client;
	int status;

	pr_info("%s+: client name=%s addr=0x%x\n",
		__func__, client->name, client->addr);

	gate_client = devm_kzalloc(&client->dev, sizeof(struct gate_ic_client), GFP_KERNEL);

	if (!gate_client)
		return -ENOMEM;

	gate_client->dev = &client->dev;
	gate_client->i2c_client = client;
	gate_client->pinctrl = devm_gpiod_get(&client->dev, "gate-power",
				   GPIOD_OUT_HIGH);
	if (IS_ERR(gate_client->pinctrl)) {
		status = PTR_ERR(gate_client->pinctrl);
		pr_info("ERROR!! Failed to enable gpio: %d\n", status);
		return status;
	}
	devm_gpiod_put(gate_client->dev, gate_client->pinctrl);
	i2c_set_clientdata(client, gate_client);
	_gate_ic_i2c_client = client;
	atomic_set(&gate_client->gate_ic_power_status, 1);
	atomic_set(&gate_client->gate_ic_power_status, 1);


	return 0;
}

static int _gate_ic_i2c_remove(struct i2c_client *client)
{
	struct gate_ic_client *gate_client;

	pr_info("%s+\n", __func__);

	gate_client = i2c_get_clientdata(client);

	i2c_unregister_device(client);

	kfree(gate_client);
	gate_client = NULL;
	_gate_ic_i2c_client = NULL;
	i2c_unregister_device(client);
	return 0;
}

/*****************************************************************************
 * Data Structure
 *****************************************************************************/

static const struct i2c_device_id _gate_ic_i2c_id[] = {
	{GATE_I2C_ID_NAME, 0},
	{}
};

static struct i2c_driver _gate_ic_i2c_driver = {
	.id_table = _gate_ic_i2c_id,
	.probe = _gate_ic_i2c_probe,
	.remove = _gate_ic_i2c_remove,
	.driver = {
		   .owner = THIS_MODULE,
		   .name = GATE_I2C_ID_NAME,
		   .of_match_table = _gate_ic_i2c_of_match,
		   },
};

module_i2c_driver(_gate_ic_i2c_driver);

MODULE_AUTHOR("Mediatek Corporation");
MODULE_DESCRIPTION("MTK OCP2131 I2C Driver");
MODULE_LICENSE("GPL");



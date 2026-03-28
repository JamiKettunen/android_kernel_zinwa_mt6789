// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2019 MediaTek Inc.

#include <linux/delay.h>
#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/regmap.h>
#include <linux/videodev2.h>
#include <linux/pinctrl/consumer.h>
#include <media/v4l2-subdev.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <linux/pm_runtime.h>

#if IS_ENABLED(CONFIG_MTK_FLASHLIGHT)
#include "flashlight-core.h"

#include <linux/power_supply.h>
#endif

#define FLASHLIGHTGPIO_NAME	"flashlightgpio"

/* registers definitions */
/* TODO: define register */
#define FLASHLIGHTGPIO_FLASH_BRT_MIN 10900
#define FLASHLIGHTGPIO_FLASH_BRT_STEP 11725
#define FLASHLIGHTGPIO_FLASH_BRT_MAX 1499975

#define FLASHLIGHTGPIO_FLASH_TOUT_MIN 200
#define FLASHLIGHTGPIO_FLASH_TOUT_STEP 200
#define FLASHLIGHTGPIO_FLASH_TOUT_MAX 1600

#define FLASHLIGHTGPIO_TORCH_BRT_MIN 1964
#define FLASHLIGHTGPIO_TORCH_BRT_STEP 2800
#define FLASHLIGHTGPIO_TORCH_BRT_MAX 357554

enum flashlightgpio_led_id {
	FLASHLIGHTGPIO_LED0 = 0,
	FLASHLIGHTGPIO_LED1,
	FLASHLIGHTGPIO_LED_MAX
};

/* struct flashlightgpio_platform_data
 *
 * @max_flash_timeout: flash timeout
 * @max_flash_brt: flash mode led brightness
 * @max_torch_brt: torch mode led brightness
 */
struct flashlightgpio_platform_data {
	u32 max_flash_timeout;
	u32 max_flash_brt[FLASHLIGHTGPIO_LED_MAX];
	u32 max_torch_brt[FLASHLIGHTGPIO_LED_MAX];
};

/**
 * struct flashlightgpio_flash
 *
 * @dev: pointer to &struct device
 * @pdata: platform data
 * @regmap: reg. map for i2c
 * @lock: muxtex for serial access.
 * @led_mode: V4L2 LED mode
 * @ctrls_led: V4L2 controls
 * @subdev_led: V4L2 subdev
 */
struct flashlightgpio_flash {
	struct device *dev;
	struct flashlightgpio_platform_data *pdata;
	struct regmap *regmap;
	struct mutex lock;

	enum v4l2_flash_led_mode led_mode;
	struct v4l2_ctrl_handler ctrls_led[FLASHLIGHTGPIO_LED_MAX];
	struct v4l2_subdev subdev_led[FLASHLIGHTGPIO_LED_MAX];
	struct pinctrl *flashlightgpio_pinctrl;
	struct pinctrl_state *flashlightgpio_enable_high;
	struct pinctrl_state *flashlightgpio_enable_low;
	struct pinctrl_state *flashlightgpio_mode_high;
	struct pinctrl_state *flashlightgpio_mode_low;
#if IS_ENABLED(CONFIG_MTK_FLASHLIGHT)
	struct flashlight_device_id flash_dev_id[FLASHLIGHTGPIO_LED_MAX];
#endif
};

/* define usage count */
static int use_count;

static struct flashlightgpio_flash *flashlightgpio_flash_data;

#define to_flashlightgpio_flash(_ctrl, _no)	\
	container_of(_ctrl->handler, struct flashlightgpio_flash, ctrls_led[_no])

/* define pinctrl */
/* TODO: define pinctrl */
#define FLASHLIGHTGPIO_PINCTRL_PIN_ENALE 0
#define FLASHLIGHTGPIO_PINCTRL_PIN_MODE 1
#define FLASHLIGHTGPIO_PINCTRL_PINSTATE_LOW 0
#define FLASHLIGHTGPIO_PINCTRL_PINSTATE_HIGH 1
#define FLASHLIGHTGPIO_PINCTRL_STATE_ENABLE_HIGH "enable_high"
#define FLASHLIGHTGPIO_PINCTRL_STATE_ENABLE_LOW  "enable_low"
#define FLASHLIGHTGPIO_PINCTRL_STATE_MODE_HIGH "mode_high"
#define FLASHLIGHTGPIO_PINCTRL_STATE_MODE_LOW  "mode_low"
/******************************************************************************
 * Pinctrl configuration
 *****************************************************************************/
static int flashlightgpio_pinctrl_init(struct flashlightgpio_flash *flash)
{
	int ret = 0;

	/* get pinctrl */
	flash->flashlightgpio_pinctrl = devm_pinctrl_get(flash->dev);
	if (IS_ERR(flash->flashlightgpio_pinctrl)) {
		pr_info("Failed to get flashlight pinctrl.\n");
		ret = PTR_ERR(flash->flashlightgpio_pinctrl);
		return ret;
	}

	/* Flashlight pin initialization */
	flash->flashlightgpio_enable_high = pinctrl_lookup_state(
			flash->flashlightgpio_pinctrl,
			FLASHLIGHTGPIO_PINCTRL_STATE_ENABLE_HIGH);
	if (IS_ERR(flash->flashlightgpio_enable_high)) {
		pr_info("Failed to init (%s)\n",
			FLASHLIGHTGPIO_PINCTRL_STATE_ENABLE_HIGH);
		ret = PTR_ERR(flash->flashlightgpio_enable_high);
	}
	flash->flashlightgpio_enable_low = pinctrl_lookup_state(
			flash->flashlightgpio_pinctrl,
			FLASHLIGHTGPIO_PINCTRL_STATE_ENABLE_LOW);
	if (IS_ERR(flash->flashlightgpio_enable_low)) {
		pr_info("Failed to init (%s)\n", FLASHLIGHTGPIO_PINCTRL_STATE_ENABLE_LOW);
		ret = PTR_ERR(flash->flashlightgpio_enable_low);
	}
	
	flash->flashlightgpio_mode_high = pinctrl_lookup_state(
			flash->flashlightgpio_pinctrl,
			FLASHLIGHTGPIO_PINCTRL_STATE_MODE_HIGH);
	if (IS_ERR(flash->flashlightgpio_mode_high)) {
		pr_info("Failed to init (%s)\n",
			FLASHLIGHTGPIO_PINCTRL_STATE_MODE_HIGH);
		ret = PTR_ERR(flash->flashlightgpio_mode_high);
	}
	flash->flashlightgpio_mode_low = pinctrl_lookup_state(
			flash->flashlightgpio_pinctrl,
			FLASHLIGHTGPIO_PINCTRL_STATE_MODE_LOW);
	if (IS_ERR(flash->flashlightgpio_mode_low)) {
		pr_info("Failed to init (%s)\n", FLASHLIGHTGPIO_PINCTRL_STATE_MODE_LOW);
		ret = PTR_ERR(flash->flashlightgpio_mode_low);
	}

	return ret;
}

static int flashlightgpio_pinctrl_set(struct flashlightgpio_flash *flash, int pin, int state)
{
	int ret = 0;

	if (IS_ERR(flash->flashlightgpio_pinctrl)) {
		pr_info("pinctrl is not available\n");
		return -1;
	}

	switch (pin) {
	case FLASHLIGHTGPIO_PINCTRL_PIN_ENALE:
		if (state == FLASHLIGHTGPIO_PINCTRL_PINSTATE_LOW &&
				!IS_ERR(flash->flashlightgpio_enable_low))
			pinctrl_select_state(flash->flashlightgpio_pinctrl,
					flash->flashlightgpio_enable_low);
		else if (state == FLASHLIGHTGPIO_PINCTRL_PINSTATE_HIGH &&
				!IS_ERR(flash->flashlightgpio_enable_high))
			pinctrl_select_state(flash->flashlightgpio_pinctrl,
					flash->flashlightgpio_enable_high);
		else
			pr_info("set err, pin(%d) state(%d)\n", pin, state);
		break;
	case FLASHLIGHTGPIO_PINCTRL_PIN_MODE:
		if (state == FLASHLIGHTGPIO_PINCTRL_PINSTATE_LOW &&
				!IS_ERR(flash->flashlightgpio_mode_low))
			pinctrl_select_state(flash->flashlightgpio_pinctrl,
					flash->flashlightgpio_mode_low);
		else if (state == FLASHLIGHTGPIO_PINCTRL_PINSTATE_HIGH &&
				!IS_ERR(flash->flashlightgpio_mode_high))
			pinctrl_select_state(flash->flashlightgpio_pinctrl,
					flash->flashlightgpio_mode_high);
		else
			pr_info("set err, pin(%d) state(%d)\n", pin, state);
		break;
	default:
		pr_info("set err, pin(%d) state(%d)\n", pin, state);
		break;
	}
	pr_info("pin(%d) state(%d)\n", pin, state);

	return ret;
}

/* enable mode control */
static int flashlightgpio_mode_ctrl(struct flashlightgpio_flash *flash)
{
	int rval = -EINVAL;

	/* TODO: wrap mode ctrl function */
	switch (flash->led_mode) {
	case V4L2_FLASH_LED_MODE_NONE:
		/* turn off */
		break;
	case V4L2_FLASH_LED_MODE_TORCH:
		flashlightgpio_pinctrl_set(flash, FLASHLIGHTGPIO_PINCTRL_PIN_MODE, FLASHLIGHTGPIO_PINCTRL_PINSTATE_LOW);
		/* torch mode */
		break;
	case V4L2_FLASH_LED_MODE_FLASH:
		flashlightgpio_pinctrl_set(flash, FLASHLIGHTGPIO_PINCTRL_PIN_MODE, FLASHLIGHTGPIO_PINCTRL_PINSTATE_HIGH);
		/* flash mode */
		break;
	}
	return rval;
}

/* led1/2 enable/disable */
static int flashlightgpio_enable_ctrl(struct flashlightgpio_flash *flash,
			      enum flashlightgpio_led_id led_no, bool on)
{
	int rval = 0;

	/* TODO: wrap enable function */
	if (led_no == FLASHLIGHTGPIO_LED0) {
		if (on) {
			/* enable led 0*/
			flashlightgpio_pinctrl_set(flash, FLASHLIGHTGPIO_PINCTRL_PIN_ENALE, FLASHLIGHTGPIO_PINCTRL_PINSTATE_HIGH);
			;
		} else {
			/* disable led 0*/
			flashlightgpio_pinctrl_set(flash, FLASHLIGHTGPIO_PINCTRL_PIN_ENALE, FLASHLIGHTGPIO_PINCTRL_PINSTATE_LOW);
			;
		}
	} else {
		if (on) {
			/* enable led 1*/
			;
		} else {
			/* disable led 1*/
			;
		}
	}
	return rval;
}

/* torch1/2 brightness control */
static int flashlightgpio_torch_brt_ctrl(struct flashlightgpio_flash *flash,
				 enum flashlightgpio_led_id led_no, unsigned int brt)
{
	int rval = 0;

	/* TODO: wrap set torch brightness function */
	return rval;
}

/* flash1/2 brightness control */
static int flashlightgpio_flash_brt_ctrl(struct flashlightgpio_flash *flash,
				 enum flashlightgpio_led_id led_no, unsigned int brt)
{
	int rval = 0;

	/* TODO: wrap set flash brightness function */
	return rval;
}

/* flash1/2 timeout control */
static int flashlightgpio_flash_tout_ctrl(struct flashlightgpio_flash *flash,
				unsigned int tout)
{
	int rval = 0;

	/* TODO: wrap set flash timeout function */
	return rval;
}

/* v4l2 controls  */
static int flashlightgpio_get_ctrl(struct v4l2_ctrl *ctrl, enum flashlightgpio_led_id led_no)
{
	struct flashlightgpio_flash *flash = to_flashlightgpio_flash(ctrl, led_no);
	int rval = -EINVAL;

	mutex_lock(&flash->lock);

	/* TODO: wrap get hw fault function */
	mutex_unlock(&flash->lock);
	return rval;
}

static int flashlightgpio_set_ctrl(struct v4l2_ctrl *ctrl, enum flashlightgpio_led_id led_no)
{
	struct flashlightgpio_flash *flash = to_flashlightgpio_flash(ctrl, led_no);
	int rval = -EINVAL;

	mutex_lock(&flash->lock);

	switch (ctrl->id) {
	case V4L2_CID_FLASH_LED_MODE:
		flash->led_mode = ctrl->val;
		if (flash->led_mode != V4L2_FLASH_LED_MODE_FLASH)
			rval = flashlightgpio_mode_ctrl(flash);
		else
			rval = 0;
		if (flash->led_mode == V4L2_FLASH_LED_MODE_NONE)
			flashlightgpio_enable_ctrl(flash, led_no, false);
		break;

	case V4L2_CID_FLASH_STROBE_SOURCE:
		break;

	case V4L2_CID_FLASH_STROBE:
		if (flash->led_mode != V4L2_FLASH_LED_MODE_FLASH) {
			rval = -EBUSY;
			goto err_out;
		}
		flash->led_mode = V4L2_FLASH_LED_MODE_FLASH;
		rval = flashlightgpio_mode_ctrl(flash);
		break;

	case V4L2_CID_FLASH_STROBE_STOP:
		if (flash->led_mode != V4L2_FLASH_LED_MODE_FLASH) {
			rval = -EBUSY;
			goto err_out;
		}
		flash->led_mode = V4L2_FLASH_LED_MODE_NONE;
		rval = flashlightgpio_mode_ctrl(flash);
		flashlightgpio_enable_ctrl(flash, led_no, false);
		break;

	case V4L2_CID_FLASH_TIMEOUT:
		rval = flashlightgpio_flash_tout_ctrl(flash, ctrl->val);
		break;

	case V4L2_CID_FLASH_INTENSITY:
		rval = flashlightgpio_flash_brt_ctrl(flash, led_no, ctrl->val);
		break;

	case V4L2_CID_FLASH_TORCH_INTENSITY:
		rval = flashlightgpio_torch_brt_ctrl(flash, led_no, ctrl->val);
		break;
	}

err_out:
	mutex_unlock(&flash->lock);
	return rval;
}

static int flashlightgpio_led1_get_ctrl(struct v4l2_ctrl *ctrl)
{
	return flashlightgpio_get_ctrl(ctrl, FLASHLIGHTGPIO_LED1);
}

static int flashlightgpio_led1_set_ctrl(struct v4l2_ctrl *ctrl)
{
	return flashlightgpio_set_ctrl(ctrl, FLASHLIGHTGPIO_LED1);
}

static int flashlightgpio_led0_get_ctrl(struct v4l2_ctrl *ctrl)
{
	return flashlightgpio_get_ctrl(ctrl, FLASHLIGHTGPIO_LED0);
}

static int flashlightgpio_led0_set_ctrl(struct v4l2_ctrl *ctrl)
{
	return flashlightgpio_set_ctrl(ctrl, FLASHLIGHTGPIO_LED0);
}

static const struct v4l2_ctrl_ops flashlightgpio_led_ctrl_ops[FLASHLIGHTGPIO_LED_MAX] = {
	[FLASHLIGHTGPIO_LED0] = {
			.g_volatile_ctrl = flashlightgpio_led0_get_ctrl,
			.s_ctrl = flashlightgpio_led0_set_ctrl,
			},
	[FLASHLIGHTGPIO_LED1] = {
			.g_volatile_ctrl = flashlightgpio_led1_get_ctrl,
			.s_ctrl = flashlightgpio_led1_set_ctrl,
			}
};

static int flashlightgpio_init_controls(struct flashlightgpio_flash *flash,
				enum flashlightgpio_led_id led_no)
{
	struct v4l2_ctrl *fault;
	u32 max_flash_brt = flash->pdata->max_flash_brt[led_no];
	u32 max_torch_brt = flash->pdata->max_torch_brt[led_no];
	struct v4l2_ctrl_handler *hdl = &flash->ctrls_led[led_no];
	const struct v4l2_ctrl_ops *ops = &flashlightgpio_led_ctrl_ops[led_no];

	v4l2_ctrl_handler_init(hdl, 8);

	/* flash mode */
	v4l2_ctrl_new_std_menu(hdl, ops, V4L2_CID_FLASH_LED_MODE,
			       V4L2_FLASH_LED_MODE_TORCH, ~0x7,
			       V4L2_FLASH_LED_MODE_NONE);
	flash->led_mode = V4L2_FLASH_LED_MODE_NONE;

	/* flash source */
	v4l2_ctrl_new_std_menu(hdl, ops, V4L2_CID_FLASH_STROBE_SOURCE,
			       0x1, ~0x3, V4L2_FLASH_STROBE_SOURCE_SOFTWARE);

	/* flash strobe */
	v4l2_ctrl_new_std(hdl, ops, V4L2_CID_FLASH_STROBE, 0, 0, 0, 0);

	/* flash strobe stop */
	v4l2_ctrl_new_std(hdl, ops, V4L2_CID_FLASH_STROBE_STOP, 0, 0, 0, 0);

	/* flash strobe timeout */
	v4l2_ctrl_new_std(hdl, ops, V4L2_CID_FLASH_TIMEOUT,
			  FLASHLIGHTGPIO_FLASH_TOUT_MIN,
			  flash->pdata->max_flash_timeout,
			  FLASHLIGHTGPIO_FLASH_TOUT_STEP,
			  flash->pdata->max_flash_timeout);

	/* flash brt */
	v4l2_ctrl_new_std(hdl, ops, V4L2_CID_FLASH_INTENSITY,
			  FLASHLIGHTGPIO_FLASH_BRT_MIN, max_flash_brt,
			  FLASHLIGHTGPIO_FLASH_BRT_STEP, max_flash_brt);

	/* torch brt */
	v4l2_ctrl_new_std(hdl, ops, V4L2_CID_FLASH_TORCH_INTENSITY,
			  FLASHLIGHTGPIO_TORCH_BRT_MIN, max_torch_brt,
			  FLASHLIGHTGPIO_TORCH_BRT_STEP, max_torch_brt);

	/* fault */
	fault = v4l2_ctrl_new_std(hdl, ops, V4L2_CID_FLASH_FAULT, 0,
				  V4L2_FLASH_FAULT_OVER_VOLTAGE
				  | V4L2_FLASH_FAULT_OVER_TEMPERATURE
				  | V4L2_FLASH_FAULT_SHORT_CIRCUIT
				  | V4L2_FLASH_FAULT_TIMEOUT, 0, 0);
	if (fault != NULL)
		fault->flags |= V4L2_CTRL_FLAG_VOLATILE;

	if (hdl->error)
		return hdl->error;

	flash->subdev_led[led_no].ctrl_handler = hdl;
	return 0;
}

/* initialize device */
static const struct v4l2_subdev_ops flashlightgpio_ops = {
	.core = NULL,
};

static const struct regmap_config flashlightgpio_regmap = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = 0xFF,
};

static void flashlightgpio_v4l2_i2c_subdev_init(struct v4l2_subdev *sd,
		struct i2c_client *client,
		const struct v4l2_subdev_ops *ops)
{
	v4l2_subdev_init(sd, ops);
	sd->flags |= V4L2_SUBDEV_FL_IS_I2C;
	/* the owner is the same as the i2c_client's driver owner */
	sd->owner = client->dev.driver->owner;
	sd->dev = &client->dev;
	/* i2c_client and v4l2_subdev point to one another */
	v4l2_set_subdevdata(sd, client);
	i2c_set_clientdata(client, sd);
	/* initialize name */
	snprintf(sd->name, sizeof(sd->name), "%s %d-%04x",
		client->dev.driver->name, i2c_adapter_id(client->adapter),
		client->addr);
}

static int flashlightgpio_open(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	int ret;

	pr_info("%s\n", __func__);

	ret = pm_runtime_get_sync(sd->dev);
	if (ret < 0) {
		pm_runtime_put_noidle(sd->dev);
		return ret;
	}

	return 0;
}

static int flashlightgpio_close(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	pr_info("%s\n", __func__);

	pm_runtime_put(sd->dev);

	return 0;
}

static const struct v4l2_subdev_internal_ops flashlightgpio_int_ops = {
	.open = flashlightgpio_open,
	.close = flashlightgpio_close,
};

static int flashlightgpio_subdev_init(struct flashlightgpio_flash *flash,
			      enum flashlightgpio_led_id led_no, char *led_name)
{
	struct i2c_client *client = to_i2c_client(flash->dev);
	struct device_node *np = flash->dev->of_node, *child;
	const char *fled_name = "flash";
	int rval;

	// pr_info("%s %d", __func__, led_no);

	flashlightgpio_v4l2_i2c_subdev_init(&flash->subdev_led[led_no],
				client, &flashlightgpio_ops);
	flash->subdev_led[led_no].flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	flash->subdev_led[led_no].internal_ops = &flashlightgpio_int_ops;
	strscpy(flash->subdev_led[led_no].name, led_name,
		sizeof(flash->subdev_led[led_no].name));

	for (child = of_get_child_by_name(np, fled_name); child;
			child = of_find_node_by_name(child, fled_name)) {
		int rv;
		u32 reg = 0;

		rv = of_property_read_u32(child, "reg", &reg);
		if (rv)
			continue;

		if (reg == led_no)
			flash->subdev_led[led_no].fwnode = of_fwnode_handle(child);
	}

	rval = flashlightgpio_init_controls(flash, led_no);
	if (rval)
		goto err_out;
	rval = media_entity_pads_init(&flash->subdev_led[led_no].entity, 0, NULL);
	if (rval < 0)
		goto err_out;
	flash->subdev_led[led_no].entity.function = MEDIA_ENT_F_FLASH;

	rval = v4l2_async_register_subdev(&flash->subdev_led[led_no]);
	if (rval < 0)
		goto err_out;

	return rval;

err_out:
	v4l2_ctrl_handler_free(&flash->ctrls_led[led_no]);
	return rval;
}

/* flashlight init */
static int flashlightgpio_init(struct flashlightgpio_flash *flash)
{
	int rval = 0;

	/* TODO: wrap init function */
	//flashlightgpio_pinctrl_set(flash, FLASHLIGHTGPIO_PINCTRL_PIN_ENALE, FLASHLIGHTGPIO_PINCTRL_PINSTATE_HIGH);

	return rval;
}

/* flashlight uninit */
static int flashlightgpio_uninit(struct flashlightgpio_flash *flash)
{
	flashlightgpio_pinctrl_set(flash,
			FLASHLIGHTGPIO_PINCTRL_PIN_ENALE, FLASHLIGHTGPIO_PINCTRL_PINSTATE_LOW);

	return 0;
}

static int flashlightgpio_flash_open(void)
{
	return 0;
}

static int flashlightgpio_flash_release(void)
{
	return 0;
}

static int flashlightgpio_ioctl(unsigned int cmd, unsigned long arg)
{
	struct flashlight_dev_arg *fl_arg;
	int channel;

	fl_arg = (struct flashlight_dev_arg *)arg;
	channel = fl_arg->channel;

	switch (cmd) {
	case FLASH_IOC_SET_ONOFF:
		pr_info("FLASH_IOC_SET_ONOFF(%d): %d\n",
				channel, (int)fl_arg->arg);
		if ((int)fl_arg->arg) {
			flashlightgpio_torch_brt_ctrl(flashlightgpio_flash_data, channel, 25000);
			flashlightgpio_flash_data->led_mode = V4L2_FLASH_LED_MODE_TORCH;
			flashlightgpio_mode_ctrl(flashlightgpio_flash_data);
			flashlightgpio_enable_ctrl(flashlightgpio_flash_data, channel, true);
		} else {
			flashlightgpio_flash_data->led_mode = V4L2_FLASH_LED_MODE_NONE;
			flashlightgpio_mode_ctrl(flashlightgpio_flash_data);
			flashlightgpio_enable_ctrl(flashlightgpio_flash_data, channel, false);
		}
		break;
	default:
		pr_info("No such command and arg(%d): (%d, %d)\n",
				channel, _IOC_NR(cmd), (int)fl_arg->arg);
		return -ENOTTY;
	}

	return 0;
}

static int flashlightgpio_set_driver(int set)
{
	int ret = 0;

	/* set chip and usage count */
	//mutex_lock(&flashlightgpio_mutex);
	if (set) {
		if (!use_count)
			ret = flashlightgpio_init(flashlightgpio_flash_data);
		use_count++;
		pr_debug("Set driver: %d\n", use_count);
	} else {
		use_count--;
		if (!use_count)
			ret = flashlightgpio_uninit(flashlightgpio_flash_data);
		if (use_count < 0)
			use_count = 0;
		pr_debug("Unset driver: %d\n", use_count);
	}
	//mutex_unlock(&flashlightgpio_mutex);

	return 0;
}

static ssize_t flashlightgpio_strobe_store(struct flashlight_arg arg)
{
	flashlightgpio_set_driver(1);
	//flashlightgpio_set_level(arg.channel, arg.level);
	//flashlightgpio_timeout_ms[arg.channel] = 0;
	//flashlightgpio_enable(arg.channel);
	flashlightgpio_torch_brt_ctrl(flashlightgpio_flash_data, arg.channel,
				arg.level * 25000);
	flashlightgpio_flash_data->led_mode = V4L2_FLASH_LED_MODE_TORCH;
	flashlightgpio_mode_ctrl(flashlightgpio_flash_data);
	msleep(arg.dur);
	//flashlightgpio_disable(arg.channel);
	flashlightgpio_flash_data->led_mode = V4L2_FLASH_LED_MODE_NONE;
	flashlightgpio_mode_ctrl(flashlightgpio_flash_data);
	flashlightgpio_enable_ctrl(flashlightgpio_flash_data, arg.channel, false);
	flashlightgpio_set_driver(0);
	return 0;
}

static struct flashlight_operations flashlightgpio_flash_ops = {
	flashlightgpio_flash_open,
	flashlightgpio_flash_release,
	flashlightgpio_ioctl,
	flashlightgpio_strobe_store,
	flashlightgpio_set_driver
};

static int flashlightgpio_parse_dt(struct flashlightgpio_flash *flash)
{
	struct device_node *np, *cnp;
	struct device *dev = flash->dev;
	u32 decouple = 0;
	int i = 0;

	if (!dev || !dev->of_node)
		return -ENODEV;

	np = dev->of_node;
	for_each_child_of_node(np, cnp) {
		if (of_property_read_u32(cnp, "type",
					&flash->flash_dev_id[i].type))
			goto err_node_put;
		if (of_property_read_u32(cnp,
					"ct", &flash->flash_dev_id[i].ct))
			goto err_node_put;
		if (of_property_read_u32(cnp,
					"part", &flash->flash_dev_id[i].part))
			goto err_node_put;
		snprintf(flash->flash_dev_id[i].name, FLASHLIGHT_NAME_SIZE,
				flash->subdev_led[i].name);
		flash->flash_dev_id[i].channel = i;
		flash->flash_dev_id[i].decouple = decouple;

		pr_info("Parse dt (type,ct,part,name,channel,decouple)=(%d,%d,%d,%s,%d,%d).\n",
				flash->flash_dev_id[i].type,
				flash->flash_dev_id[i].ct,
				flash->flash_dev_id[i].part,
				flash->flash_dev_id[i].name,
				flash->flash_dev_id[i].channel,
				flash->flash_dev_id[i].decouple);
		if (flashlight_dev_register_by_device_id(&flash->flash_dev_id[i],
			&flashlightgpio_flash_ops))
			return -EFAULT;
		i++;
	}

	return 0;

err_node_put:
	of_node_put(cnp);
	return -EINVAL;
}

static int flashlightgpio_probe(struct i2c_client *client,
			const struct i2c_device_id *devid)
{
	struct flashlightgpio_flash *flash;
	struct flashlightgpio_platform_data *pdata = dev_get_platdata(&client->dev);
	int rval;

	pr_info("%s:%d", __func__, __LINE__);

	flash = devm_kzalloc(&client->dev, sizeof(*flash), GFP_KERNEL);
	if (flash == NULL)
		return -ENOMEM;

	flash->regmap = devm_regmap_init_i2c(client, &flashlightgpio_regmap);
	if (IS_ERR(flash->regmap)) {
		rval = PTR_ERR(flash->regmap);
		return rval;
	}

	/* if there is no platform data, use chip default value */
	if (pdata == NULL) {
		pdata = devm_kzalloc(&client->dev, sizeof(*pdata), GFP_KERNEL);
		if (pdata == NULL)
			return -ENODEV;
		pdata->max_flash_timeout = FLASHLIGHTGPIO_FLASH_TOUT_MAX;
		/* led 1 */
		pdata->max_flash_brt[FLASHLIGHTGPIO_LED0] = FLASHLIGHTGPIO_FLASH_BRT_MAX;
		pdata->max_torch_brt[FLASHLIGHTGPIO_LED0] = FLASHLIGHTGPIO_TORCH_BRT_MAX;
		/* led 2 */
		pdata->max_flash_brt[FLASHLIGHTGPIO_LED1] = FLASHLIGHTGPIO_FLASH_BRT_MAX;
		pdata->max_torch_brt[FLASHLIGHTGPIO_LED1] = FLASHLIGHTGPIO_TORCH_BRT_MAX;
	}
	flash->pdata = pdata;
	flash->dev = &client->dev;
	mutex_init(&flash->lock);
	flashlightgpio_flash_data = flash;

	rval = flashlightgpio_pinctrl_init(flash);
	if (rval < 0)
		return rval;

	rval = flashlightgpio_subdev_init(flash, FLASHLIGHTGPIO_LED0, "flashlightgpio-led0");
	if (rval < 0)
		return rval;

	rval = flashlightgpio_subdev_init(flash, FLASHLIGHTGPIO_LED1, "flashlightgpio-led1");
	if (rval < 0)
		return rval;

	pm_runtime_enable(flash->dev);

	rval = flashlightgpio_parse_dt(flash);

	i2c_set_clientdata(client, flash);

	pr_info("%s:%d", __func__, __LINE__);
	return 0;
}

static int flashlightgpio_remove(struct i2c_client *client)
{
	struct flashlightgpio_flash *flash = i2c_get_clientdata(client);
	unsigned int i;

	for (i = FLASHLIGHTGPIO_LED0; i < FLASHLIGHTGPIO_LED_MAX; i++) {
		v4l2_device_unregister_subdev(&flash->subdev_led[i]);
		v4l2_ctrl_handler_free(&flash->ctrls_led[i]);
		media_entity_cleanup(&flash->subdev_led[i].entity);
	}

	pm_runtime_disable(&client->dev);

	pm_runtime_set_suspended(&client->dev);
	return 0;
}

static int __maybe_unused flashlightgpio_suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct flashlightgpio_flash *flash = i2c_get_clientdata(client);

	pr_info("%s %d", __func__, __LINE__);

	return flashlightgpio_uninit(flash);
}

static int __maybe_unused flashlightgpio_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct flashlightgpio_flash *flash = i2c_get_clientdata(client);

	pr_info("%s %d", __func__, __LINE__);

	return flashlightgpio_init(flash);
}

static const struct i2c_device_id flashlightgpio_id_table[] = {
	{FLASHLIGHTGPIO_NAME, 0},
	{}
};

MODULE_DEVICE_TABLE(i2c, flashlightgpio_id_table);

static const struct of_device_id flashlightgpio_of_table[] = {
	{ .compatible = "mediatek,flashlightgpio" },
	{ },
};
MODULE_DEVICE_TABLE(of, flashlightgpio_of_table);

static const struct dev_pm_ops flashlightgpio_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(pm_runtime_force_suspend,
				pm_runtime_force_resume)
	SET_RUNTIME_PM_OPS(flashlightgpio_suspend, flashlightgpio_resume, NULL)
};

static struct i2c_driver flashlightgpio_i2c_driver = {
	.driver = {
		   .name = FLASHLIGHTGPIO_NAME,
		   .pm = &flashlightgpio_pm_ops,
		   .of_match_table = flashlightgpio_of_table,
		   },
	.probe = flashlightgpio_probe,
	.remove = flashlightgpio_remove,
	.id_table = flashlightgpio_id_table,
};

module_i2c_driver(flashlightgpio_i2c_driver);

MODULE_AUTHOR("Roger-HY Wang <roger-hy.wang@mediatek.com>");
MODULE_DESCRIPTION("FLASHLIGHTGPIO LED flash driver");
MODULE_LICENSE("GPL");

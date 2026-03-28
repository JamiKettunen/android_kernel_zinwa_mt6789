/*
 * bbq20kbd.c - Linux driver for BlackBerry Q10 keyboard over I2C
 *
 * Copyright (C) 2023 Your Name
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
twi1: twi@0x05002400{
clock-frequency = <100000>;
pinctrl-0 = <&twi1_pins_a>;
pinctrl-1 = <&twi1_pins_b>;
status = "okay";
q20_keyboard {
    status = "okay";
    compatible = "allwinner,q20-keyboard";
    reg = <0x1f>;
    power_ldo = <&reg_ldoio0>;
    power_ldo_vol = <3300>;
    int_gpio = <&pio PH 18 6 0xffffffff 0xffffffff 0>;
    rst_gpio = <&pio PH 19 1 0xffffffff 0xffffffff 1>;
};
};

 */

#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/gpio.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/pm.h>
#include <linux/pm_runtime.h>

#include <linux/gpio.h>
#include <linux/pinctrl/consumer.h>
#include <linux/of.h>
#include <linux/of_gpio.h>

#include <linux/regulator/consumer.h>

#include <linux/proc_fs.h> 
#include <linux/seq_file.h>


#define DRIVER_NAME "bbq20kbd"

/* I2C Registers */
#define BBQ20_REG_VER		0x01 //fw version
#define BBQ20_REG_CFG		0x02 //config
#define BBQ20_REG_INT		0x03 //interrupt status
#define BBQ20_REG_KEY		0x04 //key status
#define BBQ20_REG_BKL		0x05 //backlight
#define BBQ20_REG_EDB		0x06 //debounce cfg
#define BBQ20_REG_FRQ		0x07 //poll freq cfg
#define BBQ20_REG_RST		0x08 //reset
#define BBQ20_REG_FIF		0x09 //fifo
#define BBQ20_REG_BK2		0x0A //backlight 2
#define BBQ20_REG_DIR           0x0B //gpio direction
#define BBQ20_REG_PUE           0x0C //gpio input pull enable
#define BBQ20_REG_PUD           0x0D //gpio input pull direction
#define BBQ20_REG_GIO           0x0E //gpio value
#define BBQ20_REG_GIC           0x0F //gpio interrupt config
#define BBQ20_REG_GIN           0x10 //gpio interrupt status
#define BBQ20_REG_HLD           0x11 //Key hold threshold configuration
#define BBQ20_REG_ADR           0x12 //Device I2C addres default 0x1F
#define BBQ20_REG_IND           0x13 //Interrupt duration default 1ms
#define BBQ20_REG_CF2           0x14 //The configuration register 2
#define BBQ20_REG_TOX           0x15 //Trackpad X-axis position range of (-128 to 127)
#define BBQ20_REG_TOY           0x16 //Trackpad Y-axis position range of (-128 to 127)

/* Configuration bits */
#define BBQ20_CFG_OVERFLOW_ON  (1 << 0)
#define BBQ20_CFG_OVERFLOW_INT (1 << 1)
#define BBQ20_CFG_CAPSLOCK_INT (1 << 2)
#define BBQ20_CFG_NUMLOCK_INT  (1 << 3)
#define BBQ20_CFG_KEY_INT      (1 << 4)
#define BBQ20_CFG_PANIC_INT    (1 << 5)
#define BBQ20_CFG_REPORT_MODS  (1 << 6)
#define BBQ20_CFG_USE_MODS     (1 << 7)

/* Interrupt status bits */
#define BBQ20_INT_OVERFLOW    (1 << 0)
#define BBQ20_INT_CAPSLOCK    (1 << 1)
#define BBQ20_INT_NUMLOCK     (1 << 2)
#define BBQ20_INT_KEY         (1 << 3)
#define BBQ20_INT_PANIC       (1 << 4)
#define BBQ20_INT_GPIO	      (1 << 5)
#define BBQ20_INT_TOUCH       (1 << 6)

#define Q20_KEYBOARD_DEBUG 1
#if Q20_KEYBOARD_DEBUG
#define Q20_KEYBOARD_LOG(fmt, args...)    pr_err("[%s] %s %d: " fmt, DRIVER_NAME, __func__, __LINE__, ##args)
#else
#define Q20_KEYBOARD_LOG(fmt, args...)    pr_info("[%s] %s %d: " fmt, DRIVER_NAME, __func__, __LINE__, ##args)
#endif
#define Q20_KEYBOARD_ERR(fmt, args...)    pr_err("[%s] %s %d: " fmt, DRIVER_NAME, __func__, __LINE__, ##args)

const char *power;
u32 power_vol;

#define PROC_NAME	"firmware_upgrade"
static struct proc_dir_entry *fwu_proc_entry;

struct regulator *power_ldo;


struct bbq20kbd {
    struct i2c_client *client;
    struct input_dev *input;
    struct work_struct work;
    u16 **keymap[128];
    u8 backlight_level;
    bool suspended;
    
    int irq_gpio;
	int rst_gpio;
	int power_vdd_gpio;
	int power_1v8_gpio;
	int power_2v8_gpio;
	int firmware_download_gpio;
};
struct bbq20kbd * g_kbd;
#if 0
static int touch_x[3] = {0};
static int touch_y[3] = {0};
static int touch_count = 0;
#endif

#define KEY_YUAN    227
#define KEY_SYM	    333

static const u16 keycodes[128] = {
	/* 0x00 */ KEY_A, 		/*  30 */
	/* 0x01 */ KEY_S, 		/*  31 */
	/* 0x02 */ KEY_D,		/*  32 */
	/* 0x03 */ KEY_F,		/*  33 */
	/* 0x04 */ KEY_H,		/*  35 */
	/* 0x05 */ KEY_ENTER,		/*  34 */
	/* 0x06 */ 231,			/*  KEY_CALL */
	/* 0x07 */ KEY_BACK,		/*  45 */
	/* 0x08 */ KEY_BACKSPACE,	/*  46 */
	/* 0x09 */ KEY_V,		/*  47 */
	/* 0x0a */ KEY_ENTER,		/*  86 */
	/* 0x0b */ KEY_B,		/*  48 */
	/* 0x0c */ KEY_Q,		/*  16 */
	/* 0x0d */ KEY_W,		/*  17 */
	/* 0x0e */ KEY_E,		/*  18 */
	/* 0x0f */ KEY_R,		/*  19 */
	/* 0x10 */ KEY_Y,		/*  21 */
	/* 0x11 */ KEY_MENU,		/*  20 */
	/* 0x12 */ 107,			/*   KEY_ENDCALL */
	/* 0x13 */ KEY_2,		/*   3 */
	/* 0x14 */ KEY_3,		/*   4 */
	/* 0x15 */ KEY_4,		/*   5 */
	/* 0x16 */ KEY_6,		/*   7 */
	/* 0x17 */ KEY_5,		/*   6 */
	/* 0x18 */ KEY_EQUAL,		/*  13 */
	/* 0x19 */ KEY_9,		/*  10 */
	/* 0x1a */ KEY_LEFTALT,		/*   8 */
	/* 0x1b */ KEY_LEFTSHIFT,	/*  12 */
	/* 0x1c */ KEY_RIGHTSHIFT,	/*   9 */
	/* 0x1d */ KEY_SYM,		/*  sym */
	/* 0x1e */ KEY_RIGHTBRACE,	/*  27 */
	/* 0x1f */ KEY_O,		/*  24 */
	/* 0x20 */ KEY_SPACE,		/*  22 */
	/* 0x21 */ KEY_LEFTBRACE,	/*  26 */
	/* 0x22 */ KEY_I,		/*  23 */
	/* 0x23 */ KEY_P,		/*  25 */
	/* 0x24 */ KEY_YUAN,		/*  $ */
	/* 0x25 */ KEY_L,		/*  38 */
	/* 0x26 */ KEY_J,		/*  36 */
	/* 0x27 */ KEY_APOSTROPHE,	/*  40 */
	/* 0x28 */ KEY_K,		/*  37 */
	/* 0x29 */ KEY_SEMICOLON,	/*  39 */
	/* 0x2a */ KEY_BACKSLASH,	/*  43 */
	/* 0x2b */ KEY_Q,		/*  51 */
	/* 0x2c */ KEY_SLASH,		/*  53 */
	/* 0x2d */ KEY_N,		/*  49 */
	/* 0x2e */ KEY_M,		/*  50 */
	/* 0x2f */ KEY_DOT,		/*  52 */
	/* 0x30 */ KEY_0,		/*  15 */
	/* 0x31 */ KEY_SPACE,		/*  57 */
	/* 0x32 */ KEY_GRAVE,		/*  41 */
	/* 0x33 */ KEY_BACKSPACE,	/*  14 */
	/* 0x34 */ KEY_KPENTER,		/*  96 */
	/* 0x35 */ KEY_ESC,		/*   1 */
	/* 0x36 */ KEY_LEFTCTRL,	/*  29 */
	/* 0x37 */ KEY_LEFTMETA,	/* 125 */
	/* 0x38 */ KEY_LEFTSHIFT,	/*  42 */
	/* 0x39 */ KEY_CAPSLOCK,	/*  58 */
	/* 0x3a */ KEY_LEFTALT,		/*  56 */
	/* 0x3b */ KEY_LEFT,		/* 105 */
	/* 0x3c */ KEY_RIGHT,		/* 106 */
	/* 0x3d */ KEY_DOWN,		/* 108 */
	/* 0x3e */ KEY_UP,		/* 103 */
	/* 0x3f */ KEY_FN,		/* 0x1d0 */
	/* 0x40 */ 0,
	/* 0x41 */ KEY_A,		/*  83 */
	/* 0x42 */ KEY_B,
	/* 0x43 */ KEY_C,	/*  55 */
	/* 0x44 */ KEY_D,
	/* 0x45 */ KEY_E,		/*  78 */
	/* 0x46 */ KEY_F,
	/* 0x47 */ KEY_G,		/*  69 */
	/* 0x48 */ KEY_H,
	/* 0x49 */ KEY_I,
	/* 0x4a */ KEY_J,
	/* 0x4b */ KEY_K,		/*  98 */
	/* 0x4c */ KEY_L,		/*  96 */
	/* 0x4d */ KEY_M,
	/* 0x4e */ KEY_N,		/*  74 */
	/* 0x4f */ KEY_O,
	/* 0x50 */ KEY_P,
	/* 0x51 */ KEY_Q,		/* 117 */
	/* 0x52 */ KEY_R,		/*  82 */
	/* 0x53 */ KEY_S,		/*  79 */
	/* 0x54 */ KEY_T,		/*  80 */
	/* 0x55 */ KEY_U,		/*  81 */
	/* 0x56 */ KEY_V,		/*  75 */
	/* 0x57 */ KEY_W,		/*  76 */
	/* 0x58 */ KEY_X,		/*  77 */
	/* 0x59 */ KEY_Y,		/*  71 */
	/* 0x5a */ KEY_Z,
	/* 0x5b */ KEY_KP8,		/*  72 */
	/* 0x5c */ KEY_KP9,		/*  73 */
	/* 0x5d */ KEY_YEN,		/* 124 */
	/* 0x5e */ KEY_RO,		/*  89 */
	/* 0x5f */ KEY_KPCOMMA,		/* 121 */
	/* 0x60 */ KEY_F5,		/*  63 */
	/* 0x61 */ KEY_A,		/*  64 */
	/* 0x62 */ KEY_B,		/*  65 */
	/* 0x63 */ KEY_C,		/*  61 */
	/* 0x64 */ KEY_D,		/*  66 */
	/* 0x65 */ KEY_E,		/*  67 */
	/* 0x66 */ KEY_F,		/* 123 */
	/* 0x67 */ KEY_G,		/*  87 */
	/* 0x68 */ KEY_H,		/* 122 */
	/* 0x69 */ KEY_I,		/*  99 */
	/* 0x6a */ KEY_J,
	/* 0x6b */ KEY_K,		/*  70 */
	/* 0x6c */ KEY_L,
	/* 0x6d */ KEY_M,		/*  68 */
	/* 0x6e */ KEY_N,		/* 127 */
	/* 0x6f */ KEY_O,		/*  88 */
	/* 0x70 */ KEY_P,
	/* 0x71 */ KEY_Q,		/* 119 */
	/* 0x72 */ KEY_R,		/* 110 */
	/* 0x73 */ KEY_S,		/* 102 */
	/* 0x74 */ KEY_T,		/* 104 */
	/* 0x75 */ KEY_U,		/* 111 */
	/* 0x76 */ KEY_V,		/*  62 */
	/* 0x77 */ KEY_W,		/* 107 */
	/* 0x78 */ KEY_X,		/*  60 */
	/* 0x79 */ KEY_Y,		/* 109 */
	/* 0x7a */ KEY_Z,		/*  59 */
	/* 0x7b */ KEY_RIGHTSHIFT,	/*  54 */
	/* 0x7c */ KEY_RIGHTALT,	/* 100 */
	/* 0x7d */ KEY_RIGHTCTRL,	/*  97 */
	/* 0x7e */ KEY_RIGHTMETA,	/* 126 */
	/* 0x7f */ KEY_POWER,		/* 116 */
};

static int write_register(struct i2c_client *client, u8 reg, u8 value)
{
    u8 buf[2];
    buf[0] = reg | 0x80; // 应用写入掩码
    buf[1] = value;

    return i2c_master_send(client, buf, 2);
}

static irqreturn_t bbq20kbd_interrupt(int irq, void *dev_id)
{
    struct bbq20kbd *kbd = dev_id;
    
    if (!kbd->suspended)
        schedule_work(&kbd->work);
    
    return IRQ_HANDLED;
}

static void bbq20kbd_work_handler(struct work_struct *work)
{
    struct bbq20kbd *kbd = container_of(work, struct bbq20kbd, work);
    struct i2c_client *client = kbd->client;
    u8 int_status, key_count;//key_data[3];
    u8 fifo_data[2];
    int ret, i;
    int x,y;
    int key_code = -1;
    
    /* Read interrupt status */
    int_status = i2c_smbus_read_byte_data(client, BBQ20_REG_INT);
    //Q20_KEYBOARD_ERR("%s:reg int status = %d\n",__func__,int_status);
    if (int_status < 0) {
        Q20_KEYBOARD_ERR("failed to read interrupt status\n");
        return;
    }
    
    /* Handle key press interrupt */
    if (int_status == 8) { //key
        /* Read number of keys in FIFO */
        key_count = (i2c_smbus_read_byte_data(client, BBQ20_REG_KEY) & 0x0F);
	//Q20_KEYBOARD_ERR("%s:key_count = %d\n",__func__,key_count);
        if (key_count > 0) {
            for (i = 0; i < key_count; i++) {
                /* Read key data (3 bytes per key: state, code, mods) */
                //ret = i2c_smbus_read_i2c_block_data(client, BBQ20_REG_KEY, 3, key_data);
		//Q20_KEYBOARD_ERR("%s:KEY:state = %d;code = %d;mods = %d\n",__func__,key_data[0],key_data[1],key_data[2]); 
                ret = i2c_smbus_read_i2c_block_data(client, BBQ20_REG_FIF, 2, fifo_data);
                Q20_KEYBOARD_ERR("%s:FIFO:state = %d;code = 0x%x\n",__func__,fifo_data[0],fifo_data[1]);
		/* Report key event */
		if(fifo_data[0] == 1){ //key down
		    //input_report_key(kbd->input, fifo_data[1], 1);
		    input_report_key(kbd->input, keycodes[fifo_data[1]], 1);
		    input_sync(kbd->input);
		}else if(fifo_data[0] == 3){//key up
		    //input_report_key(kbd->input, fifo_data[1], 0);
		    input_report_key(kbd->input, keycodes[fifo_data[1]], 0);
		    input_sync(kbd->input);
		}

            }
	}
    } else if(int_status == 64){ //touch pad
#if 0
	touch_x[touch_count] = i2c_smbus_read_byte_data(client, BBQ20_REG_TOX);	
	touch_y[touch_count] = i2c_smbus_read_byte_data(client, BBQ20_REG_TOY);
	if (touch_x[touch_count] > 127){
                touch_x[touch_count] -= 256;
	}
	if (touch_y[touch_count] > 127){
                touch_y[touch_count] -= 256;
	}

	Q20_KEYBOARD_ERR("%s:touch_x[%d] = %d; start_y[%d] = %d\n",__func__,touch_count,touch_x[touch_count],touch_count,touch_y[touch_count]);
	touch_count++;
	if(touch_count >= 3){
	    // 判断滑动方向
	    if (abs(touch_y[1]) > 8) {// 垂直滑动
		key_code = (touch_y[1] > 0) ? KEY_DOWN : KEY_UP;
	    } else if(abs(touch_x[1]) > 8){// 水平滑动
		key_code = (touch_x[1] > 0) ? KEY_RIGHT : KEY_LEFT;
	    }
	    // 上报按键事件
	    if(key_code > 0) {
		input_report_key(kbd->input, key_code, 1);  // 按下
		input_report_key(kbd->input, key_code, 0);  // 释放
		input_sync(kbd->input);
		touch_count = 0;
	    }
	}
#else
	x = i2c_smbus_read_byte_data(client, BBQ20_REG_TOX);
	y = i2c_smbus_read_byte_data(client, BBQ20_REG_TOY);
	if (x > 127){
            x -= 256;
        }
        if (y > 127){
            y -= 256;
        }

	Q20_KEYBOARD_ERR("%s:touch_x= %d; start_y= %d\n",__func__,x,y);
	// 判断滑动方向
	if (abs(y) > 10) {// 垂直滑动
	    Q20_KEYBOARD_ERR("------>y = %d\n",y);
	    if(y > 0){
		key_code = KEY_DOWN; //0x6c
	    }else{
		key_code = KEY_UP;  //0x67
	    }
        }
	if(abs(x) > 13){// 水平滑动
	    Q20_KEYBOARD_ERR("------>x = %d\n",x);
	    if(x > 0){
                key_code = KEY_RIGHT;  //0x6a
            }else{
                key_code = KEY_LEFT; //0x69
            }
        }
        // 上报按键事件
	if(key_code > 0){
	    Q20_KEYBOARD_ERR("------>key code = %x\n",key_code);
	    input_report_key(kbd->input, key_code, 1);  // 按下
	    input_report_key(kbd->input, key_code, 0);  // 释放
	    input_sync(kbd->input);
	    mdelay(200);
	}
	key_code = -1;
#endif
    }
    /* Clear interrupts */
    i2c_smbus_write_byte_data(client, BBQ20_REG_INT, 0x0);
}

static int bbq20kbd_initialize(struct bbq20kbd *kbd)
{
    struct i2c_client *client = kbd->client;
    int ret;
    
    /* Check device fw vsion */
    ret = i2c_smbus_read_byte_data(client, BBQ20_REG_VER);
    Q20_KEYBOARD_ERR("%s:fw version = %d\n",__func__,ret);
        
    /* Configure keyboard */
    ret = write_register(client, BBQ20_REG_CFG, (BBQ20_CFG_OVERFLOW_INT | BBQ20_CFG_KEY_INT | BBQ20_CFG_REPORT_MODS | BBQ20_CFG_NUMLOCK_INT | BBQ20_CFG_CAPSLOCK_INT));
    //ret = i2c_smbus_write_byte_data(client, BBQ20_REG_CFG, (BBQ20_CFG_KEY_INT | BBQ20_CFG_OVERFLOW_INT | BBQ20_CFG_REPORT_MODS | BBQ20_CFG_NUMLOCK_INT | BBQ20_CFG_CAPSLOCK_INT));
    if (ret < 0) {
        Q20_KEYBOARD_ERR("failed to configure keyboard\n");
        return ret;
    }

    /* Check Configure */
    ret = i2c_smbus_read_byte_data(client, BBQ20_REG_CFG);
    Q20_KEYBOARD_ERR("%s:Configure = %d\n",__func__,ret);

    /* Clear any pending interrupts */
    ret = i2c_smbus_read_byte_data(client, BBQ20_REG_INT);
    Q20_KEYBOARD_ERR("%s:reg int status = %d\n",__func__,ret);
    if (ret > 0) {
        i2c_smbus_write_byte_data(client, BBQ20_REG_INT, ret);
    }
    
    return 0;
}

static int q20_keyboard_probe_dt(struct i2c_client *client)
{
	struct property *prop;
    struct device_node *np = client->dev.of_node;
    struct bbq20kbd *kbd = i2c_get_clientdata(client);
    int ret;	

    if (!np) {
	np = of_find_node_by_name(NULL, "q20_keyboard");
    }

    if (!np) {
        pr_err("ERROR! get keyboard failed, func:%s, line:%d\n", __func__, __LINE__);
	//goto devicetree_get_item_err;
    }

    ret = of_property_read_string(np,"power_ldo", &power);
    if (ret)
        pr_err("get power_ldo is fail, %d\n", ret);

    ret = of_property_read_u32(np,"power_ldo_vol", &power_vol);
    if (ret)
        pr_err("get power_ldo_vol is fail, %d\n", ret);

	prop = of_find_property(np, "int-gpios", NULL);
	if (prop && prop->length) {
		kbd->irq_gpio = of_get_named_gpio_flags(np,
				"int-gpios", 0, NULL);
	} else {
		kbd->irq_gpio = -1;
	}

	prop = of_find_property(np, "reset-gpios", NULL);
	if (prop && prop->length) {
		kbd->rst_gpio = of_get_named_gpio_flags(np,
				"reset-gpios", 0, NULL);
	} else {
		kbd->rst_gpio = -1;
	}
	
	prop = of_find_property(np, "power-vdd-gpios", NULL);
	if (prop && prop->length) {
		kbd->power_vdd_gpio = of_get_named_gpio_flags(np,
				"power-vdd-gpios", 0, NULL);
	} else {
		kbd->power_vdd_gpio = -1;
	}
	
	prop = of_find_property(np, "power-1v8-gpios", NULL);
	if (prop && prop->length) {
		kbd->power_1v8_gpio = of_get_named_gpio_flags(np,
				"power-1v8-gpios", 0, NULL);
	} else {
		kbd->power_1v8_gpio = -1;
	}
	
	prop = of_find_property(np, "power-2v8-gpios", NULL);
	if (prop && prop->length) {
		kbd->power_2v8_gpio = of_get_named_gpio_flags(np,
				"power-2v8-gpios", 0, NULL);
	} else {
		kbd->power_2v8_gpio = -1;
	}
	
	prop = of_find_property(np, "firmware-download-gpios", NULL);
	if (prop && prop->length) {
		kbd->firmware_download_gpio = of_get_named_gpio_flags(np,
				"firmware-download-gpios", 0, NULL);
	} else {
		kbd->firmware_download_gpio = -1;
	}

    return 0;
}

static int q20_keypoard_set_gpio(int gpio,
		bool config, int dir, int state)
{
	int retval;
	char label[16];

	if (config) {
		retval = snprintf(label, 16, "q20_gpio_%d\n", gpio);
		if (retval < 0) {
			Q20_KEYBOARD_ERR("Failed to set GPIO label\n");
			return retval;
		}

		retval = gpio_request(gpio, label);
		if (retval < 0) {
			Q20_KEYBOARD_ERR("Failed to request GPIO %d\n",
					gpio);
			return retval;
		}

		if (dir == 0)
			retval = gpio_direction_input(gpio);
		else
			retval = gpio_direction_output(gpio, state);
		if (retval < 0) {
			Q20_KEYBOARD_ERR("Failed to set GPIO %d direction\n",
					gpio);
			return retval;
		}
	} else {
		gpio_free(gpio);
	}

	return 0;
}

static int power_enable(struct i2c_client *client, u32 enable)
{
	struct bbq20kbd *kbd = i2c_get_clientdata(client);
	int retval;
	
    power_ldo = regulator_get(NULL, power);
    if(!IS_ERR(power_ldo)){
	regulator_set_voltage(power_ldo,(int)(power_vol)*1000,(int)(power_vol)*1000);
	if(enable){
	    if (0 != regulator_enable(power_ldo))
			Q20_KEYBOARD_ERR("%s:enable ldo err...\n",__func__);
		}else{
			if (0 != regulator_disable(power_ldo))
			Q20_KEYBOARD_ERR("%s:disable ldo err...\n",__func__);
		}
    }
    
    if (kbd->power_vdd_gpio >= 0) {
		retval = q20_keypoard_set_gpio(kbd->power_vdd_gpio,
				true, 1, 0);
		if (retval < 0) {
			Q20_KEYBOARD_ERR("Failed to configure power vdd GPIO\n");
			goto err_set_gpio_vdd_power;
		}
	}
	if (kbd->power_1v8_gpio >= 0) {
		retval = q20_keypoard_set_gpio(kbd->power_1v8_gpio,
				true, 1, 0);
		if (retval < 0) {
			Q20_KEYBOARD_ERR("Failed to configure power 1v8 GPIO\n");
			goto err_set_gpio_1v8_power;
		}
	}
	if (kbd->power_2v8_gpio >= 0) {
		retval = q20_keypoard_set_gpio(kbd->power_2v8_gpio,
				true, 1, 0);
		if (retval < 0) {
			Q20_KEYBOARD_ERR("Failed to configure power 2v8 GPIO\n");
			goto err_set_gpio_2v8_power;
		}
	}
	
	if (kbd->firmware_download_gpio >= 0) {
		retval = q20_keypoard_set_gpio(kbd->firmware_download_gpio,
				true, 1, 0);
		if (retval < 0) {
			Q20_KEYBOARD_ERR("Failed to configure power firmware download GPIO\n");
		}
	}
	
	if (kbd->power_vdd_gpio >= 0) {
		gpio_set_value(kbd->power_vdd_gpio, 1);
	}
	mdelay(10);
	if (kbd->power_1v8_gpio >= 0) {
		gpio_set_value(kbd->power_1v8_gpio, 1);
	}
	
	if (kbd->power_2v8_gpio >= 0) {
		gpio_set_value(kbd->power_2v8_gpio, 1);
	}
	
	if (kbd->firmware_download_gpio >= 0) {
		gpio_set_value(kbd->firmware_download_gpio, 0);
	}
    mdelay(10);
err_set_gpio_2v8_power:
	if (kbd->power_2v8_gpio >= 0)
		q20_keypoard_set_gpio(kbd->power_2v8_gpio, false, 0, 0);

err_set_gpio_1v8_power:
	if (kbd->power_1v8_gpio >= 0)
		q20_keypoard_set_gpio(kbd->power_1v8_gpio, false, 0, 0);
		
err_set_gpio_vdd_power:
	if (kbd->power_vdd_gpio >= 0)
		q20_keypoard_set_gpio(kbd->power_vdd_gpio, false, 0, 0);
    return 0;
}

/*add by hodafone begin*/
static int firmware_upgrade_debug_read(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", gpio_get_value(g_kbd->firmware_download_gpio));
	return 0;
}

static int firmware_upgrade_open(struct inode *inode, struct file *file)
{
	return single_open(file, firmware_upgrade_debug_read, NULL);
};

static ssize_t firmware_upgrade_debug_write(struct file *filp, const char  *buff, size_t len, loff_t *data)
{
	unsigned char writebuf[50];	
	if (copy_from_user(&writebuf, buff, len)) {
		return -EFAULT;
	}

    Q20_KEYBOARD_ERR("HodafoneLog firmware_upgrade_debug_write writebuf=%s",writebuf);
   	if(writebuf[0] == '0'){
		if (g_kbd->power_vdd_gpio >= 0) {
			gpio_set_value(g_kbd->power_vdd_gpio, 1);
		}
		mdelay(10);
		if (g_kbd->power_1v8_gpio >= 0) {
			gpio_set_value(g_kbd->power_1v8_gpio, 1);
		}
		
		if (g_kbd->power_2v8_gpio >= 0) {
			gpio_set_value(g_kbd->power_2v8_gpio, 1);
		}
		
		if (g_kbd->firmware_download_gpio >= 0) {
			gpio_set_value(g_kbd->firmware_download_gpio, 0);
		}
	}else if(writebuf[0] == '1'){
		if (g_kbd->power_1v8_gpio >= 0) {
			gpio_set_value(g_kbd->power_1v8_gpio, 0);
		}
		
		if (g_kbd->power_2v8_gpio >= 0) {
			gpio_set_value(g_kbd->power_2v8_gpio, 0);
		}
		mdelay(10);
		
		if (g_kbd->power_vdd_gpio >= 0) {
			gpio_set_value(g_kbd->power_vdd_gpio, 0);
		}
		mdelay(100);

		if (g_kbd->power_vdd_gpio >= 0) {
			gpio_set_value(g_kbd->power_vdd_gpio, 1);
		}
		mdelay(10);
		if (g_kbd->firmware_download_gpio >= 0) {
			gpio_set_value(g_kbd->firmware_download_gpio, 1);
		}
	} 
	return len;
}

static const struct proc_ops firmware_upgrade_proc_ops = {
	.proc_open  = firmware_upgrade_open,
	.proc_read  = seq_read,
    .proc_write = firmware_upgrade_debug_write,
    .proc_release = single_release,
};

/*add by hodafone end*/

static int bbq20kbd_probe(struct i2c_client *client,
                         const struct i2c_device_id *id)
{
    struct bbq20kbd *kbd;
    struct input_dev *input;
    int error;
    int i = 0;
    u32 irq_number = 0;
 
    if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
        Q20_KEYBOARD_ERR("I2C functionality not supported\n");
        return -ENODEV;
    }
    
    kbd = devm_kzalloc(&client->dev, sizeof(*kbd), GFP_KERNEL);
    if (!kbd)
        return -ENOMEM;

    kbd->client = client;
    g_kbd = kbd;
    i2c_set_clientdata(client, kbd);
    
    q20_keyboard_probe_dt(client);
 
    input = devm_input_allocate_device(&client->dev);
    if (!input)
        return -ENOMEM;
    
    kbd->input = input;
    INIT_WORK(&kbd->work, bbq20kbd_work_handler);
    
    /* Initialize keymap (should be filled with actual BB Q20 keycodes) */
    memset(kbd->keymap, KEY_RESERVED, sizeof(kbd->keymap));
    
    /* Set up input device */
    input->name = "BlackBerry Q20 Keyboard";
    input->id.bustype = BUS_I2C;
    input->dev.parent = &client->dev;
    
    /* Set supported key events */
    set_bit(EV_KEY, input->evbit);
    set_bit(EV_REP, input->evbit);
    //set_bit(EV_ABS, input->evbit); 
    /* Set supported keys */
    for (i = 0; i < ARRAY_SIZE(keycodes); i++) {
		__set_bit(keycodes[i], input->keybit);
    }
    
    // 设置坐标范围，根据实际屏幕分辨率调整
    //input_set_abs_params(dev->input_dev, ABS_MT_TRACKING_ID, 0,255, 0, 0);
    //input_set_abs_params(dev->input_dev, ABS_MT_POSITION_X, 0, 720, 0, 0);
    //input_set_abs_params(dev->input_dev, ABS_MT_POSITION_Y, 0, 720, 0, 0);
    //input_set_abs_params(dev->input_dev, ABS_MT_TOUCH_MAJOR, 0,255, 0, 0);
    //input_set_abs_params(dev->input_dev, ABS_MT_WIDTH_MAJOR, 0, 255, 0, 0);
 
    error = input_register_device(input);
    if (error) {
        Q20_KEYBOARD_ERR("failed to register input device\n");
        return error;
    }
    
    /*add by hodafone begin*/
	fwu_proc_entry = proc_create(PROC_NAME, 0777, NULL,&firmware_upgrade_proc_ops);
	/*add by hodafone end*/
    
    power_enable(client,1);   
    /* Initialize hardware */
    bbq20kbd_initialize(kbd);
   
	irq_number = gpio_to_irq(kbd->irq_gpio); 
    /* Set up interrupt */
    if (irq_number > 0) {
        error = devm_request_irq(&client->dev, irq_number, bbq20kbd_interrupt,
	    IRQF_TRIGGER_LOW | IRQF_ONESHOT,
            DRIVER_NAME, kbd);
        if (error) {
            Q20_KEYBOARD_ERR("failed to request IRQ\n");
            return error;
        }
    } else {
        Q20_KEYBOARD_LOG("no IRQ configured, using polling\n");
        // TODO: Implement polling if no interrupt available
    }
    
    device_init_wakeup(&client->dev, true);
    
    return 0;
}

static int bbq20kbd_remove(struct i2c_client *client)
{
    struct bbq20kbd *kbd = i2c_get_clientdata(client);
    
    cancel_work_sync(&kbd->work);
    
    /* Turn off backlight */
    i2c_smbus_write_byte_data(client, BBQ20_REG_BKL, 0);
    
    return 0;
}

#ifdef CONFIG_PM_SLEEP
static int bbq20kbd_suspend(struct device *dev)
{
    struct i2c_client *client = to_i2c_client(dev);
    struct bbq20kbd *kbd = i2c_get_clientdata(client);
    
    kbd->suspended = true;
    
    if (device_may_wakeup(dev)) {
        enable_irq_wake(client->irq);
    } else {
        disable_irq(client->irq);
        /* Turn off backlight to save power */
        i2c_smbus_write_byte_data(client, BBQ20_REG_BKL, 0);
    }
    
    return 0;
}

static int bbq20kbd_resume(struct device *dev)
{
    struct i2c_client *client = to_i2c_client(dev);
    struct bbq20kbd *kbd = i2c_get_clientdata(client);
    
    if (device_may_wakeup(dev)) {
        disable_irq_wake(client->irq);
    } else {
        enable_irq(client->irq);
        /* Restore backlight level */
        i2c_smbus_write_byte_data(client, BBQ20_REG_BKL, kbd->backlight_level);
    }
    
    kbd->suspended = false;
    
    /* Clear any pending interrupts */
    i2c_smbus_write_byte_data(client, BBQ20_REG_INT, 
        i2c_smbus_read_byte_data(client, BBQ20_REG_INT));
    
    return 0;
}
#endif

static SIMPLE_DEV_PM_OPS(bbq20kbd_pm_ops, bbq20kbd_suspend, bbq20kbd_resume);

static const struct i2c_device_id bbq20kbd_id[] = {
    { "bbq20kbd", 0 },
    { }
};
MODULE_DEVICE_TABLE(i2c, bbq20kbd_id);

#ifdef CONFIG_OF
static const struct of_device_id bbq20kbd_of_match[] = {
    { .compatible = "allwinner,q20-keyboard" },
    { }
};
MODULE_DEVICE_TABLE(of, bbq20kbd_of_match);
#endif

static struct i2c_driver bbq20kbd_driver = {
    .driver = {
        .name = DRIVER_NAME,
        .pm = &bbq20kbd_pm_ops,
        .of_match_table = of_match_ptr(bbq20kbd_of_match),
    },
    .probe = bbq20kbd_probe,
    .remove = bbq20kbd_remove,
    .id_table = bbq20kbd_id,
};

module_i2c_driver(bbq20kbd_driver);

MODULE_AUTHOR("aaron_cx@163.com");
MODULE_DESCRIPTION("BlackBerry Q20 Keyboard Driver");
MODULE_LICENSE("GPL v2");

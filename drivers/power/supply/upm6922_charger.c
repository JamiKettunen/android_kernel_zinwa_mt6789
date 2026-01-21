/*
 * UPM6922 battery charging driver
 *
 * Copyright (C) 2023 Unisemipower
 *
 * This package is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 * THIS PACKAGE IS PROVIDED ``AS IS'' AND WITHOUT ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, WITHOUT LIMITATION, THE IMPLIED
 * WARRANTIES OF MERCHANTIBILITY AND FITNESS FOR A PARTICULAR PURPOSE.
 */

#define pr_fmt(fmt) "[upm6922-charger]:%s:" fmt, __func__

#include <linux/gpio.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/power_supply.h>
#include <linux/slab.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_gpio.h>
#include <linux/err.h>
#include <linux/bitops.h>
#include <linux/math64.h>
#include <linux/extcon-provider.h>
#include <linux/regulator/consumer.h>
#include <linux/regulator/driver.h>
#include <linux/phy/phy.h>
#include <linux/iio/consumer.h>

#include "upm6922_charger.h"
#include "charger_class.h"
#include "mtk_charger.h"
#include "mtk_battery_table.h"

struct upm6922_platform_data {
    int ilim;
    int ichg;
    int iprechg;
    int iterm;
    int cv;
    int vlim;
    int boostv;
    int boosti;
    int vac_ovp;
    int statctrl;
    
    /*mediatek usb */
    u32 bc12_sel;
	u32 boot_mode;
	u32 boot_type;
	enum upm6922_attach_trigger attach_trig;
};

struct upm6922 {
    struct device *dev;
    struct i2c_client *client;

    int part_no;
    int revision;

    const char *chg_dev_name;

    int irq;
    u32 intr_gpio;

    struct mutex i2c_rw_lock;

    bool power_good;
    bool vbus_attach;

    struct upm6922_platform_data *upd;
    bool en_auto_bc12;

    struct charger_device *chg_dev;

    struct delayed_work psy_dwork;
    struct delayed_work prob_dwork;
    struct delayed_work monitor_dwork;
    struct wakeup_source *monitor_ws;

    int psy_usb_type;

    struct power_supply_desc psy_desc;
    struct power_supply_config psy_cfg;
    struct power_supply *psy;

    struct regulator_dev *otg_rdev;
    struct power_supply *inform_psy;
    
    struct iio_channel *iio_adcs;
	struct mutex adc_lock;
	
	struct mutex attach_lock;
	atomic_t attach;
	enum port_stat bc12_stat;
	bool bc12_en;
	bool bc12_dn;
	struct workqueue_struct *wq;
	struct work_struct bc12_work;
};

struct upm6922 *g_upm;

static const char *const upm6922_attach_trig_names[] = {
	"ignore", "pwr_rdy", "typec",
};

static const char *const upm6922_port_stat_names[] = {
	[PORT_STAT_NOINFO] = "No Info",
	[PORT_STAT_SDP] = "SDP",
	[PORT_STAT_CDP] = "CDP",
	[PORT_STAT_DCP] = "DCP",
	[PORT_STAT_UNKNOWN_TA] = "Unknown TA",
	[PORT_STAT_NON_STD] = "NON_STD",
};

static int __upm6922_read_reg(struct upm6922 *upm, u8 reg, u8 *data)
{
    s32 ret;

    ret = i2c_smbus_read_byte_data(upm->client, reg);
    if (ret < 0) {
        upm6922_err("i2c read fail: can't read from reg 0x%02X\n", reg);
        return ret;
    }

    *data = (u8) ret;

    return 0;
}

static int __upm6922_write_reg(struct upm6922 *upm, int reg, u8 val)
{
    s32 ret;

    ret = i2c_smbus_write_byte_data(upm->client, reg, val);
    if (ret < 0) {
        upm6922_err("i2c write fail: can't write 0x%02X to reg 0x%02X: %d\n",
               val, reg, ret);
        return ret;
    }
    return 0;
}

static int upm6922_read_byte(struct upm6922 *upm, u8 reg, u8 *data)
{
    int ret;

    mutex_lock(&upm->i2c_rw_lock);
    ret = __upm6922_read_reg(upm, reg, data);
    mutex_unlock(&upm->i2c_rw_lock);

    return ret;
}

static int upm6922_write_byte(struct upm6922 *upm, u8 reg, u8 data)
{
    int ret;

    mutex_lock(&upm->i2c_rw_lock);
    ret = __upm6922_write_reg(upm, reg, data);
    mutex_unlock(&upm->i2c_rw_lock);

    if (ret)
        upm6922_err("Failed: reg=%02X, ret=%d\n", reg, ret);

    return ret;
}

static int upm6922_update_bits(struct upm6922 *upm, u8 reg, u8 mask, u8 data)
{
    int ret;
    u8 tmp;

    mutex_lock(&upm->i2c_rw_lock);
    ret = __upm6922_read_reg(upm, reg, &tmp);
    if (ret) {
        upm6922_err("Failed: reg=%02X, ret=%d\n", reg, ret);
        goto out;
    }

    tmp &= ~mask;
    tmp |= data & mask;

    ret = __upm6922_write_reg(upm, reg, tmp);
    if (ret) {
        upm6922_err("Failed: reg=%02X, ret=%d\n", reg, ret);
    }

out:
    mutex_unlock(&upm->i2c_rw_lock);
    return ret;
}

static int __maybe_unused upm6922_enter_hiz_mode(struct upm6922 *upm)
{
    u8 val = REG00_HIZ_ENABLE << REG00_ENHIZ_SHIFT;

    upm6922_info("hiz enter\n");
    return upm6922_update_bits(upm, UPM6922_REG_00, REG00_ENHIZ_MASK, val);
}

static int __maybe_unused upm6922_exit_hiz_mode(struct upm6922 *upm)
{
    u8 val = REG00_HIZ_DISABLE << REG00_ENHIZ_SHIFT;

    upm6922_info("hiz exit\n");
    return upm6922_update_bits(upm, UPM6922_REG_00, REG00_ENHIZ_MASK, val);
}

static int __maybe_unused upm6922_get_hiz_mode(struct upm6922 *upm, bool *en)
{
    int ret = 0;
    u8 val = 0;

    ret = upm6922_read_byte(upm, UPM6922_REG_00, &val);
    if (ret < 0) {
        upm6922_info("read UPM6922_REG_00 fail\n");
        return ret;
    }

    *en = (val & REG00_ENHIZ_MASK) ? true : false;

    return ret;
}

/* CON0 */
static int upm6922_set_en_hiz(struct charger_device *chg_dev, bool en)
{
	struct upm6922 *upm = charger_get_data(chg_dev);
	upm6922_err("[%s] en:%d\n", __func__, en);
	if (en) {
		upm6922_enter_hiz_mode(upm);
	} else {
		upm6922_exit_hiz_mode(upm);
	}
	return 0;
}


static int upm6922_set_stat_ctrl(struct upm6922 *upm, int ctrl)
{
    u8 val;

    upm6922_info("ctrl:%d\n", ctrl);
    val = ctrl;

    return upm6922_update_bits(upm, UPM6922_REG_00, REG00_STAT_CTRL_MASK,
                   val << REG00_STAT_CTRL_SHIFT);
}

static int upm6922_set_input_current_limit(struct upm6922 *upm, int ma)
{
    u8 val;

    upm6922_info("ma:%d\n", ma);

    if (ma < REG00_IINLIM_MIN) {
        ma = REG00_IINLIM_MIN;
    } else if (ma > REG00_IINLIM_MAX) {
        ma = REG00_IINLIM_MAX;
    }

    val = (ma - REG00_IINLIM_BASE) / REG00_IINLIM_LSB;
    return upm6922_update_bits(upm, UPM6922_REG_00, REG00_IINLIM_MASK,
                   val << REG00_IINLIM_SHIFT);
}

static int __maybe_unused upm6922_get_input_current_limit(struct upm6922 *upm, int *ma)
{
    u8 val = 0;
    int ret;
    int iindpm;

    ret = upm6922_read_byte(upm, UPM6922_REG_00, &val);
    if (ret < 0){
        upm6922_err("read UPM6922_REG_00 fail, ret:%d\n", ret);
        return ret;
    }

    iindpm = (val & REG00_IINLIM_MASK) >> REG00_IINLIM_SHIFT;
    iindpm = iindpm * REG00_IINLIM_LSB + REG00_IINLIM_BASE;
    *ma = iindpm;

    return ret;
}

static int __maybe_unused upm6922_reset_watchdog_timer(struct upm6922 *upm)
{
    u8 val = REG01_WDT_RESET << REG01_WDT_RESET_SHIFT;

    upm6922_info("reset\n");
    return upm6922_update_bits(upm, UPM6922_REG_01, REG01_WDT_RESET_MASK,
                   val);
}

static int __maybe_unused upm6922_enable_otg(struct upm6922 *upm)
{
    u8 val = REG01_OTG_ENABLE << REG01_OTG_CONFIG_SHIFT;

    upm6922_info("enable otg\n");
    return upm6922_update_bits(upm, UPM6922_REG_01, REG01_OTG_CONFIG_MASK,
                   val);
}

static int __maybe_unused upm6922_disable_otg(struct upm6922 *upm)
{
    u8 val = REG01_OTG_DISABLE << REG01_OTG_CONFIG_SHIFT;

    upm6922_info("disable otg\n");
    return upm6922_update_bits(upm, UPM6922_REG_01, REG01_OTG_CONFIG_MASK,
                   val);
}

static int __maybe_unused upm6922_enable_charger(struct upm6922 *upm)
{
    int ret;
    u8 val = REG01_CHG_ENABLE << REG01_CHG_CONFIG_SHIFT;

    upm6922_info("enable charge\n");
    ret = upm6922_update_bits(upm, UPM6922_REG_01,
                    REG01_CHG_CONFIG_MASK, val);

    return ret;
}

static int __maybe_unused upm6922_disable_charger(struct upm6922 *upm)
{
    int ret;
    u8 val = REG01_CHG_DISABLE << REG01_CHG_CONFIG_SHIFT;

    upm6922_info("disable charge\n");
    ret = upm6922_update_bits(upm, UPM6922_REG_01,
                    REG01_CHG_CONFIG_MASK, val);

    return ret;
}

static int upm6922_set_boost_current(struct upm6922 *upm, int ma)
{
    u8 val1 = 0, val2 = 0;
    int ret = 0;

    upm6922_info("ma:%d\n", ma);

    if (ma >= 2000) {
        val2 = REG15_BOOST_LIM1_2A;
    } else if (ma >= 1200) {
        val1 = REG02_BOOST_LIM0_1P2A;
        val2 = REG15_BOOST_LIM1_DIS;
    } else {
        val1 = REG02_BOOST_LIM0_0P5A;
        val2 = REG15_BOOST_LIM1_DIS;
    }

    ret = upm6922_update_bits(upm, UPM6922_REG_02, REG02_BOOST_LIM0_MASK,
                   val1 << REG02_BOOST_LIM0_SHIFT);
    if (ret < 0) {
        upm6922_err("write UPM6922_REG_02 fail, ret:%d\n", ret);
        return ret;
    }

    ret = upm6922_update_bits(upm, UPM6922_REG_15, REG15_BOOST_LIM1_MASK,
                   val1 << REG15_BOOST_LIM1_SHIFT);
    if (ret < 0) {
        upm6922_err("write UPM6922_REG_02 fail, ret:%d\n", ret);
    }

    return ret;
}

static int upm6922_set_chargecurrent(struct upm6922 *upm, int ma)
{
    u8 ichg;

    upm6922_info("ma:%d\n", ma);

    if (ma < REG02_ICHG_MIN) {
        ma = REG02_ICHG_MIN;
    } else if (ma > REG02_ICHG_MAX) {
        ma = REG02_ICHG_MAX;
    }

    ichg = (ma - REG02_ICHG_BASE) / REG02_ICHG_LSB;
    return upm6922_update_bits(upm, UPM6922_REG_02, REG02_ICHG_MASK,
                   ichg << REG02_ICHG_SHIFT);
}

static int __maybe_unused upm6922_get_chargecurrent(struct upm6922 *upm, int *ma)
{
    u8 val = 0;
    int ret = 0;

    ret = upm6922_read_byte(upm, UPM6922_REG_02, &val);
    if (ret < 0){
        upm6922_err("read UPM6922_REG_02 fail, ret:%d\n", ret);
        return ret;
    }

    *ma = ((val & REG02_ICHG_MASK) >> REG02_ICHG_SHIFT) * REG02_ICHG_LSB;

    return ret;
}

static int upm6922_set_prechg_current(struct upm6922 *upm, int ma)
{
    u8 iprechg;

    upm6922_info("ma:%d\n", ma);

    if (ma < REG03_IPRECHG_MIN) {
        ma = REG03_IPRECHG_MIN;
    } else if (ma < REG03_IPRECHG_MAX) {
        ma = REG03_IPRECHG_MAX;
    }

    iprechg = (ma - REG03_IPRECHG_BASE) / REG03_IPRECHG_LSB;

    return upm6922_update_bits(upm, UPM6922_REG_03, REG03_IPRECHG_MASK,
                   iprechg << REG03_IPRECHG_SHIFT);
}

static int __maybe_unused upm6922_get_prechg_current(struct upm6922 *upm, int *ma)
{
    u8 val = 0;
    int ret = 0;

   ret = upm6922_read_byte(upm, UPM6922_REG_03, &val);
    if (ret < 0){
        upm6922_err("read UPM6922_REG_03 fail, ret:%d\n", ret);
        return ret;
    }

    *ma = ((val & REG03_IPRECHG_MASK) >> REG03_IPRECHG_SHIFT) * REG03_IPRECHG_LSB;

    return ret;
}

static int upm6922_set_term_current(struct upm6922 *upm, int ma)
{
    u8 iterm;

    upm6922_info("ma:%d\n", ma);

    if (ma < REG03_ITERM_MIN) {
        ma = REG03_ITERM_MIN;
    } else if (ma > REG03_ITERM_MAX) {
        ma = REG03_ITERM_MAX;
    }

    iterm = (ma - REG03_ITERM_BASE) / REG03_ITERM_LSB;

    return upm6922_update_bits(upm, UPM6922_REG_03, REG03_ITERM_MASK,
                   iterm << REG03_ITERM_SHIFT);
}

static int __maybe_unused upm6922_get_term_current(struct upm6922 *upm, int *ma)
{
    u8 val = 0;
    int ret = 0;

   ret = upm6922_read_byte(upm, UPM6922_REG_03, &val);
    if (ret < 0){
        upm6922_err("read UPM6922_REG_03 fail, ret:%d\n", ret);
        return ret;
    }

    *ma = ((val & REG03_ITERM_MASK) >> REG03_ITERM_SHIFT) * REG03_ITERM_LSB;

    return ret;
}

static int upm6922_set_chargevolt(struct upm6922 *upm, int mv)
{
    u8 val1, val2;
    int ret = 0;

    upm6922_info("mv:%d\n", mv);

    if (mv < REG04_VREG_MIN) {
        mv = REG04_VREG_MIN;
    } else if (mv > REG04_VREG_MAX) {
        mv = REG04_VREG_MAX;
    }

    val1 = (mv - REG04_VREG_BASE) / REG04_VREG_LSB;
    ret = upm6922_update_bits(upm, UPM6922_REG_04, REG04_VREG_MASK,
                   val1 << REG04_VREG_SHIFT);
    if (ret < 0) {
        upm6922_err("write UPM6922_REG_04 fail, ret:%d\n", ret);
        return ret;
    }

    val2 = ((mv - REG04_VREG_BASE) % REG04_VREG_LSB) / REG0D_VREG_FT_LSB;
    ret = upm6922_update_bits(upm, UPM6922_REG_0D, REG0D_VREG_FT_MASK,
                   val2 << REG0D_VREG_FT_SHIFT);
    if (ret < 0) {
        upm6922_err("write UPM6922_REG_0D fail, ret:%d\n", ret);
    }

    return ret;
}

static int __maybe_unused upm6922_get_chargevolt(struct upm6922 *upm, int *mv)
{
    u8 val1, val2;
    int cv = 0;
    int ret = 0;

    ret = upm6922_read_byte(upm, UPM6922_REG_04, &val1);
    if (ret < 0){
        upm6922_err("read UPM6922_REG_04 fail, ret:%d\n", ret);
        return ret;
    }

    ret = upm6922_read_byte(upm, UPM6922_REG_0D, &val2);
    if (ret < 0){
        upm6922_err("read UPM6922_REG_0D fail, ret:%d\n", ret);
        return ret;
    }

    cv = ((UPM6922_REG_04 & REG04_VREG_MASK) >> REG04_VREG_SHIFT) *
                REG04_VREG_LSB + REG04_VREG_BASE;
    cv += ((val2 & REG0D_VREG_FT_MASK) >> REG0D_VREG_FT_SHIFT) * REG0D_VREG_FT_LSB;

    *mv = cv;

    return ret;
}

static int __maybe_unused upm6922_set_recharge_volt(struct upm6922 *upm, int mv)
{
    u8 val = 0;

    upm6922_info("mv:%d\n", mv);

    if (mv < 200) {
        val = REG04_VRECHG_100MV;
    } else {
        val = REG04_VRECHG_200MV;
    }

    return upm6922_update_bits(upm, UPM6922_REG_04, REG04_VRECHG_MASK,
                    val << REG04_VRECHG_SHIFT);
}

static int __maybe_unused upm6922_enable_term(struct upm6922 *upm, bool enable)
{
    u8 val;
    int ret;

    upm6922_info("enable:%d\n", enable);

    if (enable) {
        val = REG05_TERM_ENABLE << REG05_EN_TERM_SHIFT;
    } else {
        val = REG05_TERM_DISABLE << REG05_EN_TERM_SHIFT;
    }

    ret = upm6922_update_bits(upm, UPM6922_REG_05, REG05_EN_TERM_MASK, val);

    return ret;
}

static int __maybe_unused upm6922_set_watchdog_timer(struct upm6922 *upm, u8 seconds)
{
    u8 val = 0;

    upm6922_info("seconds:%d\n", seconds);

    if (seconds >= 160) {
        val = REG05_WDT_160S;
    } else if (seconds >= 80) {
        val = REG05_WDT_80S;
    } else if (seconds >= 40) {
        val = REG05_WDT_40S;
    } else {
        val = REG05_WDT_DISABLE;
    }

    return upm6922_update_bits(upm, UPM6922_REG_05, REG05_WDT_MASK,
                    val << REG05_WDT_SHIFT);
}

static int upm6922_disable_watchdog_timer(struct upm6922 *upm)
{
    u8 val = REG05_WDT_DISABLE << REG05_WDT_SHIFT;

    upm6922_info("disable wd\n");

    return upm6922_update_bits(upm, UPM6922_REG_05, REG05_WDT_MASK, val);
}

static int __maybe_unused upm6922_enable_safety_timer(struct upm6922 *upm)
{
    const u8 val = REG05_CHG_TIMER_ENABLE << REG05_EN_TIMER_SHIFT;

    upm6922_info("enable timer\n");

    return upm6922_update_bits(upm, UPM6922_REG_05, REG05_EN_TIMER_MASK,
                   val);
}

static int __maybe_unused upm6922_disable_safety_timer(struct upm6922 *upm)
{
    const u8 val = REG05_CHG_TIMER_DISABLE << REG05_EN_TIMER_SHIFT;

    upm6922_info("disable timer\n");

    return upm6922_update_bits(upm, UPM6922_REG_05, REG05_EN_TIMER_MASK,
                   val);
}

static int __maybe_unused upm6922_set_safety_timer(struct upm6922 *upm, u8 hours)
{
    u8 val;

    upm6922_info("hours:%d\n", hours);

    if (hours > 10) {
        val = REG05_CHG_TIMER_10HOURS;
    } else {
        val = REG05_CHG_TIMER_5HOURS;
    }

    return upm6922_update_bits(upm, UPM6922_REG_05, REG05_CHG_TIMER_MASK,
                   val << REG05_CHG_TIMER_SHIFT);
}

static int upm6922_set_acovp_threshold(struct upm6922 *upm, int mv)
{
    u8 val;

    upm6922_info("mv:%d\n", mv);

    if (mv >= 14000) {
        val = REG06_OVP_14V;
    } else if (mv >= 11000) {
        val = REG06_OVP_11V;
    } else if (mv >= 6500) {
        val = REG06_OVP_6P5V;
    } else {
        val = REG06_OVP_5P85V;
    }

    return upm6922_update_bits(upm, UPM6922_REG_06, REG06_OVP_MASK,
                   val << REG06_OVP_SHIFT);
}

static int upm6922_set_boost_voltage(struct upm6922 *upm, int mv)
{
    u8 val;

    upm6922_info("mv:%d\n", mv);

    if (mv >= 5300) {
        val = REG06_BOOSTV_5P3V;
    } else if (mv >= 5150) {
        val = REG06_BOOSTV_5P15V;
    } else if (mv >= 5000) {
        val = REG06_BOOSTV_5V;
    } else {
        val = REG06_BOOSTV_4P85V;
    }

    return upm6922_update_bits(upm, UPM6922_REG_06, REG06_BOOSTV_MASK,
                   val << REG06_BOOSTV_SHIFT);
}

static int upm6922_set_input_volt_limit(struct upm6922 *upm, int mv)
{
    u8 val1, val2;
    int ret = 0;

    upm6922_info("mv:%d\n", mv);

    if (mv < 3900) {
        val1 = 0;
        val2 = REG0D_VINDPM_OS_3P9V;
    } else if (mv <= 5400) {
        val1 = (mv - 3900) / REG06_VINDPM_LSB;
        val2 = REG0D_VINDPM_OS_3P9V;
    } else if (mv < 5900) {
        val1 = 0xf;
        val2 = REG0D_VINDPM_OS_3P9V;
    } else if (mv < 7500) {
        val1 = (mv - 5900) / REG06_VINDPM_LSB;
        val2 = REG0D_VINDPM_OS_5P9V;
    } else if (mv <= 9000) {
        val1 = (mv - 7500) / REG06_VINDPM_LSB;
        val2 = REG0D_VINDPM_OS_7P5V;
    } else if (mv < 10500) {
        val1 = 0xf;
        val2 = REG0D_VINDPM_OS_7P5V;
    } else if (mv <= 12000) {
        val1 = (mv - 10500) / REG06_VINDPM_LSB;
        val2 = REG0D_VINDPM_OS_10P5V;
    } else {
        val1 = 0xf;
        val2 = REG0D_VINDPM_OS_10P5V;
    }

    ret =  upm6922_update_bits(upm, UPM6922_REG_06, REG06_VINDPM_MASK,
                   val1 << REG06_VINDPM_SHIFT);
    if (ret < 0) {
        upm6922_err("write UPM6922_REG_06 fail, ret:%d\n", ret);
        return ret;
    }

    ret =  upm6922_update_bits(upm, UPM6922_REG_0D, REG0D_VINDPM_OS_MASK,
                   val2 << REG0D_VINDPM_OS_SHIFT);
    if (ret < 0) {
        upm6922_err("write UPM6922_REG_0D fail, ret:%d\n", ret);
    }

    return ret;
}

static int upm6922_force_dpdm(struct upm6922 *upm)
{
    int ret = 0;
    const u8 val = REG07_FORCE_DPDM << REG07_FORCE_DPDM_SHIFT;

    upm6922_info("iindet en\n");

    ret = upm6922_update_bits(upm, UPM6922_REG_07,
            REG07_FORCE_DPDM_MASK, val);

    return ret;
}

static int __maybe_unused upm6922_enable_batfet(struct upm6922 *upm)
{
    const u8 val = REG07_BATFET_ON << REG07_BATFET_DIS_SHIFT;

    upm6922_info("batfet turn on\n");

    return upm6922_update_bits(upm, UPM6922_REG_07, REG07_BATFET_DIS_MASK,
                   val);
}

static int __maybe_unused upm6922_disable_batfet(struct upm6922 *upm)
{
    const u8 val = REG07_BATFET_OFF << REG07_BATFET_DIS_SHIFT;

    upm6922_info("batfet turn off\n");

    return upm6922_update_bits(upm, UPM6922_REG_07, REG07_BATFET_DIS_MASK,
                   val);
}

static int __maybe_unused upm6922_en_batfet_delay(struct upm6922 *upm, bool en)
{
    u8 val;

    upm6922_info("en:%d\n", en);

    if (en) {
        val = REG07_BATFET_DLY_10S;
    } else {
        val = REG07_BATFET_DLY_0S;
    }

    return upm6922_update_bits(upm, UPM6922_REG_07, REG07_BATFET_DLY_MASK,
                   val << REG07_BATFET_DLY_SHIFT);
}

static int __maybe_unused upm6922_batfet_rst_en(struct upm6922 *upm, bool en)
{
    u8 val = 0;

    upm6922_info("en:%d\n", en);

    if (en) {
        val = REG07_BATFET_RST_ENABLE << REG07_BATFET_RST_EN_SHIFT;
    } else {
        val = REG07_BATFET_RST_DISABLE << REG07_BATFET_RST_EN_SHIFT;
    }

    return upm6922_update_bits(upm, UPM6922_REG_07, REG07_BATFET_RST_EN_MASK,
                   val);
}

static int __maybe_unused upm6922_vdpm_bat_track(struct upm6922 *upm, int mv)
{
    u8 val = 0;

    upm6922_info("mv:%d\n", mv);

    if (mv >= 300) {
        val = REG07_VDPM_BAT_TRACK_300MV;
    } else if (mv >= 250) {
        val = REG07_VDPM_BAT_TRACK_250MV;
    } else if (mv >= 200) {
        val = REG07_VDPM_BAT_TRACK_200MV;
    } else {
        val = REG07_VDPM_BAT_TRACK_DISABLE;
    }

    return upm6922_update_bits(upm, UPM6922_REG_07, REG07_VDPM_BAT_TRACK_MASK,
                   val << REG07_VDPM_BAT_TRACK_SHIFT);
}

#if 0
static int upm6922_get_charger_type(struct upm6922 *upm, int *type)
{
    int ret;
    u8 reg_val = 0;
    int vbus_stat = 0;
    int chg_type = POWER_SUPPLY_USB_TYPE_UNKNOWN;

    ret = upm6922_read_byte(upm, UPM6922_REG_08, &reg_val);
    if (ret) {
        upm6922_err("read UPM6922_REG_08 fail, ret:%d\n", ret);
        return ret;
    }

    vbus_stat = (reg_val & REG08_VBUS_STAT_MASK);
    vbus_stat >>= REG08_VBUS_STAT_SHIFT;

    switch (vbus_stat) {
        case REG08_VBUS_TYPE_NONE:
            chg_type = POWER_SUPPLY_USB_TYPE_UNKNOWN;
            upm->psy_desc.type = POWER_SUPPLY_TYPE_USB;
            upm6922_info("charger type: none\n");
            break;
        case REG08_VBUS_TYPE_SDP:
            chg_type = POWER_SUPPLY_USB_TYPE_SDP;
            upm->psy_desc.type = POWER_SUPPLY_TYPE_USB;
            upm6922_info("charger type: SDP\n");
            break;
        case REG08_VBUS_TYPE_CDP:
            chg_type = POWER_SUPPLY_USB_TYPE_CDP;
            upm->psy_desc.type = POWER_SUPPLY_TYPE_USB_CDP;
            upm6922_info("charger type: CDP\n");
            break;
        case REG08_VBUS_TYPE_DCP:
            chg_type = POWER_SUPPLY_USB_TYPE_DCP;
            upm->psy_desc.type = POWER_SUPPLY_TYPE_USB_DCP;
            upm6922_info("charger type: DCP\n");
            break;
        case REG08_VBUS_TYPE_UNKNOWN:
            chg_type = POWER_SUPPLY_USB_TYPE_SDP;
            upm->psy_desc.type = POWER_SUPPLY_TYPE_USB;
            upm6922_info("charger type: unknown adapter\n");
            break;
        case REG08_VBUS_TYPE_NON_STD:
            chg_type = POWER_SUPPLY_USB_TYPE_DCP;
            upm->psy_desc.type = POWER_SUPPLY_TYPE_USB_DCP;
            upm6922_info("charger type: non-std charger\n");
            break;
        default:
            chg_type = POWER_SUPPLY_USB_TYPE_UNKNOWN;
            upm->psy_desc.type = POWER_SUPPLY_TYPE_USB;
            upm6922_info("charger type: invalid value\n");
            break;
    }

    if (chg_type == POWER_SUPPLY_USB_TYPE_UNKNOWN) {
        upm6922_err("unknown type\n");
        //Charger_Detect_Release();
    } else if (chg_type == POWER_SUPPLY_USB_TYPE_SDP ||
        chg_type == POWER_SUPPLY_USB_TYPE_CDP) {
        //Charger_Detect_Release();
    } else if (chg_type == POWER_SUPPLY_USB_TYPE_DCP) {

    }

    *type = chg_type;

    return 0;
}
#endif

static int upm6922_get_charging_stat(struct upm6922 *upm, int *stat)
{
    int ret = 0;
    u8 reg_val1 = 0, reg_val2 = 0;
    u8 chrg_stat = 0;

    ret = upm6922_read_byte(upm, UPM6922_REG_08, &reg_val1);
    if (ret) {
        upm6922_err("read UPM6922_REG_08 fail, ret:%d\n", ret);
        return ret;
    }

    chrg_stat = (reg_val1 & REG08_CHRG_STAT_MASK) >> REG08_CHRG_STAT_SHIFT;
    switch (chrg_stat) {
        case REG08_CHRG_STAT_CHGDONE:
            *stat = POWER_SUPPLY_STATUS_FULL;
            break;

        case REG08_CHRG_STAT_PRECHG:
        case REG08_CHRG_STAT_FASTCHG:
            *stat = POWER_SUPPLY_STATUS_CHARGING;
            break;

        case REG08_CHRG_STAT_IDLE:
            ret = upm6922_read_byte(upm, UPM6922_REG_0A, &reg_val2);
            if (ret) {
                upm6922_err("read UPM6922_REG_0A fail, ret:%d\n", ret);
            }

            if (reg_val2 & REG0A_VBUS_GD_MASK) {
                *stat = POWER_SUPPLY_STATUS_NOT_CHARGING;
            } else {
                *stat = POWER_SUPPLY_STATUS_DISCHARGING;
            }
            break;

        default:
            *stat = POWER_SUPPLY_STATUS_UNKNOWN;
            break;
    }

    return ret;
}

static int upm6922_enable_vindpm_int(struct upm6922 *upm, bool en)
{
    u8 val = 0;

    upm6922_info("en:%d\n", en);

    if (en) {
        val = REG0A_VINDPM_INT_ENABLE;
    } else {
        val = REG0A_VINDPM_INT_DISABLE;
    }

    return upm6922_update_bits(upm, UPM6922_REG_0A, REG0A_VINDPM_INT_MASK,
                   val << REG0A_VINDPM_INT_SHIFT);
}

static int upm6922_enable_iindpm_int(struct upm6922 *upm, bool en)
{
    u8 val = 0;

    upm6922_info("en:%d\n", en);

    if (en) {
        val = REG0A_IINDPM_INT_ENABLE;
    } else {
        val = REG0A_IINDPM_INT_DISABLE;
    }

    return upm6922_update_bits(upm, UPM6922_REG_0A, REG0A_IINDPM_INT_MASK,
                   val << REG0A_IINDPM_INT_SHIFT);
}

static int __maybe_unused upm6922_reset_chip(struct upm6922 *upm)
{
    u8 val = REG0B_REG_RESET << REG0B_REG_RESET_SHIFT;

    upm6922_info("reset chip\n");

    return upm6922_update_bits(upm, UPM6922_REG_0B, REG0B_REG_RESET_MASK, val);

}

static int upm6922_detect_device(struct upm6922 *upm)
{
    int ret;
    u8 data;

    ret = upm6922_read_byte(upm, UPM6922_REG_0B, &data);
    if (!ret) {
        upm->part_no = (data & REG0B_PN_MASK) >> REG0B_PN_SHIFT;
        upm->revision =
            (data & REG0B_DEV_REV_MASK) >> REG0B_DEV_REV_SHIFT;
    }

    return ret;
}

static int __maybe_unused upm6922_enable_dpdm_mux(struct upm6922 *upm, bool en)
{
    const u8 val = en << REG0C_DPDM_LOCK_SHIFT;

    upm6922_info("en:%d\n", en);

    return upm6922_update_bits(upm, UPM6922_REG_0C, REG0C_DPDM_LOCK_MASK,
                   val);
}

int upm6922_set_dp(struct upm6922 *upm, int dp_stat)
{
    const u8 val = dp_stat << REG0C_DP_MUX_SHIFT;

    upm6922_info("dp_stat:%d\n", dp_stat);

    return upm6922_update_bits(upm, UPM6922_REG_0C, REG0C_DP_MUX_MASK,
                   val);
}

static int __maybe_unused upm6922_set_dm(struct upm6922 *upm, int dm_stat)
{
    const u8 val = dm_stat << REG0C_DM_MUX_SHIFT;

    upm6922_info("dm_stat:%d\n", dm_stat);

    return upm6922_update_bits(upm, UPM6922_REG_0C, REG0C_DM_MUX_MASK,
                   val);
}

static int __maybe_unused upm6922_auto_bc12_en(struct upm6922 *upm, bool en)
{
    u8 val = en << REG0C_AUTO_BC12_SHIFT;

    upm6922_info("en:%d\n", en);

    return upm6922_update_bits(upm, UPM6922_REG_0C, REG0C_AUTO_BC12_MASK,
                   val);
}

static int __maybe_unused upm6922_set_ishort(struct upm6922 *upm, int ma)
{
    u8 val = 0;

    upm6922_info("ma:%d\n", ma);

    if (ma < 90) {
        val = REG0D_ISHORT_50MA << REG0D_ISHORT_SET_SHIFT;
    } else {
        val = REG0D_ISHORT_90MA << REG0D_ISHORT_SET_SHIFT;
    }

    return upm6922_update_bits(upm, UPM6922_REG_0D, REG0D_ISHORT_SET_MASK,
                   val);
}

static int __maybe_unused upm6922_set_boost_ocp_ctrl(struct upm6922 *upm, int ctrl)
{
    u8 val = 0;

    upm6922_info("ctrl:%d\n", ctrl);

    val = (!!ctrl) << REG0D_BOOST_OCP_CTRL_SHIFT;

    return upm6922_update_bits(upm, UPM6922_REG_0D, REG0D_BOOST_OCP_CTRL_MASK,
                   val);
}

static int __maybe_unused upm6922_fast_chg_volt_th(struct upm6922 *upm, int mv)
{
    u8 val = 0;

    upm6922_info("mv:%d\n", mv);

    if (mv <= 2700) {
        val = REG0D_PRE2FAST_2P7V;
    } else if (mv <= 2800) {
        val = REG0D_PRE2FAST_2P8V;
    } else if (mv <= 2900) {
        val = REG0D_PRE2FAST_2P9V;
    } else {
        val = REG0D_PRE2FAST_3V;
    }

    return upm6922_update_bits(upm, UPM6922_REG_0D, REG0D_V_RRE2FAST_MASK,
                   val << REG0D_V_RRE2FAST_SHIFT);
}

void upm6922_dump_regs(struct upm6922 *upm)
{
    int addr;
    u8 val;
    int ret;

    for (addr = 0x0; addr <= 0x15; addr++) {
        ret = upm6922_read_byte(upm, addr, &val);
        if (ret == 0)
            upm6922_err("Reg[%.2x] = 0x%.2x\n", addr, val);
    }
}

static struct upm6922_platform_data *upm6922_parse_dt(struct device_node *np,
                              struct upm6922 *upm)
{
    int ret;
    u32 val;
    struct upm6922_platform_data *pdata;
    struct device_node *bc12_np, *boot_np;
    const struct {
		u32 size;
		u32 tag;
		u32 boot_mode;
		u32 boot_type;
	} *tag;	

    pdata = devm_kzalloc(&upm->client->dev, sizeof(struct upm6922_platform_data),
                 GFP_KERNEL);
    if (!pdata)
        return NULL;

    if (of_property_read_string(np, "charger_name", &upm->chg_dev_name) < 0) {
        upm->chg_dev_name = "primary_chg";
        upm6922_err("no charger name, default primary chg\n");
    }

    upm->intr_gpio = of_get_named_gpio(np, "up,intr_gpio", 0);
    if (upm->intr_gpio < 0) {
        upm6922_err("no up,intr_gpio, ret:%d\n", upm->intr_gpio);
    }
    upm6922_err("intr_gpio = %d\n", upm->intr_gpio);

    ret = of_property_read_u32(np, "up,upm6922,iindpm", &pdata->ilim);
    if (ret) {
        pdata->ilim = 2000;
    }

    ret = of_property_read_u32(np, "up,upm6922,ichg", &pdata->ichg);
    if (ret) {
        pdata->ichg = 2000;
        upm6922_err("Failed to read node of up,upm6922,ichg, default 2000\n");
    }

    ret = of_property_read_u32(np, "up,upm6922,iprechg", &pdata->iprechg);
    if (ret) {
        pdata->iprechg = 180;
        upm6922_err("Failed to read node of up,upm6922,iprechg, default 180\n");
    }

    ret = of_property_read_u32(np, "up,upm6922,iterm", &pdata->iterm);
    if (ret) {
        pdata->iterm = 180;
        upm6922_err("Failed to read node of up,upm6922,iterm, default 180\n");
    }

    ret = of_property_read_u32(np, "up,upm6922,vreg", &pdata->cv);
    if (ret) {
        pdata->cv = 4200;
        upm6922_err("Failed to read node of up,upm6922,vreg, default 4200\n");
    }

    ret = of_property_read_u32(np, "up,upm6922,vindpm", &pdata->vlim);
    if (ret) {
        pdata->vlim = 4700;
        upm6922_err("Failed to read node of up,upm6922,vindpm, default 4700\n");
    }

    ret = of_property_read_u32(np, "up,upm6922,boostv", &pdata->boostv);
    if (ret) {
        pdata->boostv = 5000;
        upm6922_err("Failed to read node of up,upm6922,boostv, default 5000\n");
    }

    ret = of_property_read_u32(np, "up,upm6922,boosti", &pdata->boosti);
    if (ret) {
        pdata->boosti = 1200;
        upm6922_err("Failed to read node of up,upm6922,boosti, default 1200\n");
    }

    ret = of_property_read_u32(np, "up,upm6922,vac-ovp", &pdata->vac_ovp);
    if (ret) {
        pdata->vac_ovp = 6500;
        upm6922_err("Failed to read node of up,upm6922,vac-ovp, default 6500\n");
    }

    ret = of_property_read_u32(np, "up,upm6922,stat-pin-ctrl", &pdata->statctrl);
    if (ret) {
        pdata->statctrl = 0;
        upm6922_err("Failed to read node of up,upm6922,stat-pin-ctrl, default 0\n");
    }

    upm->en_auto_bc12 = !of_property_read_bool(np, "up,upm6922,auto-bc12-disable");

	/* mediatek boot mode */
	boot_np = of_parse_phandle(np, "boot_mode", 0);
	if (!boot_np) {
		upm6922_err("failed to get bootmode phandle\n");
	}
	tag = of_get_property(boot_np, "atag,boot", NULL);
	if (!tag) {
		upm6922_err("failed to get atag,boot\n");
	}
	upm6922_info("sz:0x%x tag:0x%x mode:0x%x type:0x%x\n",
		 tag->size, tag->tag, tag->boot_mode, tag->boot_type);
	pdata->boot_mode = tag->boot_mode;
	pdata->boot_type = tag->boot_type;

	/*
	 * mediatek bc12_sel
	 * 0 means bc12 owner is THIS_MODULE,
	 * if it is not 0, always ignore
	 */
	bc12_np = of_parse_phandle(np, "bc12_sel", 0);
	if (!bc12_np) {
		upm6922_err("failed to get bc12_sel phandle\n");
	}
	if (of_property_read_u32(bc12_np, "bc12_sel", &val) < 0) {
		upm6922_err("property bc12_sel not found\n");
	}
	
	if (val != 0)
		pdata->attach_trig = ATTACH_TRIG_IGNORE;
	else if (IS_ENABLED(CONFIG_TCPC_CLASS))
		pdata->attach_trig = ATTACH_TRIG_TYPEC;
	else
		pdata->attach_trig = ATTACH_TRIG_PWR_RDY;
			
    return pdata;
}

static void upm6922_chg_attach_pre_process(struct upm6922 *upm,
					  enum upm6922_attach_trigger trig,
					  int attach)
{
	struct upm6922_platform_data *pdata = upm->upd;
	bool bc12_dn;

	upm6922_info("upm6922_chg_attach_pre_process trig=%s,attach=%d\n",
	       upm6922_attach_trig_names[trig], attach);
	/* if attach trigger is not match, ignore it */
	if (pdata->attach_trig != trig) {
		upm6922_err("trig=%s ignored\n",
		       upm6922_attach_trig_names[trig]);
		return;
	}

	mutex_lock(&upm->attach_lock);
	if (attach == ATTACH_TYPE_NONE)
		upm->bc12_dn = false;

	bc12_dn = upm->bc12_dn;
	if (!bc12_dn)
		atomic_set(&upm->attach, attach);
	mutex_unlock(&upm->attach_lock);

	if (attach > ATTACH_TYPE_PD && bc12_dn)
		return;

	if (!queue_work(upm->wq, &upm->bc12_work))
		upm6922_info("%s bc12 work already queued\n", __func__);
}

static void upm6922_chg_pwr_rdy_process(struct upm6922 *upm)
{
	int ret;
	u8 reg_val;
	u32 val;
	int attach;

	mutex_lock(&upm->attach_lock);
	attach = atomic_read(&upm->attach);
	mutex_unlock(&upm->attach_lock);

	ret = upm6922_read_byte(upm, UPM6922_REG_08, &reg_val);
    if (ret < 0)
		return;
		
    val = (reg_val & REG08_PG_STAT_MASK) >> REG08_PG_STAT_SHIFT;
    
    if (val) {
		upm->bc12_dn = (attach != ATTACH_TYPE_NONE? true : false);
	}
    
	upm6922_chg_attach_pre_process(upm, ATTACH_TRIG_TYPEC,
				val ? ATTACH_TYPE_TYPEC : ATTACH_TYPE_NONE);
}

static int upm6922_chg_set_usbsw(struct upm6922 *upm,
				enum upm6922_usbsw usbsw)
{
	struct phy *phy;
	int ret, mode = (usbsw == USBSW_CHG) ? PHY_MODE_BC11_SET :
					       PHY_MODE_BC11_CLR;

	upm6922_info("usbsw=%d\n", usbsw);
	phy = phy_get(upm->dev, "usb2-phy");
	if (IS_ERR_OR_NULL(phy)) {
		upm6922_err("failed to get usb2-phy\n");
		return -ENODEV;
	}
	ret = phy_set_mode_ext(phy, PHY_MODE_USB_DEVICE, mode);
	if (ret)
		upm6922_err("failed to set phy ext mode\n");
	phy_put(upm->dev, phy);
	return ret;
}

static bool is_usb_rdy(struct device *dev)
{
	bool ready = true;
	struct device_node *node;

	node = of_parse_phandle(dev->of_node, "usb", 0);
	if (node) {
		ready = !of_property_read_bool(node, "cdp-block");
		upm6922_info("usb ready = %d\n", ready);
	} else
		dev_warn(dev, "usb node missing or invalid\n");
	return ready;
}

static int upm6922_chg_enable_bc12(struct upm6922 *upm, bool en)
{
	int i, ret, attach;
	static const int max_wait_cnt = 250;

	upm6922_info("en=%d\n", en);
	if (en) {
		/* CDP port specific process */
		upm6922_info("check CDP block\n");
		for (i = 0; i < max_wait_cnt; i++) {
			if (is_usb_rdy(upm->dev))
				break;
			attach = atomic_read(&upm->attach);
			if (attach == ATTACH_TYPE_TYPEC)
				msleep(100);
			else {
				upm6922_info("%s: change attach:%d, disable bc12\n",
					   __func__, attach);
				en = false;
				break;
			}
		}
		if (i == max_wait_cnt)
			upm6922_info("CDP timeout\n", __func__);
		else
			upm6922_info("CDP free\n", __func__);
	}
	ret = upm6922_chg_set_usbsw(upm, en ? USBSW_CHG : USBSW_USB);
	if (ret)
		return ret;
	return upm6922_force_dpdm(upm);
}

static void upm6922_chg_bc12_work_func(struct work_struct *work)
{
	struct upm6922 *upm = container_of(work,
						     struct upm6922,
						     bc12_work);
	struct upm6922_platform_data *pdata = upm->upd;
	bool bc12_ctrl = true, rpt_psy = true;
	int ret, attach;
	u32 val = 0;
	u8 reg_val;
	
	upm->bc12_en =false;

	mutex_lock(&upm->attach_lock);
	attach = atomic_read(&upm->attach);
	upm6922_info("attach=%d\n", attach);

	if (attach > ATTACH_TYPE_NONE && pdata->boot_mode == 5) {
		/* skip bc12 to speed up ADVMETA_BOOT */
		upm6922_info("force SDP in meta mode\n");
		upm->psy_desc.type = POWER_SUPPLY_TYPE_USB;
		upm->psy_usb_type = POWER_SUPPLY_USB_TYPE_SDP;
		goto out;
	}

	switch (attach) {
	case ATTACH_TYPE_NONE:
		upm->psy_desc.type = POWER_SUPPLY_TYPE_USB;
		upm->psy_usb_type = POWER_SUPPLY_USB_TYPE_UNKNOWN;
		goto out;
	case ATTACH_TYPE_TYPEC:
		if (!upm->bc12_dn) {
			upm->bc12_en = true;
			rpt_psy = false;
			goto out;
		}
		ret = upm6922_read_byte(upm, UPM6922_REG_08, &reg_val);
		if (ret) {
			upm6922_err("UPM6922_REG_15 fail, ret:%d\n", ret);
			rpt_psy = false;
			goto out;
		}
		val = (reg_val &REG08_VBUS_STAT_MASK) >> REG08_VBUS_STAT_SHIFT;
		break;
	case ATTACH_TYPE_PD_SDP:
		val = PORT_STAT_SDP;
		break;
	case ATTACH_TYPE_PD_DCP:
		/* not to enable bc12 */
		upm->psy_desc.type = POWER_SUPPLY_TYPE_USB_DCP;
		upm->psy_usb_type = POWER_SUPPLY_USB_TYPE_DCP;
		goto out;
	case ATTACH_TYPE_PD_NONSTD:
		val = PORT_STAT_UNKNOWN_TA;
		break;
	default:
		dev_info(upm->dev,
			 "%s: using tradtional bc12 flow!\n", __func__);
		break;
	}

	switch (val) {
	case PORT_STAT_NOINFO:
		bc12_ctrl = false;
		rpt_psy = false;
		upm6922_info("%s no info\n", __func__);
		goto out;
	case PORT_STAT_DCP:
		upm->psy_desc.type = POWER_SUPPLY_TYPE_USB_DCP;
		upm->psy_usb_type = POWER_SUPPLY_USB_TYPE_DCP;
		upm->bc12_en = true;
		break;
	case PORT_STAT_SDP:
		upm->psy_desc.type = POWER_SUPPLY_TYPE_USB;
		upm->psy_usb_type = POWER_SUPPLY_USB_TYPE_SDP;
		break;
	case PORT_STAT_CDP:
		upm->psy_desc.type = POWER_SUPPLY_TYPE_USB_CDP;
		upm->psy_usb_type = POWER_SUPPLY_USB_TYPE_CDP;
		break;
	case PORT_STAT_UNKNOWN_TA:
	case PORT_STAT_NON_STD:
		upm->psy_desc.type = POWER_SUPPLY_TYPE_USB;
		upm->psy_usb_type = POWER_SUPPLY_USB_TYPE_DCP;
		break;
	default:
		bc12_ctrl = false;
		rpt_psy = false;
		upm6922_info("Unknown port stat %d\n", val);
		goto out;
	}
	upm6922_info("port stat = %s\n", upm6922_port_stat_names[val]);
out:
	mutex_unlock(&upm->attach_lock);
	if (bc12_ctrl && (upm6922_chg_enable_bc12(upm, upm->bc12_en) < 0))
		upm6922_err("failed to set bc12 = %d\n", upm->bc12_en);
		
	if (rpt_psy)
		power_supply_changed(upm->psy);
}

static irqreturn_t upm6922_irq_handler(int irq, void *data)
{
    int ret;
    u8 reg_val;
    bool prev_pg;
    bool prev_va;
    int attach;
    struct upm6922 *upm = (struct upm6922 *)data;

    upm6922_info("upm6922_irq_handler enter\n");

	mutex_lock(&upm->attach_lock);
	attach = atomic_read(&upm->attach);
	mutex_unlock(&upm->attach_lock);

    ret = upm6922_read_byte(upm, UPM6922_REG_0A, &reg_val);
    if (ret) {
        upm6922_err("read UPM6922_REG_0A fail, ret:%d\n", ret);
        return IRQ_HANDLED;
    }

    prev_va = upm->vbus_attach;
    upm->vbus_attach = !!(reg_val & REG0A_VBUS_GD_MASK);

    ret = upm6922_read_byte(upm, UPM6922_REG_08, &reg_val);
    if (ret) {
        upm6922_err("read UPM6922_REG_08 fail, ret:%d\n", ret);
        return IRQ_HANDLED;
    }

    prev_pg = upm->power_good;
    upm->power_good = !!(reg_val & REG08_PG_STAT_MASK);

    if (!prev_va && upm->vbus_attach) {
        //Charger_Detect_Init();
        upm6922_err("adapter/usb inserted\n");
        upm6922_set_input_volt_limit(upm, upm->upd->vlim);
    }
    
    upm6922_info("upm6922_irq_handler attach=%d,bc12_dn=%d,bc12_en=%d,power_good=%d\n",attach,upm->bc12_dn,upm->bc12_en,upm->power_good);
    if (upm->power_good && attach == ATTACH_TYPE_TYPEC && !upm->bc12_dn && upm->bc12_en) {
		upm6922_err("upm6922_chg_pwr_rdy_process\n");
		upm6922_chg_pwr_rdy_process(upm);
	}

    return IRQ_HANDLED;
}

static int upm6922_register_interrupt(struct upm6922 *upm)
{
    int ret = 0;

    if (upm->intr_gpio < 0) {
        upm6922_err("no intr gpio, skip register interrupt\n");
        return 0;
    }

    ret = devm_gpio_request_one(upm->dev, upm->intr_gpio, GPIOF_DIR_IN,
            devm_kasprintf(upm->dev, GFP_KERNEL,
            "upm6922_intr_gpio.%s", dev_name(upm->dev)));
    if (ret < 0) {
        upm6922_err("gpio request fail(%d)\n", ret);
        return ret;
    }

    upm->client->irq = gpio_to_irq(upm->intr_gpio);
    if (upm->client->irq < 0) {
        upm6922_err("gpio2irq fail(%d)\n", upm->client->irq);
        return upm->client->irq;
    }

    ret = devm_request_threaded_irq(upm->dev, upm->client->irq, NULL,
                    upm6922_irq_handler,
                    IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
                    "upm_irq", upm);
    if (ret < 0) {
        upm6922_err("request thread irq failed:%d\n", ret);
        return ret;
    }

    return 0;
}

static int upm6922_init_device(struct upm6922 *upm)
{
    int ret;

    upm6922_disable_watchdog_timer(upm);

    ret = upm6922_set_input_current_limit(upm, upm->upd->ilim);
    if (ret) {
        upm6922_err("Failed to set iindpm, ret = %d\n", ret);
    }

    ret = upm6922_set_chargecurrent(upm, upm->upd->ichg);
    if (ret) {
        upm6922_err("Failed to set ichg, ret = %d\n", ret);
    }

    ret = upm6922_set_prechg_current(upm, upm->upd->iprechg);
    if (ret) {
        upm6922_err("Failed to set prechg current, ret = %d\n", ret);
    }

    ret = upm6922_set_term_current(upm, upm->upd->iterm);
    if (ret) {
        upm6922_err("Failed to set termination current, ret = %d\n", ret);
    }

    ret = upm6922_set_chargevolt(upm, upm->upd->cv);
    if (ret) {
        upm6922_err("Failed to set cv, ret = %d\n", ret);
    }

    ret = upm6922_set_input_volt_limit(upm, upm->upd->vlim);
    if (ret) {
        upm6922_err("Failed to set vlim, ret = %d\n", ret);
    }

    ret = upm6922_set_boost_voltage(upm, upm->upd->boostv);
    if (ret) {
        upm6922_err("Failed to set boost voltage, ret = %d\n", ret);
    }

    ret = upm6922_set_boost_current(upm, upm->upd->boosti);
    if (ret) {
        upm6922_err("Failed to set boost current, ret = %d\n", ret);
    }

    ret = upm6922_set_acovp_threshold(upm, upm->upd->vac_ovp);
    if (ret) {
        upm6922_err("Failed to set acovp threshold, ret = %d\n", ret);
    }

    ret = upm6922_set_stat_ctrl(upm, upm->upd->statctrl);
    if (ret) {
        upm6922_err("Failed to set stat pin control mode, ret = %d\n", ret);
    }

    ret = upm6922_enable_vindpm_int(upm, false);
    if (ret) {
        upm6922_err("Failed to mask vindpm int, ret = %d\n", ret);
    }

    ret = upm6922_enable_iindpm_int(upm, false);
    if (ret) {
        upm6922_err("Failed to mask iindpm int, ret = %d\n", ret);
    }

    return 0;
}

static void upm6922_inform_prob_dwork_handler(struct work_struct *work)
{
    struct upm6922 *upm = container_of(work, struct upm6922,
                                prob_dwork.work);
    //Charger_Detect_Init();
    upm6922_force_dpdm(upm);
}

#ifndef __MTK_CHARGER_TYPE_H__
#define __MTK_CHARGER_TYPE_H__
enum charger_type {
    CHARGER_UNKNOWN = 0,
    STANDARD_HOST,
    CHARGING_HOST,
    NONSTANDARD_CHARGER,
    STANDARD_CHARGER,
    APPLE_2_1A_CHARGER,
    APPLE_1_0A_CHARGER,
    APPLE_0_5A_CHARGER,
    WIRELESS_CHARGER,
};
#endif /* __MTK_CHARGER_TYPE_H__ */

static void upm6922_inform_psy_dwork_handler(struct work_struct *work)
{
    struct upm6922 *upm = container_of(work, struct upm6922,
                                psy_dwork.work);
    int ret = 0;
    union power_supply_propval propval;
    int vbus_stat = 0;
    u8 reg_val = 0;

    if (!upm->inform_psy) {
        upm->inform_psy = power_supply_get_by_name("charger");
        if (!upm->inform_psy) {
            upm6922_err("%s Couldn't get psy\n", __func__);
            mod_delayed_work(system_wq, &upm->psy_dwork,
                    msecs_to_jiffies(2*1000));
            return;
        }
    }

    propval.intval = upm->psy_usb_type ? 1 : 0;
    ret = power_supply_set_property(upm->inform_psy,
                POWER_SUPPLY_PROP_ONLINE, &propval);
    if (ret < 0) {
        upm6922_err("inform power supply online failed:%d\n", ret);
    }

    ret = upm6922_read_byte(upm, UPM6922_REG_08, &reg_val);
    if (ret) {
        upm6922_err("read UPM6922_REG_08 fail, ret:%d\n", ret);
        return ;
    }

    vbus_stat = (reg_val & REG08_VBUS_STAT_MASK);
    vbus_stat >>= REG08_VBUS_STAT_SHIFT;

    switch (vbus_stat) {
        case REG08_VBUS_TYPE_NONE:
            propval.intval = CHARGER_UNKNOWN;
            break;

        case REG08_VBUS_TYPE_SDP:
            propval.intval = STANDARD_HOST;
            break;

        case REG08_VBUS_TYPE_CDP:
            propval.intval = CHARGING_HOST;
            break;

        case REG08_VBUS_TYPE_DCP:
        case REG08_VBUS_TYPE_NON_STD:
            propval.intval = STANDARD_CHARGER;
            break;

        case REG08_VBUS_TYPE_UNKNOWN:
            propval.intval = NONSTANDARD_CHARGER;
            break;

        default:
            upm6922_err("is otg? vbus_stat:%d\n", vbus_stat);
            propval.intval = CHARGER_UNKNOWN;
            break;
    }

    ret = power_supply_set_property(upm->inform_psy,
                POWER_SUPPLY_PROP_CHARGE_TYPE, &propval);
    if (ret < 0) {
        upm6922_err("inform power supply charge type failed:%d\n", ret);
    }
}

static void upm6922_monitor_dwork_handler(struct work_struct *work)
{
    struct upm6922 *upm = container_of(work, struct upm6922,
                                monitor_dwork.work);

    __pm_stay_awake(upm->monitor_ws);

    upm6922_info("monitor work\n");
    upm6922_dump_regs(upm);

    schedule_delayed_work(&upm->monitor_dwork, msecs_to_jiffies(10*1000));

    __pm_relax(upm->monitor_ws);
}

static ssize_t upm6922_show_registers(struct device *dev, 
                    struct device_attribute *attr, char *buf)
{
    struct upm6922 *upm = dev_get_drvdata(dev);
    u8 addr;
    u8 val;
    u8 tmpbuf[200];
    int len;
    int idx = 0;
    int ret;

    idx = snprintf(buf, PAGE_SIZE, "%s:\n", "upm6922 Reg");
    for (addr = 0x0; addr <= 0x16; addr++) {
        ret = upm6922_read_byte(upm, addr, &val);
        if (ret == 0) {
            len = snprintf(tmpbuf, PAGE_SIZE - idx,
                       "Reg[%.2x] = 0x%.2x\n", addr, val);
            memcpy(&buf[idx], tmpbuf, len);
            idx += len;
        }
    }

    return idx;
}

static ssize_t upm6922_store_registers(struct device *dev,
            struct device_attribute *attr, const char *buf, size_t count)
{
    struct upm6922 *upm = dev_get_drvdata(dev);
    int ret;
    unsigned int reg;
    unsigned int val;

    ret = sscanf(buf, "%x %x", &reg, &val);
    if (ret == 2 && reg <= 0x16) {
        upm6922_write_byte(upm, (unsigned char) reg,
                   (unsigned char) val);
    }

    return count;
}

static DEVICE_ATTR(registers, S_IRUGO | S_IWUSR, upm6922_show_registers,
           upm6922_store_registers);

static struct attribute *upm6922_attributes[] = {
    &dev_attr_registers.attr,
    NULL,
};

static const struct attribute_group upm6922_attr_group = {
    .attrs = upm6922_attributes,
};

static enum power_supply_usb_type upm6922_usb_types[] = {
    POWER_SUPPLY_USB_TYPE_UNKNOWN,
    POWER_SUPPLY_USB_TYPE_SDP,
    POWER_SUPPLY_USB_TYPE_CDP,
    POWER_SUPPLY_USB_TYPE_DCP,
};

static enum power_supply_property upm6922_charger_props[] = {
    POWER_SUPPLY_PROP_STATUS,
    POWER_SUPPLY_PROP_HEALTH,
    POWER_SUPPLY_PROP_ONLINE,
    POWER_SUPPLY_PROP_USB_TYPE,
    POWER_SUPPLY_PROP_MANUFACTURER,
    POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT,
    POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE,
    POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT,
};

static int upm6922_charger_get_health(struct upm6922 *upm,
		union power_supply_propval *val)
{
	u8 reg_val;
	int health;
	int ret;
	
	ret = upm6922_read_byte(upm, UPM6922_REG_09, &reg_val);
    if (ret) {
        upm6922_err("read UPM6922_REG_09 fail, ret:%d\n", ret);
    }
    if (reg_val) {
        upm6922_info("UPM6922_REG_09:0x%x\n", reg_val);
    }
    
	if (reg_val & REG09_FAULT_NTC_MASK) {
		switch (reg_val >> REG09_FAULT_NTC_SHIFT & 0x7) {
		case 0x3:
		case 0x5: 
			health = POWER_SUPPLY_HEALTH_COLD;
			break;
		case 0x2:
		case 0x6:
			health = POWER_SUPPLY_HEALTH_OVERHEAT;
			break;
		default:
			health = POWER_SUPPLY_HEALTH_UNKNOWN;
		}
	} else if (reg_val & REG09_FAULT_BAT_MASK) {
		health = POWER_SUPPLY_HEALTH_OVERVOLTAGE;
	} else if (reg_val & REG09_FAULT_CHRG_MASK) {
		switch (reg_val >> REG09_FAULT_CHRG_SHIFT & 0x3) {
		case 0x1: /* Input Fault (VBUS OVP or VBAT<VBUS<3.8V) */
			/*
			 * This could be over-voltage or under-voltage
			 * and there's no way to tell which.  Instead
			 * of looking foolish and returning 'OVERVOLTAGE'
			 * when its really under-voltage, just return
			 * 'UNSPEC_FAILURE'.
			 */
			health = POWER_SUPPLY_HEALTH_UNSPEC_FAILURE;
			break;
		case 0x2: /* Thermal Shutdown */
			health = POWER_SUPPLY_HEALTH_OVERHEAT;
			break;
		case 0x3: /* Charge Safety Timer Expiration */
			health = POWER_SUPPLY_HEALTH_SAFETY_TIMER_EXPIRE;
			break;
		default:  /* prevent compiler warning */
			health = -1;
		}
	} else if (reg_val & REG09_FAULT_BOOST_MASK) {
		/*
		 * This could be over-current or over-voltage but there's
		 * no way to tell which.  Return 'OVERVOLTAGE' since there
		 * isn't an 'OVERCURRENT' value defined that we can return
		 * even if it was over-current.
		 */
		health = POWER_SUPPLY_HEALTH_OVERVOLTAGE;
	} else {
		health = POWER_SUPPLY_HEALTH_GOOD;
	}

	val->intval = health;

	return 0;
}

static int upm6922_charger_get_precharge(struct upm6922 *upm,
		union power_supply_propval *val)
{
	int ma = 0, ret;

	ret = upm6922_get_prechg_current(upm, &ma);
	if (ret) {
        upm6922_err("get prechg current fail, ret:%d\n", ret);
        return ret;
    }
	val->intval =  ma;
	return 0;
}

static int upm6922_charger_get_charge_term(struct upm6922 *upm,
		union power_supply_propval *val)
{
	int ma = 0, ret;

	ret = upm6922_get_term_current(upm, &ma);
	if (ret) {
        upm6922_err("get term current fail, ret:%d\n", ret);
        return ret;
    }
	val->intval =  ma;
	return 0;
}

static int upm6922_charger_get_current(struct upm6922 *upm,
		union power_supply_propval *val)
{
	int ma = 0, ret;

	ret = upm6922_get_chargecurrent(upm, &ma);
	if (ret) {
        upm6922_err("get ichg fail, ret:%d\n", ret);
        return ret;
    }
	val->intval = ma;
	return 0;
}

static int upm6922_charger_get_voltage(struct upm6922 *upm,
		union power_supply_propval *val)
{
	int mv = 0, ret;

	ret = upm6922_get_chargevolt(upm, &mv);
	if (ret) {
        upm6922_err("get charge voltage fail, ret:%d\n", ret);
        return ret;
    }
	val->intval = mv;
	return 0;
}

static int upm6922_charger_get_iinlimit(struct upm6922 *upm,
		union power_supply_propval *val)
{
	int ma = 0, ret;

	ret = upm6922_get_input_current_limit(upm, &ma);
	if (ret) {
        upm6922_err("get input current limit fail, ret:%d\n", ret);
        return ret;
    }
	val->intval = ma;
	return 0;
}


static int upm6922_charger_get_property(struct power_supply *psy,
                enum power_supply_property psp,
                union power_supply_propval *val)
{
    struct upm6922 *upm = power_supply_get_drvdata(psy);
    int ret = 0,attach;

    val->intval = 0;
    switch (psp) {
        case POWER_SUPPLY_PROP_STATUS:
            ret = upm6922_get_charging_stat(upm, &val->intval);
            if (!ret && val->intval == POWER_SUPPLY_STATUS_NOT_CHARGING) {
				attach = atomic_read(&upm->attach);
				if (attach) {
					val->intval = POWER_SUPPLY_STATUS_CHARGING;
				}
			}
            break;

        case POWER_SUPPLY_PROP_ONLINE:
            val->intval = atomic_read(&upm->attach);
            break;
            
		case POWER_SUPPLY_PROP_HEALTH:
			ret = upm6922_charger_get_health(upm,val);
			break;
		case POWER_SUPPLY_PROP_PRECHARGE_CURRENT:
			ret = upm6922_charger_get_precharge(upm, val);
			break;
		case POWER_SUPPLY_PROP_CHARGE_TERM_CURRENT:
			ret = upm6922_charger_get_charge_term(upm, val);
			break;
		case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT:
			ret = upm6922_charger_get_current(upm, val);
			break;
		case POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE:
			ret = upm6922_charger_get_voltage(upm, val);
			break;
		case POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT:
			ret = upm6922_charger_get_iinlimit(upm, val);
			break;
        case POWER_SUPPLY_PROP_USB_TYPE:
            val->intval = upm->psy_usb_type;
            break;
		case POWER_SUPPLY_PROP_TYPE:
			val->intval = upm->psy_desc.type;
		break;
        case POWER_SUPPLY_PROP_MANUFACTURER:
            val->strval = "Unisemipower";
            break;

        default:
            break;
    }
    
	upm6922_info("%s psp:%d ret:%d val:%d",
		__func__, psp, ret, val->intval);
		
    return ret;
}

static int upm6922_charger_set_current(struct upm6922 *upm,
		const union power_supply_propval *val)
{
	return upm6922_set_chargecurrent(upm,val->intval);
}

static int upm6922_charger_set_voltage(struct upm6922 *upm,
		const union power_supply_propval *val)
{
	return upm6922_set_chargevolt(upm,val->intval);
}

static int upm6922_charger_set_iinlimit(struct upm6922 *upm,
		const union power_supply_propval *val)
{
	return upm6922_set_input_current_limit(upm,val->intval);
}

static int upm6922_charger_set_property(struct power_supply *psy,
                    enum power_supply_property psp,
                    const union power_supply_propval *val)
{
    int ret = 0;
	struct upm6922 *upm = power_supply_get_drvdata(psy);

	switch (psp) {
	case POWER_SUPPLY_PROP_ONLINE:
		upm6922_chg_attach_pre_process(upm, ATTACH_TRIG_TYPEC,
					      val->intval);
		break;
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT:
		ret = upm6922_charger_set_current(upm, val);
		break;
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE:
		ret = upm6922_charger_set_voltage(upm, val);
		break;
	case POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT:
		ret = upm6922_charger_set_iinlimit(upm, val);
		break;
		
	default:
		ret = -EINVAL;
	}
	return ret;
}

static int upm6922_charger_is_writeable(struct power_supply *psy,
                    enum power_supply_property psp)
{
	switch (psp) {
		case POWER_SUPPLY_PROP_ONLINE:
		case POWER_SUPPLY_PROP_HEALTH:
		case POWER_SUPPLY_PROP_TEMP_ALERT_MAX:
		case POWER_SUPPLY_PROP_CHARGE_TYPE:
		case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT:
		case POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE:
		case POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT:
			return 1;
		default:
			return 0;
	}
}

static char *upm6922_charger_supplied_to[] = {
    "battery",
    "mtk-master-charger",
};

static int upm6922_charger_psy_register(struct upm6922 *upm)
{
    upm->psy_cfg.drv_data = upm;
    upm->psy_cfg.of_node = upm->dev->of_node;
    upm->psy_cfg.supplied_to = upm6922_charger_supplied_to;
    upm->psy_cfg.num_supplicants = ARRAY_SIZE(upm6922_charger_supplied_to);

    upm->psy_desc.name = "upm6922-charger";
    upm->psy_desc.type = POWER_SUPPLY_TYPE_MAINS;
    upm->psy_desc.properties = upm6922_charger_props;
    upm->psy_desc.num_properties = ARRAY_SIZE(upm6922_charger_props);
    upm->psy_desc.get_property = upm6922_charger_get_property;
    upm->psy_desc.set_property = upm6922_charger_set_property;
    upm->psy_desc.property_is_writeable = upm6922_charger_is_writeable;
    upm->psy_desc.usb_types = upm6922_usb_types;
    upm->psy_desc.num_usb_types = ARRAY_SIZE(upm6922_usb_types);

    upm->psy = devm_power_supply_register(upm->dev, 
            &upm->psy_desc, &upm->psy_cfg);
    if (IS_ERR(upm->psy)) {
        upm6922_err("failed to register charger_psy\n");
        return PTR_ERR(upm->psy);
    }

    upm6922_err("%s power supply register successfully\n", upm->psy_desc.name);

    return 0;
}

/*************************mtk interface start*************************/
#if IS_ENABLED(CONFIG_MTK_CHARGER)
static int upm6922_ops_charging(struct charger_device *chg_dev, bool enable)
{
    struct upm6922 *upm = dev_get_drvdata(&chg_dev->dev);
    int ret = 0;

    if (enable) {
        ret = upm6922_enable_charger(upm);
    } else {
        ret = upm6922_disable_charger(upm);
    }

    return ret;
}

static int upm6922_ops_plug_in(struct charger_device *chg_dev)
{
    int ret;

    ret = upm6922_ops_charging(chg_dev, true);
    if (ret) {
        upm6922_err("Failed to enable charging:%d\n", ret);
    }

    return ret;
}

static int upm6922_ops_plug_out(struct charger_device *chg_dev)
{
    int ret;

    ret = upm6922_ops_charging(chg_dev, false);
    if (ret) {
        upm6922_err("Failed to disable charging:%d\n", ret);
    }

    return ret;
}

static int upm6922_ops_dump_register(struct charger_device *chg_dev)
{
    struct upm6922 *upm = dev_get_drvdata(&chg_dev->dev);

    upm6922_dump_regs(upm);

    return 0;
}

static int upm6922_ops_is_charging_enable(struct charger_device *chg_dev, bool *en)
{
    struct upm6922 *upm = dev_get_drvdata(&chg_dev->dev);
    int ret = 0;
    u8 val = 0;

    ret = upm6922_read_byte(upm, UPM6922_REG_01, &val);
    if (ret) {
        upm6922_err("read UPM6922_REG_01 fail, ret:%d\n", ret);
        return ret;
    }

    *en = !!(val & REG01_CHG_CONFIG_MASK);

    return 0;
}

static int upm6922_ops_get_ichg(struct charger_device *chg_dev, u32 *ua)
{
    struct upm6922 *upm = dev_get_drvdata(&chg_dev->dev);
    int ma = 0;
    int ret = 0;

    ret = upm6922_get_chargecurrent(upm, &ma);
    if (ret) {
        upm6922_err("get ichg fail, ret:%d\n", ret);
        return ret;
    }
    *ua = ma * 1000;

    return ret;
}

static int upm6922_ops_set_ichg(struct charger_device *chg_dev, u32 ua)
{
    struct upm6922 *upm = dev_get_drvdata(&chg_dev->dev);

    return upm6922_set_chargecurrent(upm, ua / 1000);
}

static int upm6922_ops_get_icl(struct charger_device *chg_dev, u32 *ua)
{
    struct upm6922 *upm = dev_get_drvdata(&chg_dev->dev);
    int ma = 0;
    int ret = 0;

    ret = upm6922_get_input_current_limit(upm, &ma);
    if (ret) {
        upm6922_err("get icl fail, ret:%d\n", ret);
        return ret;
    }

    *ua = ma * 1000;

    return ret;
}

static int upm6922_ops_set_icl(struct charger_device *chg_dev, u32 ua)
{
    struct upm6922 *upm = dev_get_drvdata(&chg_dev->dev);

    return upm6922_set_input_current_limit(upm, ua / 1000);
}

static int upm6922_ops_get_vchg(struct charger_device *chg_dev, u32 *uv)
{
    struct upm6922 *upm = dev_get_drvdata(&chg_dev->dev);
    int mv;
    int ret;

    ret = upm6922_get_chargevolt(upm, &mv);
    if (ret) {
        upm6922_err("get charge voltage fail, ret:%d\n", ret);
        return ret;
    }
    *uv = mv * 1000;

    return ret;
}

static int upm6922_ops_set_vchg(struct charger_device *chg_dev, u32 uv)
{
    struct upm6922 *upm = dev_get_drvdata(&chg_dev->dev);

    return upm6922_set_chargevolt(upm, uv / 1000);
}

static int upm6922_ops_kick_wdt(struct charger_device *chg_dev)
{
    struct upm6922 *upm = dev_get_drvdata(&chg_dev->dev);

    return upm6922_reset_watchdog_timer(upm);
}

static int upm6922_ops_set_ivl(struct charger_device *chg_dev, u32 uv)
{
    struct upm6922 *upm = dev_get_drvdata(&chg_dev->dev);

    return upm6922_set_input_volt_limit(upm, uv / 1000);
}

static int upm6922_ops_is_charging_done(struct charger_device *chg_dev, bool *done)
{
    struct upm6922 *upm = dev_get_drvdata(&chg_dev->dev);
    int ret;
    u8 val;

    ret = upm6922_read_byte(upm, UPM6922_REG_08, &val);
    if (!ret) {
        val = val & REG08_CHRG_STAT_MASK;
        val = val >> REG08_CHRG_STAT_SHIFT;
        *done = (val == REG08_CHRG_STAT_CHGDONE);
    }

    return ret;
}

static int upm6922_ops_get_min_ichg(struct charger_device *chg_dev, u32 *ua)
{
    *ua = REG02_ICHG_LSB * 1000;

    return 0;
}

static int upm6922_ops_set_safety_timer(struct charger_device *chg_dev, bool en)
{
    struct upm6922 *upm = dev_get_drvdata(&chg_dev->dev);
    int ret;

    if (en) {
        ret = upm6922_enable_safety_timer(upm);
    } else {
        ret = upm6922_disable_safety_timer(upm);
    }

    return ret;
}

static int upm6922_ops_is_safety_timer_enabled(struct charger_device *chg_dev,
                       bool *en)
{
    struct upm6922 *upm = dev_get_drvdata(&chg_dev->dev);
    int ret;
    u8 reg_val;

    ret = upm6922_read_byte(upm, UPM6922_REG_05, &reg_val);

    if (!ret) {
        *en = !!(reg_val & REG05_EN_TIMER_MASK);
    }

    return ret;
}

static int upm6922_ops_set_otg(struct charger_device *chg_dev, bool en)
{
    int ret;
    struct upm6922 *upm = dev_get_drvdata(&chg_dev->dev);

    if (en) {
        ret = upm6922_enable_otg(upm);
    } else {
        ret = upm6922_disable_otg(upm);
    }

    return ret;
}

static int upm6922_ops_set_boost_ilmt(struct charger_device *chg_dev, u32 ua)
{
    struct upm6922 *upm = dev_get_drvdata(&chg_dev->dev);
    int ret;

    ret = upm6922_set_boost_current(upm, ua / 1000);

    return ret;
}

static const struct charger_properties upm6922_chg_props = {
    .alias_name = "upm6922",
};

static int upm6922_do_event(struct charger_device *chg_dev, unsigned int event, unsigned int args)
{
	struct upm6922 *upm = charger_get_data(chg_dev);
	if (chg_dev == NULL)
		return -EINVAL;

	upm6922_err("[upm6922] %s: event = %d\n", __func__, event);

	switch (event) {
	case EVENT_FULL:
	case EVENT_RECHARGE:
	case EVENT_DISCHARGE:
		power_supply_changed(upm->psy);
		break;
	default:
		break;
	}

	return 0;
}

static int upm6922_get_adc(struct charger_device *chgdev, enum adc_channel chan, int *min, int *max);
static int upm6922_get_vbus(struct charger_device *chgdev, u32 *vbus)
{
	return upm6922_get_adc(chgdev, ADC_CHANNEL_VBUS, vbus, vbus);
}

static int upm6922_get_vbat(struct charger_device *chgdev, u32 *vbat)
{
	return upm6922_get_adc(chgdev, ADC_CHANNEL_VBAT, vbat, vbat);
}

#define R_CHARGER_1 330
#define R_CHARGER_2 39

static int upm6922_get_ibat(struct charger_device *chgdev, u32 *ibat)
{
	return upm6922_get_adc(chgdev,ADC_CHANNEL_IBAT,ibat,ibat);
}

static int upm6922_get_ibus(struct charger_device *chgdev, u32 *ibus)
{
	return upm6922_get_adc(chgdev,ADC_CHANNEL_IBUS,ibus,ibus);
}

static int upm6922_get_adc(struct charger_device *chgdev, enum adc_channel chan,
			  int *min, int *max)
{
	int ret = 0;
	int temp;
	u32 temp_vbus = 0;
	u32 temp_vbat = 0;
	struct power_supply *psy;
	struct mtk_charger *info;
	enum upm6922_adc_chan adc_chan = ADC_CHAN_NOT_SUPPORT;
	struct upm6922 *upm = charger_get_data(chgdev);
	if (IS_ERR_OR_NULL(upm)) {
		upm6922_err("%s:failed to get upm\n", __func__);
		upm = g_upm;
	}
	//upm6922_info("%s:carrot start chan=%d\n", __func__,chan);
	mutex_unlock(&upm->adc_lock);
	switch (chan) {
	case ADC_CHANNEL_VBUS:
		adc_chan = ADC_CHAN_VBUS;
		ret = iio_read_channel_processed(&upm->iio_adcs[adc_chan], min);
		if (ret < 0) {
			upm6922_err("failed to read adc adc_chan=%d\n",adc_chan);
			goto out;
		}
		temp = *min;
		*max = *min = (((R_CHARGER_1+R_CHARGER_2)*100*temp)/R_CHARGER_2)/100 *1000;
		break;
	case ADC_CHANNEL_VSYS:
	case ADC_CHANNEL_VBAT:
		adc_chan = ADC_CHAN_VBAT;
		ret = iio_read_channel_processed(&upm->iio_adcs[adc_chan], min);
		if (ret < 0) {
			upm6922_err("failed to read adc adc_chan=%d\n",adc_chan);
			goto out;
		}
		temp = *min;
		*max = *min = temp *1000;
		break;
	case ADC_CHANNEL_IBUS:
		adc_chan = ADC_CHAN_IBUS;
		upm6922_get_vbus(chgdev,&temp_vbus);
		temp_vbus = temp_vbus/1000;
		upm6922_get_vbat(chgdev,&temp_vbat);
		temp_vbat = temp_vbat/1000;
		ret = upm6922_get_ibat(chgdev,min);
		if (ret < 0) {
			upm6922_err("failed to read adc chan=%d\n",chan);
			goto out;
		} else {
			temp = (temp_vbat * (*min/1000 + 500)) / temp_vbus;
			*max = *min = temp *1000;
		}
		break;
	case ADC_CHANNEL_IBAT:
		{
		adc_chan = ADC_CHAN_IBAT;
		psy = power_supply_get_by_name("mtk-master-charger");
		if (psy == NULL) {
			upm6922_err("get master charger psy fail!\n");
			ret = -1;
			goto out;
		}
			
		info = (struct mtk_charger *)power_supply_get_drvdata(psy);
		if (info == NULL) {
			upm6922_err("get mtk_charger info fail!\n");
			ret = -1;
			goto out; 
		}
		temp = get_battery_current(info) * 1000;
		if(temp < 0)
			temp = 10;
			
		*max = *min = temp;	
		}
		break;
	default:
		ret = -ENOTSUPP;
		break;
	}
	
	//upm6922_info("%s:get adc %s max=%d,min=%d",__func__,upm6922_adc_name[adc_chan],*max, *min);
out:
	mutex_unlock(&upm->adc_lock);
	return ret;	
}

static int upm6922_enable_chg_type_det(struct charger_device *chgdev, bool en)
{
	struct upm6922 *upm = charger_get_data(chgdev);
	int attach = en ? ATTACH_TYPE_TYPEC : ATTACH_TYPE_NONE;

	upm6922_info("%s:carrot start en=%d\n", __func__,en);
	upm6922_chg_attach_pre_process(upm, ATTACH_TRIG_TYPEC, attach);
	return 0;
}

static struct charger_ops upm6922_chg_ops = {
    /* Normal charging */
    .plug_in = upm6922_ops_plug_in,
    .plug_out = upm6922_ops_plug_out,
    .dump_registers = upm6922_ops_dump_register,
    .enable = upm6922_ops_charging,
    .is_enabled = upm6922_ops_is_charging_enable,
    .get_charging_current = upm6922_ops_get_ichg,
    .set_charging_current = upm6922_ops_set_ichg,
    .get_input_current = upm6922_ops_get_icl,
    .set_input_current = upm6922_ops_set_icl,
    .get_constant_voltage = upm6922_ops_get_vchg,
    .set_constant_voltage = upm6922_ops_set_vchg,
	/* ADC */
	.get_adc = upm6922_get_adc,
	.get_vbus_adc = upm6922_get_vbus,
	.get_ibus_adc = upm6922_get_ibus,
	.get_ibat_adc = upm6922_get_ibat,
    .kick_wdt = upm6922_ops_kick_wdt,
    .set_mivr = upm6922_ops_set_ivl,
    .is_charging_done = upm6922_ops_is_charging_done,
    .get_min_charging_current = upm6922_ops_get_min_ichg,

    /* Safety timer */
    .enable_safety_timer = upm6922_ops_set_safety_timer,
    .is_safety_timer_enabled = upm6922_ops_is_safety_timer_enabled,

    /* Power path */
    .enable_powerpath = NULL,
    .is_powerpath_enabled = NULL,

    /* OTG */
    .enable_otg = upm6922_ops_set_otg,
    .set_boost_current_limit = upm6922_ops_set_boost_ilmt,
    .enable_discharge = NULL,

    /* PE+/PE+20 */
    .send_ta_current_pattern = NULL,
    .set_pe20_efficiency_table = NULL,
    .send_ta20_current_pattern = NULL,
    .enable_cable_drop_comp = NULL,

   /* Event */
    .event = upm6922_do_event,
    /* charger type detection */
	.enable_chg_type_det = upm6922_enable_chg_type_det,
	
	.enable_hz = upm6922_set_en_hiz,
};

static int upm6922_enable_vbus(struct regulator_dev *rdev)
{
    int ret;
    struct upm6922 *upm = rdev_get_drvdata(rdev);

    ret = upm6922_enable_otg(upm);

    return 0;
}

static int upm6922_disable_vbus(struct regulator_dev *rdev)
{
    int ret;
    struct upm6922 *upm = rdev_get_drvdata(rdev);

    ret = upm6922_disable_otg(upm);

    return 0;
}

static int upm6922_is_enabled_vbus(struct regulator_dev *rdev)
{
    int ret;
    struct upm6922 *upm = rdev_get_drvdata(rdev);
    u8 val = 0;

    ret = upm6922_read_byte(upm, UPM6922_REG_01, &val);
    if (ret < 0)
        return ret;

    return (val & REG01_OTG_CONFIG_MASK) >> REG01_OTG_CONFIG_SHIFT;
}

static const struct regulator_ops upm6922_vbus_ops = {
    .enable = upm6922_enable_vbus,
    .disable = upm6922_disable_vbus,
    .is_enabled = upm6922_is_enabled_vbus,
};

static const struct regulator_desc upm6922_otg_rdesc = {
    .of_match = "upm6922,otg-vbus",
    .name = "usb-otg-vbus",
    .ops = &upm6922_vbus_ops,
    .owner = THIS_MODULE,
    .type = REGULATOR_VOLTAGE,
    .fixed_uV = 5000000,
    .n_voltages = 1,
};
#endif
/**************************mtk interface end**************************/

static int upm6922_get_iio_adc(struct upm6922 *upm)
{
	upm6922_info("%s:carrot start \n", __func__);
	upm->iio_adcs = devm_iio_channel_get_all(upm->dev);
	if (IS_ERR(upm->iio_adcs))
		return PTR_ERR(upm->iio_adcs);
	return 0;
}


static int upm6922_charger_probe(struct i2c_client *client,
                 const struct i2c_device_id *id)
{
    struct upm6922 *upm;
    struct device_node *node = client->dev.of_node;
    int ret = 0;
    __maybe_unused struct regulator_config config = { };

    upm = devm_kzalloc(&client->dev, sizeof(struct upm6922), GFP_KERNEL);
    if (!upm) {
        upm6922_err("alloc upm6922 struct failed\n");
        return -ENOMEM;
    }
	g_upm = upm;
    client->addr = 0x6B;
    upm->dev = &client->dev;
    upm->client = client;

    i2c_set_clientdata(client, upm);
    mutex_init(&upm->i2c_rw_lock);
    mutex_init(&upm->adc_lock);
    mutex_init(&upm->attach_lock);

    ret = upm6922_detect_device(upm);
    if (ret) {
        upm6922_err("No upm6922 device found!\n");
        ret = -ENODEV;
        goto err_nodev;
    }

    if (upm->part_no != REG0B_PN_UPM6922) {
        upm6922_err("part no not match, not upm6922!\n");
        ret = -ENODEV;
        goto err_nodev;
    }

    upm->upd = upm6922_parse_dt(node, upm);
    if (!upm->upd) {
        upm6922_err("No platform data provided.\n");
        ret = -EINVAL;
        goto err_parse_dt;
    }

    INIT_DELAYED_WORK(&upm->prob_dwork, upm6922_inform_prob_dwork_handler);
    INIT_DELAYED_WORK(&upm->psy_dwork, upm6922_inform_psy_dwork_handler);

    upm->monitor_ws = wakeup_source_register(upm->dev, "upm6922_monitor_ws");
    INIT_DELAYED_WORK(&upm->monitor_dwork, upm6922_monitor_dwork_handler);
    
    upm->wq = create_singlethread_workqueue(dev_name(upm->dev));
	if (!upm->wq) {
		upm6922_err("failed to create workqueue\n");
		ret = -ENOMEM;
		goto err_parse_dt;
	}
	INIT_WORK(&upm->bc12_work, upm6922_chg_bc12_work_func);

    ret = upm6922_init_device(upm);
    if (ret) {
        upm6922_err("Failed to init device\n");
        ret = -EINVAL;
        goto err_dev_init;
    }
    
    ret = upm6922_get_iio_adc(upm);
	if (ret < 0) {
		upm6922_err("failed to get iio adc\n");
		goto err_dev_init;
	}
    
    upm6922_dump_regs(upm);

    upm6922_register_interrupt(upm);

    ret = sysfs_create_group(&upm->dev->kobj, &upm6922_attr_group);
    if (ret)
        upm6922_err("failed to register sysfs. err: %d\n", ret);

    ret = upm6922_charger_psy_register(upm);
    if (ret) {
        upm6922_err("failed to register chg psy. err: %d\n", ret);
    }

#if IS_ENABLED(CONFIG_MTK_CHARGER)
    upm->chg_dev = charger_device_register(upm->chg_dev_name,
                          &client->dev, upm,
                          &upm6922_chg_ops,
                          &upm6922_chg_props);
    if (IS_ERR_OR_NULL(upm->chg_dev)) {
        ret = PTR_ERR(upm->chg_dev);
        upm6922_err("register charge device failed, ret:%d\n", ret);
    }

    /* otg regulator */
    config.dev = upm->dev;
    config.driver_data = upm;
    upm->otg_rdev = devm_regulator_register(upm->dev,
                        &upm6922_otg_rdesc, &config);
    if (IS_ERR(upm->otg_rdev)) {
        ret = PTR_ERR(upm->otg_rdev);
        upm6922_err("register otg regulator failed (%d)\n", ret);
    }
#endif

    mod_delayed_work(system_wq, &upm->prob_dwork, msecs_to_jiffies(2*1000));
    //schedule_delayed_work(&upm->monitor_dwork, msecs_to_jiffies(10*1000));

    enable_irq_wake(upm->client->irq);
    device_init_wakeup(upm->dev, true);

    upm6922_err("upm6922 probe successfully, Part Num:%d, Revision:%d\n!",
           upm->part_no, upm->revision);

    return 0;

err_dev_init:
err_parse_dt:
err_nodev:
    mutex_destroy(&upm->i2c_rw_lock);
    mutex_destroy(&upm->adc_lock);
    devm_kfree(&client->dev, upm);

    return ret;
}

static int upm6922_charger_remove(struct i2c_client *client)
{
    struct upm6922 *upm = i2c_get_clientdata(client);

    sysfs_remove_group(&upm->dev->kobj, &upm6922_attr_group);
    mutex_destroy(&upm->i2c_rw_lock);
    mutex_destroy(&upm->adc_lock);

    return 0;
}

static void upm6922_charger_shutdown(struct i2c_client *client)
{
    //struct upm6922 *upm = i2c_get_clientdata(client);

    upm6922_info("shutdown\n");
}

static int upm6922_pm_suspend(struct device *dev)
{
    upm6922_info("suspend\n");

    return 0;
}

static int upm6922_pm_resume(struct device *dev)
{
    upm6922_info("resume\n");

    return 0;
}

static const struct dev_pm_ops upm6922_pm_ops = {
    .resume = upm6922_pm_resume,
    .suspend = upm6922_pm_suspend,
};

static struct of_device_id upm6922_charger_match_table[] = {
    {.compatible = "up,upm6922_charger",},
    {},
};
MODULE_DEVICE_TABLE(of, upm6922_charger_match_table);

static struct i2c_driver upm6922_charger_driver = {
    .driver = {
        .name = "upm6922-charger",
        .owner = THIS_MODULE,
        .of_match_table = upm6922_charger_match_table,
        .pm = &upm6922_pm_ops,
    },

    .probe = upm6922_charger_probe,
    .remove = upm6922_charger_remove,
    .shutdown = upm6922_charger_shutdown,
};

module_i2c_driver(upm6922_charger_driver);

MODULE_DESCRIPTION("Unisemipower UPM6922 Charger Driver");
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("lai.du@unisemipower.com");

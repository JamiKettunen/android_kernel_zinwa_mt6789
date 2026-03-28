#include <linux/init.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <asm/uaccess.h>
#include <linux/slab.h>
#include <linux/power_supply.h>
#define CONFIG_MTK_GAUGE_VERSION 30

#include "hodafone_battery.h"
#if CONFIG_MTK_GAUGE_VERSION == 30
#include "mtk_charger.h"
#include "mtk_battery.h"
#endif
#include <linux/proc_fs.h>

#ifndef R_SENSE
#define R_SENSE 0
#endif

#define BATTERY_AVERAGE_SIZE 10


static int hodafonebattery_major = 0;
static int hodafonebattery_minor = 0;
static int bat_ac_charge = 0;
static int bat_cap_50h_pos = 0;
static int bat_cap_25h_pos = 0;
static int bat_cap_00h_pos = 0;
static int bat_cap_10h_neg = 0;
static int battery_voltage = 0;
static int battery_voltage_min = 0;
static int high_battery = 0;
static int temp_detect = 0;
static int soc_byhwfg = 0;
static int car_tune = 0;
static int rfg_value = 0;
static int rsense_value = 0;

static struct class* hodafonebattery_class = NULL;
static struct sec_dev* hodafonebattery_dev = NULL;

static int hodafonebattery_open(struct inode* inode, struct file* filp);
static int hodafonebattery_release(struct inode* inode, struct file* filp);

struct mtk_gauge *g_gauge = NULL;
struct mtk_charger *g_charger = NULL;

static struct file_operations hodafonebattery_fops = {
        .owner = THIS_MODULE,
        .open = hodafonebattery_open,
        .release = hodafonebattery_release,
};
#if 0
static unsigned int hodafone_battery_average_method(unsigned int *bufferdata,
					    unsigned int data, signed int *sum,unsigned char index)
{
	unsigned int avgdata;
	int i;
	static int batteryBufferFirst = 0;
	if (batteryBufferFirst == 0) {
		for (i = 0; i < BATTERY_AVERAGE_SIZE; i++) {
			bufferdata[i] = data;
		}
		*sum = data * BATTERY_AVERAGE_SIZE;
		batteryBufferFirst = 1;
	}
	
	*sum -= bufferdata[index];
	*sum += data;
	bufferdata[index] = data;
	avgdata = (*sum) / BATTERY_AVERAGE_SIZE;

	return avgdata;
}
#endif

int hodafonebattery_get_max_voltage(void)
{
	struct fuel_gauge_table_custom_data * ptable;
	ptable = &g_gauge->gm->fg_table_cust_data;
	return ptable->fg_profile[1].fg_profile[0].voltage;
}

int hodafonebattery_get_min_voltage(void)
{
	struct fuel_gauge_table_custom_data *ptable;
	ptable = &g_gauge->gm->fg_table_cust_data;
	return ptable->fg_profile[1].fg_profile[ptable->fg_profile[1].size-1].voltage;
}

/* ============================================================ */
/* gaugel hal interface */
/* ============================================================ */
int gauge_get_property(enum gauge_property gp,
	int *val)
{
	struct mtk_gauge *gauge;
	struct power_supply *psy;
	struct mtk_gauge_sysfs_field_info *attr;
	static struct mtk_battery *gm;
	int ret = 0;

	psy = power_supply_get_by_name("mtk-gauge");
	if (psy == NULL) {
		bm_err("Cannot get power supply of name\n");
		return -ENODEV;
	}

	gauge = (struct mtk_gauge *)power_supply_get_drvdata(psy);
	gm = gauge->gm;
	if (gm != NULL && gm->disableGM30) {
		bm_debug("%s disable GM30", __func__);
		return -EOPNOTSUPP;
	}

	attr = gauge->attr;
	if (attr == NULL) {
		bm_err("%s attr =NULL\n", __func__);
		return -ENODEV;
	}
	if (attr[gp].prop == gp) {
		mutex_lock(&gauge->ops_lock);
		ret = attr[gp].get(gauge, &attr[gp], val);

		mutex_unlock(&gauge->ops_lock);
	} else {
		bm_err("%s gp:%d idx error\n", __func__, gp);
		return -ENOTSUPP;
	}

	return ret;
}

int gauge_get_int_property(enum gauge_property gp)
{
	int val;

	gauge_get_property(gp, &val);
	return val;
}


int bat_get_debug_level(void)
{
	struct mtk_gauge *gauge;
	struct power_supply *psy;
	static struct mtk_battery *gm;

	if (gm == NULL) {
		psy = power_supply_get_by_name("mtk-gauge");
		if (psy == NULL)
			return BMLOG_DEBUG_LEVEL;
		gauge = (struct mtk_gauge *)power_supply_get_drvdata(psy);
		if (gauge == NULL || gauge->gm == NULL)
			return BMLOG_DEBUG_LEVEL;
		gm = gauge->gm;
	}
	return gm->log_level;
}

static ssize_t battery_ac_charge_show(struct device* dev, struct device_attribute* attr,  char* buf);
static ssize_t battery_ac_charge_store(struct device* dev, struct device_attribute* attr, const char* buf, size_t count);
static ssize_t battery_50pos_capacity_show(struct device* dev, struct device_attribute* attr,  char* buf);
static ssize_t battery_50pos_capacity_store(struct device* dev, struct device_attribute* attr, const char* buf, size_t count);
static ssize_t battery_25pos_capacity_show(struct device* dev, struct device_attribute* attr,  char* buf);
static ssize_t battery_25pos_capacity_store(struct device* dev, struct device_attribute* attr, const char* buf, size_t count);
static ssize_t battery_00pos_capacity_show(struct device* dev, struct device_attribute* attr,  char* buf);
static ssize_t battery_00pos_capacity_store(struct device* dev, struct device_attribute* attr, const char* buf, size_t count);
static ssize_t battery_10neg_capacity_show(struct device* dev, struct device_attribute* attr,  char* buf);
static ssize_t battery_10neg_capacity_store(struct device* dev, struct device_attribute* attr, const char* buf, size_t count);
static ssize_t battery_voltage_show(struct device* dev, struct device_attribute* attr,  char* buf);
static ssize_t battery_voltage_min_show(struct device* dev, struct device_attribute* attr,  char* buf);
static ssize_t battery_voltage_store(struct device* dev, struct device_attribute* attr, const char* buf, size_t count);
static ssize_t battery_voltage_min_store(struct device* dev, struct device_attribute* attr, const char* buf, size_t count);
static ssize_t battery_highvol_show(struct device* dev, struct device_attribute* attr,  char* buf);
static ssize_t battery_highvol_store(struct device* dev, struct device_attribute* attr, const char* buf, size_t count);
static ssize_t battery_temperature_show(struct device* dev, struct device_attribute* attr,  char* buf);
static ssize_t battery_temperature_store(struct device* dev, struct device_attribute* attr, const char* buf, size_t count);
static ssize_t battery_soc_byhwfg_show(struct device* dev, struct device_attribute* attr,  char* buf);
static ssize_t battery_soc_byhwfg_store(struct device* dev, struct device_attribute* attr, const char* buf, size_t count);
static ssize_t battery_car_tune_show(struct device* dev, struct device_attribute* attr,  char* buf);
static ssize_t battery_car_tune_store(struct device* dev, struct device_attribute* attr, const char* buf, size_t count);
static ssize_t battery_rfg_value_show(struct device* dev, struct device_attribute* attr,  char* buf);
static ssize_t battery_rfg_value_store(struct device* dev, struct device_attribute* attr, const char* buf, size_t count);
static ssize_t battery_get_rsense_show(struct device* dev, struct device_attribute* attr,  char* buf);
static ssize_t battery_get_rsense_store(struct device* dev, struct device_attribute* attr, const char* buf, size_t count);
static ssize_t show_FG_Battery_Current(struct device *dev,struct device_attribute *attr, char *buf);
static ssize_t store_FG_Battery_Current(struct device* dev, struct device_attribute* attr, const char* buf, size_t count);
static ssize_t show_FG_Charging_Current(struct device *dev,struct device_attribute *attr, char *buf);
static ssize_t store_FG_Charging_Current(struct device* dev, struct device_attribute* attr, const char* buf, size_t count);

static DEVICE_ATTR(batterycharge, S_IRUGO | S_IWUSR, battery_ac_charge_show, battery_ac_charge_store);
static DEVICE_ATTR(capacity50pos, S_IRUGO | S_IWUSR, battery_50pos_capacity_show, battery_50pos_capacity_store);
static DEVICE_ATTR(capacity25pos, S_IRUGO | S_IWUSR, battery_25pos_capacity_show, battery_25pos_capacity_store);
static DEVICE_ATTR(capacity00pos, S_IRUGO | S_IWUSR, battery_00pos_capacity_show, battery_00pos_capacity_store);
static DEVICE_ATTR(capacity10neg, S_IRUGO | S_IWUSR, battery_10neg_capacity_show, battery_10neg_capacity_store);
static DEVICE_ATTR(batteryvoltage, S_IRUGO | S_IWUSR, battery_voltage_show, battery_voltage_store);
static DEVICE_ATTR(batteryvoltagemin, S_IRUGO | S_IWUSR, battery_voltage_min_show, battery_voltage_min_store);
static DEVICE_ATTR(batteryhighvol, S_IRUGO | S_IWUSR, battery_highvol_show, battery_highvol_store);
static DEVICE_ATTR(batterytemperature, S_IRUGO | S_IWUSR, battery_temperature_show, battery_temperature_store);
static DEVICE_ATTR(socbyhwfg, S_IRUGO | S_IWUSR, battery_soc_byhwfg_show, battery_soc_byhwfg_store);
static DEVICE_ATTR(cartune, S_IRUGO | S_IWUSR, battery_car_tune_show, battery_car_tune_store);
static DEVICE_ATTR(rfgvalue, S_IRUGO | S_IWUSR, battery_rfg_value_show, battery_rfg_value_store);
static DEVICE_ATTR(rsensevalue, S_IRUGO | S_IWUSR, battery_get_rsense_show, battery_get_rsense_store);

static DEVICE_ATTR(FG_Charging_Current, S_IRUGO | S_IWUSR, show_FG_Charging_Current, store_FG_Charging_Current);
static DEVICE_ATTR(FG_Battery_Current, S_IRUGO | S_IWUSR, show_FG_Battery_Current, store_FG_Battery_Current);

static int hodafonebattery_open(struct inode* inode, struct file* filp) {
	struct sec_dev* dev;
	
	dev = container_of(inode->i_cdev, struct sec_dev, dev);
	filp->private_data = dev;

	return 0;
}

static int hodafonebattery_release(struct inode* inode, struct file* filp) {
	return 0;
}

static ssize_t battery_ac_charge_show(struct device* dev, struct device_attribute* attr, char* buf) {
	struct sec_dev* hdev = (struct sec_dev*)dev_get_drvdata(dev);
	if(down_interruptible(&(hdev->sem))) {
     	return -ERESTARTSYS;
    }
#if CONFIG_MTK_GAUGE_VERSION == 30
	bat_ac_charge =  g_charger->data.ac_charger_current/ 1000;
#else
	bat_ac_charge = batt_meter_cust_data.ac_charger_current / 100;
#endif
	printk("### ttc bat_ac_charge = %d\n",bat_ac_charge);
    up(&(hdev->sem));	
		
	return sprintf(buf, "%u\n", bat_ac_charge);
}

static ssize_t battery_ac_charge_store(struct device* dev, struct device_attribute* attr, const char* buf, size_t count) {
	return 0;
}

static ssize_t battery_50pos_capacity_show(struct device* dev, struct device_attribute* attr, char* buf) {
	struct sec_dev* hdev = (struct sec_dev*)dev_get_drvdata(dev);
	struct fuel_gauge_table_custom_data * ptable;
	ptable = &g_gauge->gm->fg_table_cust_data;
	if(down_interruptible(&(hdev->sem))) {
     	return -ERESTARTSYS;
    }

#if CONFIG_MTK_GAUGE_VERSION == 30
	bat_cap_50h_pos = ptable->fg_profile[0].q_max_h_current ;
#else
	bat_cap_50h_pos = batt_meter_cust_data.q_max_pos_50_h_current;
#endif

	printk("### ttc bat_cap_50h_pos = %d\n",bat_cap_50h_pos);
    up(&(hdev->sem));	
		
	return sprintf(buf, "%u\n", bat_cap_50h_pos);
}

static ssize_t battery_50pos_capacity_store(struct device* dev, struct device_attribute* attr, const char* buf, size_t count) {
	return 0;
}

static ssize_t battery_25pos_capacity_show(struct device* dev, struct device_attribute* attr, char* buf) {
	struct sec_dev* hdev = (struct sec_dev*)dev_get_drvdata(dev);
    struct fuel_gauge_table_custom_data *ptable;
    ptable = &g_gauge->gm->fg_table_cust_data;
	if(down_interruptible(&(hdev->sem))) {
  		return -ERESTARTSYS;
    }

#if CONFIG_MTK_GAUGE_VERSION == 30
	bat_cap_25h_pos = ptable->fg_profile[1].q_max_h_current ;
#else
	bat_cap_25h_pos = batt_meter_cust_data.q_max_pos_25_h_current;
#endif
	printk("### ttc bat_cap_25h_pos = %d\n",bat_cap_25h_pos);
    up(&(hdev->sem));	
		
	return sprintf(buf, "%u\n", bat_cap_25h_pos);
}

static ssize_t battery_25pos_capacity_store(struct device* dev, struct device_attribute* attr, const char* buf, size_t count) {
	return 0;
}

static ssize_t battery_00pos_capacity_show(struct device* dev, struct device_attribute* attr, char* buf) {
	struct sec_dev* hdev = (struct sec_dev*)dev_get_drvdata(dev);
    struct fuel_gauge_table_custom_data *ptable;
    ptable = &g_gauge->gm->fg_table_cust_data;
	if(down_interruptible(&(hdev->sem))) {
    	return -ERESTARTSYS;
    }

#if CONFIG_MTK_GAUGE_VERSION == 30
	bat_cap_00h_pos = ptable->fg_profile[3].q_max_h_current ;
#else
	bat_cap_00h_pos = batt_meter_cust_data.q_max_pos_0_h_current;
#endif
	printk("### ttc bat_cap_00h_pos = %d\n",bat_cap_00h_pos);
    up(&(hdev->sem));	
		
	return sprintf(buf, "%u\n", bat_cap_00h_pos);
}

static ssize_t battery_00pos_capacity_store(struct device* dev, struct device_attribute* attr, const char* buf, size_t count) {
	return 0;
}

static ssize_t battery_10neg_capacity_show(struct device* dev, struct device_attribute* attr, char* buf) {
	struct sec_dev* hdev = (struct sec_dev*)dev_get_drvdata(dev);
    struct fuel_gauge_table_custom_data *ptable;
    ptable = &g_gauge->gm->fg_table_cust_data;	
	if(down_interruptible(&(hdev->sem))) {
  		return -ERESTARTSYS;
    }

#if CONFIG_MTK_GAUGE_VERSION == 30
	bat_cap_10h_neg = ptable->fg_profile[4].q_max_h_current ;
#else
	bat_cap_10h_neg = batt_meter_cust_data.q_max_neg_10_h_current;
#endif
	printk("### ttc bat_cap_50h_pos = %d\n",bat_cap_10h_neg);
    up(&(hdev->sem));	
		
	return sprintf(buf, "%u\n", bat_cap_10h_neg);
}

static ssize_t battery_10neg_capacity_store(struct device* dev, struct device_attribute* attr, const char* buf, size_t count) {
	return 0;
}

static ssize_t battery_voltage_show(struct device* dev, struct device_attribute* attr, char* buf) {
	struct sec_dev* hdev = (struct sec_dev*)dev_get_drvdata(dev);
	if(down_interruptible(&(hdev->sem))) {
    	return -ERESTARTSYS;
    }
	battery_voltage = hodafonebattery_get_max_voltage()/10;
	printk("### ttc battery_value = %d\n",battery_voltage);
    up(&(hdev->sem));	
		
	return sprintf(buf, "%u\n", battery_voltage);
}

static ssize_t battery_voltage_store(struct device* dev, struct device_attribute* attr, const char* buf, size_t count) {
	return 0;
}

static ssize_t battery_voltage_min_show(struct device* dev, struct device_attribute* attr, char* buf) {
	struct sec_dev* hdev = (struct sec_dev*)dev_get_drvdata(dev);
	if(down_interruptible(&(hdev->sem))) {
    	return -ERESTARTSYS;
    }
	battery_voltage_min = hodafonebattery_get_min_voltage()/10;
	printk("### ttc battery_value = %d\n",battery_voltage_min);
    up(&(hdev->sem));	
		
	return sprintf(buf, "%u\n", battery_voltage_min);
}

static ssize_t battery_voltage_min_store(struct device* dev, struct device_attribute* attr, const char* buf, size_t count) {
	return 0;
}

static ssize_t battery_highvol_show(struct device* dev, struct device_attribute* attr, char* buf) {
	struct sec_dev* hdev = (struct sec_dev*)dev_get_drvdata(dev);
	if(down_interruptible(&(hdev->sem))) {
      	return -ERESTARTSYS;
    }
#if CONFIG_MTK_GAUGE_VERSION == 30 
	high_battery = g_charger->data.battery_cv > 4200000 ? 1:0;
#else
    high_battery = batt_cust_data.high_battery_voltage_support;
#endif
	printk("### ttc high_battery = %d\n",high_battery);
    up(&(hdev->sem));	
		
	return sprintf(buf, "%u\n", high_battery);
}

static ssize_t battery_highvol_store(struct device* dev, struct device_attribute* attr, const char* buf, size_t count) {
	return 0;
}

static ssize_t battery_temperature_show(struct device* dev, struct device_attribute* attr, char* buf) {
	struct sec_dev* hdev = (struct sec_dev*)dev_get_drvdata(dev);
	if(down_interruptible(&(hdev->sem))) {
    	return -ERESTARTSYS;
    }
	#ifdef FIXED_TBAT_25
    	temp_detect = 0;
    #else
   		temp_detect = 1;
    #endif

	printk("### ttc temp_detect = %d\n",temp_detect);
    up(&(hdev->sem));	
		
	return sprintf(buf, "%u\n", temp_detect);
}

static ssize_t battery_temperature_store(struct device* dev, struct device_attribute* attr, const char* buf, size_t count) {
	return 0;
}

static ssize_t battery_soc_byhwfg_show(struct device* dev, struct device_attribute* attr, char* buf) {
	struct sec_dev* hdev = (struct sec_dev*)dev_get_drvdata(dev);
	if(down_interruptible(&(hdev->sem))) {
    	return -ERESTARTSYS;
    }
#if CONFIG_MTK_GAUGE_VERSION == 30
	soc_byhwfg = 1;
#else
	if (batt_meter_cust_data.soc_flow == HW_FG) {
		soc_byhwfg = 1;
	} else {
		soc_byhwfg = 0;
	}
#endif

	printk("### ttc soc_byhwfg = %d\n",soc_byhwfg);
    up(&(hdev->sem));	
		
	return sprintf(buf, "%u\n", soc_byhwfg);
}

static ssize_t battery_soc_byhwfg_store(struct device* dev, struct device_attribute* attr, const char* buf, size_t count) {
	return 0;
}

static ssize_t battery_car_tune_show(struct device* dev, struct device_attribute* attr, char* buf) {
	struct sec_dev* hdev = (struct sec_dev*)dev_get_drvdata(dev);
	struct fuel_gauge_custom_data fg_cust_data;
    fg_cust_data = g_gauge->gm->fg_cust_data;
	if(down_interruptible(&(hdev->sem))) {
    	return -ERESTARTSYS;
    }
#if CONFIG_MTK_GAUGE_VERSION == 30
    car_tune = fg_cust_data.car_tune_value;
#else
    car_tune = batt_meter_cust_data.car_tune_value;
#endif
	printk("### ttc car_tune = %d\n",car_tune);
    up(&(hdev->sem));	
		
	return sprintf(buf, "%u\n", car_tune);
}

static ssize_t battery_car_tune_store(struct device* dev, struct device_attribute* attr, const char* buf, size_t count) {
	int tune_value;
	int ret;
	struct fuel_gauge_custom_data fg_cust_data;
    fg_cust_data = g_gauge->gm->fg_cust_data;
	ret = kstrtoint(buf, 0, &tune_value);
	if (ret)
		return ret;
#if CONFIG_MTK_GAUGE_VERSION == 30
	fg_cust_data.car_tune_value = tune_value;
#else
	batt_meter_cust_data.car_tune_value = tune_value;
#endif
	return count;
}

static ssize_t battery_rfg_value_show(struct device* dev, struct device_attribute* attr, char* buf) {
	struct sec_dev* hdev = (struct sec_dev*)dev_get_drvdata(dev);
	struct fuel_gauge_custom_data fg_cust_data;
    fg_cust_data = g_gauge->gm->fg_cust_data;
	if(down_interruptible(&(hdev->sem))) {
    	return -ERESTARTSYS;
    }
#if CONFIG_MTK_GAUGE_VERSION == 30
	rfg_value = fg_cust_data.r_fg_value;
#else
    rfg_value = batt_meter_cust_data.r_fg_value;
#endif
	printk("### ttc car_tune = %d\n",rfg_value);
    up(&(hdev->sem));	
		
	return sprintf(buf, "%u\n", rfg_value);
}

static ssize_t battery_rfg_value_store(struct device* dev, struct device_attribute* attr, const char* buf, size_t count) {
	return 0;
}

static ssize_t battery_get_rsense_store(struct device* dev, struct device_attribute* attr, const char* buf, size_t count) {
	return 0;
}

static ssize_t battery_get_rsense_show(struct device* dev, struct device_attribute* attr, char* buf) {
	struct sec_dev* hdev = (struct sec_dev*)dev_get_drvdata(dev);
	if(down_interruptible(&(hdev->sem))) {
    	return -ERESTARTSYS;
    }
#if CONFIG_MTK_GAUGE_VERSION == 30
	rsense_value = R_SENSE;
#else
    rsense_value = batt_meter_cust_data.cust_r_sense;
#endif
    up(&(hdev->sem));	
	return sprintf(buf, "%u\n", rsense_value);
}

static ssize_t show_FG_Battery_Current(struct device *dev,struct device_attribute *attr, char *buf)
{
    int bat_current=0;
    struct power_supply *psy = NULL;
	struct mtk_charger *info = NULL;
	
    psy = power_supply_get_by_name("mtk-master-charger");
	if (psy == NULL) {
		printk("get master charger psy fail!\n");
		goto out;
	}
		
	info = (struct mtk_charger *)power_supply_get_drvdata(psy);
	if (info == NULL) {
		printk("get mtk_charger info fail!\n");
	}
out:
	if (info != NULL) {
		bat_current = get_battery_current(info);
	} else {
		bat_current = 10;
	}

    printk("[FG] gFG_current_inout_battery : %d\n", bat_current);
    return sprintf(buf, "%d\n", bat_current);
}
static ssize_t store_FG_Battery_Current(struct device* dev, struct device_attribute* attr, const char* buf, size_t count)
{
	return 0;
}
static ssize_t show_FG_Charging_Current(struct device *dev,struct device_attribute *attr, char *buf)
{
    int ibus_current=0;
    int ibus = 0;
	charger_dev_get_ibus(g_charger->chg1_dev,&ibus);
	ibus_current += ibus/1000;

    printk("[FG] gFG_current_inout_battery : %d\n", ibus_current);
    return sprintf(buf, "%d\n", ibus_current);
}

static ssize_t store_FG_Charging_Current(struct device* dev, struct device_attribute* attr, const char* buf, size_t count)
{
	return 0;
}

static int  __hodafonebattery_setup_dev(struct sec_dev* dev) {
	int err;
	dev_t devno = MKDEV(hodafonebattery_major, hodafonebattery_minor);

	memset(dev, 0, sizeof(struct sec_dev));

	cdev_init(&(dev->dev), &hodafonebattery_fops);
	dev->dev.owner = THIS_MODULE;
	dev->dev.ops = &hodafonebattery_fops;

	err = cdev_add(&(dev->dev),devno, 1);
	if(err) {
		return err;
	}	
	sema_init(&(dev->sem),1);


	return 0;
}

static int __init hodafonebattery_init(void) { 
	int err = -1;
	dev_t dev = 0;
	struct device* temp = NULL;
	struct power_supply *psy;

	printk(KERN_ALERT"Initializing hodafonebattery device.\n");
	
	psy = power_supply_get_by_name("mtk-gauge");
	if (psy == NULL) {
		bm_err("Cannot get power supply of name\n");
	} else {
		g_gauge = (struct mtk_gauge *)power_supply_get_drvdata(psy);

		if (g_gauge == NULL) {
			bm_debug("%s Cannot get mtk_gauge", __func__);
		}
	}
	
	psy = power_supply_get_by_name("mtk-master-charger");
	if (psy == NULL) {
		bm_err("get master charger psy fail!\n");
	} else {
		g_charger = (struct mtk_charger *)power_supply_get_drvdata(psy);
		if (g_charger == NULL) {
			bm_err("get mtk_charger info fail!\n");
		}
	}

	err = alloc_chrdev_region(&dev, 0, 1, HODAFONEBATTERY_DEVICE_NODE_NAME);
	if(err < 0) {
		printk(KERN_ALERT"Failed to alloc char dev region.\n");
		goto fail;
	}

	hodafonebattery_major = MAJOR(dev);
	hodafonebattery_minor = MINOR(dev);

	hodafonebattery_dev = kmalloc(sizeof(struct sec_dev), GFP_KERNEL);
	if(!hodafonebattery_dev) {
		err = -ENOMEM;
		printk(KERN_ALERT"Failed to alloc hodafonebattery device.\n");
		goto unregister;
	}

	err = __hodafonebattery_setup_dev(hodafonebattery_dev);
	if(err) {
		printk(KERN_ALERT"Failed to setup hodafonebattery device: %d.\n", err);
		goto cleanup;
	}

	hodafonebattery_class = class_create(THIS_MODULE, HODAFONEBATTERY_DEVICE_CLASS_NAME);
	if(IS_ERR(hodafonebattery_class)) {
		err = PTR_ERR(hodafonebattery_class);
		printk(KERN_ALERT"Failed to create hodafonebattery device class.\n");
		goto destroy_cdev;
	}

	temp = device_create(hodafonebattery_class, NULL, dev, "%s", HODAFONEBATTERY_DEVICE_FILE_NAME);
	if(IS_ERR(temp)) {
		err = PTR_ERR(temp);
		printk(KERN_ALERT"Failed to create hodafonebattery device.\n");
		goto destroy_class;
	}

	err = device_create_file(temp, &dev_attr_batterycharge);
	if(err < 0) {
		printk(KERN_ALERT"Failed to create attribute batterycharge of batterycharge device.\n");
                goto destroy_device;
	}

	err = device_create_file(temp, &dev_attr_capacity50pos);
	if(err < 0) {
		printk(KERN_ALERT"Failed to create attribute capacity50pos of capacity50pos device.\n");
                goto destroy_device;
	}

	err = device_create_file(temp, &dev_attr_capacity25pos);
	if(err < 0) {
		printk(KERN_ALERT"Failed to create attribute capacity25pos of capacity25pos device.\n");
                goto destroy_device;
	}

	err = device_create_file(temp, &dev_attr_capacity00pos);
	if(err < 0) {
		printk(KERN_ALERT"Failed to create attribute capacity00pos of capacity00pos device.\n");
                goto destroy_device;
	}

	err = device_create_file(temp, &dev_attr_capacity10neg);
	if(err < 0) {
		printk(KERN_ALERT"Failed to create attribute capacity10neg of capacity10neg device.\n");
                goto destroy_device;
	}

	err = device_create_file(temp, &dev_attr_batteryvoltage);
	if(err < 0) {
        printk(KERN_ALERT"Failed to create attribute batteryvoltage of batteryvoltage device.\n");
                goto destroy_device;
    }
	
	err = device_create_file(temp, &dev_attr_batteryvoltagemin);
	if(err < 0) {
        printk(KERN_ALERT"Failed to create attribute min batteryvoltage of batteryvoltage device.\n");
                goto destroy_device;
    }

	err = device_create_file(temp, &dev_attr_batteryhighvol);
	if(err < 0) {
        printk(KERN_ALERT"Failed to create attribute batteryhighvol of batteryhighvol device.\n");
                goto destroy_device;
    }

	err = device_create_file(temp, &dev_attr_batterytemperature);
	if(err < 0) {
        printk(KERN_ALERT"Failed to create attribute batteryhighvol of batterytemperature device.\n");
                goto destroy_device;
    }

	err = device_create_file(temp, &dev_attr_socbyhwfg);
	if(err < 0) {
        printk(KERN_ALERT"Failed to create attribute batteryhighvol of socbyhwfg device.\n");
                goto destroy_device;
    }

	err = device_create_file(temp, &dev_attr_cartune);
	if(err < 0) {
        printk(KERN_ALERT"Failed to create attribute batteryhighvol of cartune device.\n");
                goto destroy_device;
    }

	err = device_create_file(temp, &dev_attr_rfgvalue);
	if(err < 0) {
        printk(KERN_ALERT"Failed to create attribute batteryhighvol of rfgvalue device.\n");
                goto destroy_device;
    }
    err = device_create_file(temp, &dev_attr_rsensevalue);
	if(err < 0) {
        printk(KERN_ALERT"Failed to create attribute rsensevalue of device.\n");
                goto destroy_device;
    }
    
    err = device_create_file(temp, &dev_attr_FG_Charging_Current);
	if(err < 0) {
        printk(KERN_ALERT"Failed to create attribute FG_Charging_Current of device.\n");
                goto destroy_device;
    }
    
    err = device_create_file(temp, &dev_attr_FG_Battery_Current);
	if(err < 0) {
        printk(KERN_ALERT"Failed to create attribute FG_Battery_Current of device.\n");
                goto destroy_device;
    }

	dev_set_drvdata(temp, hodafonebattery_dev);

	printk(KERN_ALERT"Succedded to initialize hodafonebattery device.\n");

	return 0;

destroy_device:
	device_destroy(hodafonebattery_class, dev);
destroy_class:
	class_destroy(hodafonebattery_class);
destroy_cdev:
	cdev_del(&(hodafonebattery_dev->dev));	
cleanup:
	kfree(hodafonebattery_dev);
unregister:
	unregister_chrdev_region(MKDEV(hodafonebattery_major, hodafonebattery_minor), 1);	
fail:
	return err;
}

static void __exit hodafonebattery_exit(void) {
	dev_t devno = MKDEV(hodafonebattery_major, hodafonebattery_minor);

	printk(KERN_ALERT"Destroy hodafonebattery device.\n");

	if(hodafonebattery_class) {
		device_destroy(hodafonebattery_class, MKDEV(hodafonebattery_major, hodafonebattery_minor));
		class_destroy(hodafonebattery_class);
	}

	if(hodafonebattery_dev) {
		cdev_del(&(hodafonebattery_dev->dev));
		kfree(hodafonebattery_dev);
	}

	unregister_chrdev_region(devno, 1);
}

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Efuse Driver");

module_init(hodafonebattery_init);
module_exit(hodafonebattery_exit);

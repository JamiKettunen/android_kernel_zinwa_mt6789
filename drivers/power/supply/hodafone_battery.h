#ifndef _HODAFONEBATTERY_H_
#define _HODAFONEBATTERY_H_

#include <linux/cdev.h>
#include <linux/semaphore.h>

#define HODAFONEBATTERY_DEVICE_NODE_NAME  "hodafonebattery"
#define HODAFONEBATTERY_DEVICE_FILE_NAME  "hodafonebattery"
#define HODAFONEBATTERY_DEVICE_CLASS_NAME "hodafonebattery"

struct sec_dev {
	struct semaphore sem;
	struct cdev dev;
};

extern bool battery_get_bat_current_sign(void);
extern int pmic_get_charging_current(void);
#endif


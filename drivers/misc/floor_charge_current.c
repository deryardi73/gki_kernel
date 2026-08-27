// SPDX-License-Identifier: GPL-2.0
/*
 * floor_charge: floor MTK charging current at 3A.
 *
 * charger_dev_set_charging_current() lives in charger_class.ko, a vendor
 * module loaded from userspace late in boot. This code is built in, so
 * its init runs before that module exists -- arm the kprobe from a
 * module-load notifier instead of at init.
 *
 * charger_dev_set_charging_current() is downstream of JEITA, so a floor
 * applied blindly there would also override a legitimate JEITA-driven
 * drop from real battery heat -- not just mi_thermald's throttle steps.
 * Guard the floor with our own battery-temp read, same range JEITA logs
 * ("Battery Normal Temperature between 15 and 45"), so it only overrides
 * mi_thermald and stays out of JEITA's way when the battery is actually
 * hot.
 *
 * IMPORTANT: kprobe pre_handlers run in atomic context (preemption
 * disabled). power_supply_get_property() on a fuel-gauge backed
 * battery psy can go down to an I2C read and sleep -- calling it
 * directly from the pre_handler caused an instant reboot on first
 * plug-in ("scheduling while atomic" -> panic). Poll the temperature
 * from a delayed_work instead (process context, allowed to sleep) and
 * have the pre_handler only read the cached value.
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/power_supply.h>
#include <linux/workqueue.h>

#define FC_NAME "floor_charge"
#define FC_FLOOR_UA 3000000U
#define FC_TARGET_MODULE "charger_class"
#define FC_TEMP_MIN_DECIC 150 /* 15.0C */
#define FC_TEMP_MAX_DECIC 450 /* 45.0C */
#define FC_TEMP_POLL_MS 2000
#define FC_TEMP_UNKNOWN INT_MIN

static bool fc_kp_registered;
static DEFINE_MUTEX(fc_lock);
static atomic_t fc_cached_temp = ATOMIC_INIT(FC_TEMP_UNKNOWN);
static struct delayed_work fc_temp_work;

/* Process context only -- never called from the kprobe handler. */
static void fc_temp_poll(struct work_struct *w)
{
	struct power_supply *psy;
	union power_supply_propval val;
	int temp = FC_TEMP_UNKNOWN;

	psy = power_supply_get_by_name("battery");
	if (psy) {
		if (!power_supply_get_property(psy, POWER_SUPPLY_PROP_TEMP, &val))
			temp = val.intval;
		power_supply_put(psy);
	}

	atomic_set(&fc_cached_temp, temp);
	schedule_delayed_work(&fc_temp_work, msecs_to_jiffies(FC_TEMP_POLL_MS));
}

/*
 * Safe to call from atomic context: just reads the cache. Unknown
 * (no poll result yet, or psy/read failed) -> skip the floor.
 */
static bool fc_battery_temp_ok(void)
{
	int temp = atomic_read(&fc_cached_temp);

	if (temp == FC_TEMP_UNKNOWN)
		return false;
	return temp >= FC_TEMP_MIN_DECIC && temp <= FC_TEMP_MAX_DECIC;
}

/* chg_dev in x0/regs[0], uA in w1/regs[1] (AAPCS64). */
static int fc_pre_set_cc(struct kprobe *p, struct pt_regs *regs)
{
	u32 ua = (u32)regs->regs[1];

	if (ua != 0 && ua < FC_FLOOR_UA && fc_battery_temp_ok()) {
		regs->regs[1] = FC_FLOOR_UA;
		pr_info(FC_NAME ": floored charging current %u -> %u uA\n",
			ua, FC_FLOOR_UA);
	}
	return 0;
}

static struct kprobe fc_kp_set_cc = {
	.symbol_name = "charger_dev_set_charging_current",
	.pre_handler = fc_pre_set_cc,
};

static bool fc_try_arm(void)
{
	int ret;

	mutex_lock(&fc_lock);
	if (fc_kp_registered) {
		mutex_unlock(&fc_lock);
		return true;
	}

	ret = register_kprobe(&fc_kp_set_cc);
	if (ret < 0) {
		mutex_unlock(&fc_lock);
		return false;
	}

	fc_kp_registered = true;
	mutex_unlock(&fc_lock);
	schedule_delayed_work(&fc_temp_work, 0);
	pr_info(FC_NAME ": hooked charger_dev_set_charging_current, floor=%u uA\n",
		FC_FLOOR_UA);
	return true;
}

static int fc_module_notify(struct notifier_block *nb, unsigned long event,
			     void *data)
{
	struct module *mod = data;

	if (event != MODULE_STATE_LIVE)
		return NOTIFY_DONE;

	if (strcmp(mod->name, FC_TARGET_MODULE))
		return NOTIFY_DONE;

	if (!fc_try_arm())
		pr_warn(FC_NAME ": %s loaded but symbol lookup still failed\n",
			FC_TARGET_MODULE);

	return NOTIFY_DONE;
}

static struct notifier_block fc_module_nb = {
	.notifier_call = fc_module_notify,
};

static int __init fc_init(void)
{
	INIT_DELAYED_WORK(&fc_temp_work, fc_temp_poll);
	if (!fc_try_arm())
		register_module_notifier(&fc_module_nb);
	return 0;
}

static void __exit fc_exit(void)
{
	unregister_module_notifier(&fc_module_nb);
	cancel_delayed_work_sync(&fc_temp_work);
	mutex_lock(&fc_lock);
	if (fc_kp_registered) {
		unregister_kprobe(&fc_kp_set_cc);
		fc_kp_registered = false;
	}
	mutex_unlock(&fc_lock);
}

module_init(fc_init);
module_exit(fc_exit);

MODULE_DESCRIPTION("Floor MTK charging current at 3A via kprobe");
MODULE_AUTHOR("deryardi73");
MODULE_LICENSE("GPL");

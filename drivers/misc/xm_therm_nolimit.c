// SPDX-License-Identifier: GPL-2.0
/*
 * xm_therm_nolimit: ignore the XM_THERM CPU frequency limit.
 *
 * mt_ppm_sysboost_set_freq_limit() lives in mtk_ppm_v3.ko, a vendor module
 * loaded from userspace late in boot. mi_thermald reaches it through
 * thermal_message/cpu_limits (thermal_monitor.ko) with user ==
 * BOOST_BY_XM_THERMAL, which caps the cluster max frequency too early.
 * This code is built in, so its init runs before that module exists --
 * arm the kprobe from a module-load notifier instead of at init.
 *
 * The pre_handler turns the call into a plain "ret" when the caller is
 * XM_THERM. Entry of the function is not yet past the prologue, so x30 is
 * still the caller's unsigned return address and no stack was touched.
 * All other users (WIFI, USB, PERFSERV, ...) are left alone.
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/string.h>

#define XT_NAME "xm_therm_nolimit"
#define XT_TARGET_MODULE "mtk_ppm_v3"
/* enum ppm_sysboost_user: BOOST_BY_XM_THERMAL */
#define XT_USER_XM_THERMAL 8

static bool xt_kp_registered;
static DEFINE_MUTEX(xt_lock);

/* enum ppm_sysboost_user in w0/regs[0] (AAPCS64) */
static int xt_pre_set_freq_limit(struct kprobe *p, struct pt_regs *regs)
{
	if ((u32)regs->regs[0] != XT_USER_XM_THERMAL)
		return 0;

	/* emulate "ret": skip the function body, no single-step needed */
	instruction_pointer_set(regs, regs->regs[30]);
	return 1;
}

static struct kprobe xt_kp = {
	.symbol_name = "mt_ppm_sysboost_set_freq_limit",
	.pre_handler = xt_pre_set_freq_limit,
};

static bool xt_try_arm(void)
{
	int ret;

	mutex_lock(&xt_lock);
	if (xt_kp_registered) {
		mutex_unlock(&xt_lock);
		return true;
	}

	ret = register_kprobe(&xt_kp);
	if (ret < 0) {
		mutex_unlock(&xt_lock);
		return false;
	}

	xt_kp_registered = true;
	mutex_unlock(&xt_lock);
	pr_debug(XT_NAME ": XM_THERM freq limit disabled\n");
	return true;
}

static void xt_disarm(void)
{
	mutex_lock(&xt_lock);
	if (xt_kp_registered) {
		unregister_kprobe(&xt_kp);
		xt_kp_registered = false;
	}
	mutex_unlock(&xt_lock);
}

static int xt_module_notify(struct notifier_block *nb, unsigned long event,
			    void *data)
{
	struct module *mod = data;

	if (strcmp(mod->name, XT_TARGET_MODULE))
		return NOTIFY_DONE;

	if (event == MODULE_STATE_LIVE) {
		if (!xt_try_arm())
			pr_warn(XT_NAME ": %s loaded but symbol lookup failed\n",
				XT_TARGET_MODULE);
	} else if (event == MODULE_STATE_GOING) {
		/* allow re-arming when the module is loaded again */
		xt_disarm();
	}

	return NOTIFY_DONE;
}

static struct notifier_block xt_module_nb = {
	.notifier_call = xt_module_notify,
};

static int __init xt_init(void)
{
	/* normally fails here (module not loaded yet); the notifier arms it */
	xt_try_arm();
	register_module_notifier(&xt_module_nb);
	return 0;
}

static void __exit xt_exit(void)
{
	unregister_module_notifier(&xt_module_nb);
	xt_disarm();
}

module_init(xt_init);
module_exit(xt_exit);

MODULE_DESCRIPTION("Ignore the XM_THERM CPU frequency limit via kprobe");
MODULE_AUTHOR("deryardi73");
MODULE_LICENSE("GPL");

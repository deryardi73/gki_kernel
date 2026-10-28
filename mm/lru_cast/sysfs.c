// SPDX-License-Identifier: GPL-2.0
/*
 * CAST -- /sys/kernel/mm/lru_cast/
 *
 *   version      the release
 *   enabled      1 / 0: switch CAST on or off at run time (not if booted off)
 *   decay        le per type, 1..14: half-life C * 2^(8 - le) pages taken in.
 *                Writing one sets both and stops decay_auto
 *   decay_auto   1 / 0: the controllers (tune/) set the decay and W's share
 *   tune         the controllers: per type the decay and W's share, and per
 *                simulated cache the misses and disagreements they decide on
 *   w_permille   W's share per type, in thousandths of its pages; 0 = the gate.
 *                Writing one sets both and stops decay_auto
 *   clock        per node: pages taken in, pages evicted, and the tick
 *   clean_min_ratio  the clean-file floor, percent of a node's present pages
 *   floor_held   node reclaim passes the clean-file floor held for
 *   balance      reclaim calls that took anon, took file, took nothing, or
 *                took both by size (the last pass before OOM)
 *   estimate     anon's share of memory, the swappiness the estimated refault
 *                costs amount to, and the costs: how long a task waited, on
 *                average, for an evicted page of each type to come back in
 *   walker       1 / 0: the walker reads the young bits of mapped pages
 *   prefetch     how far ahead the walker, the look-around and reclaim's
 *                candidates pull the frames they read next: toward L3, toward
 *                L1 (core/prefetch.h). Set at boot by the CPU; 0 0 = none
 *   walker_simd  1 / 0: it reads a page table's young bits with SIMD (x86-64),
 *                or one PTE at a time; reads back the kernel in use
 *   kcompressd   -100..100: kswapd's anon swap-outs by a thread per node, at
 *                most this many queued; 0 off, below 0 also with CAST off
 */
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include "control.h"
#include "core/core.h"
#include "policy/policy.h"
#include "tune/tune.h"
#include "walk/walk.h"
#include "version.h"

static ssize_t version_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%s\n", LRU_CAST_VERSION);
}

static ssize_t enabled_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", lru_cast_is_enabled());
}

static ssize_t enabled_store(struct kobject *kobj, struct kobj_attribute *attr,
			     const char *buf, size_t len)
{
	bool on;

	int err;

	if (kstrtobool(buf, &on))
		return -EINVAL;
	err = lru_cast_set_enabled(on);
	return err ? err : len;
}

static ssize_t decay_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "anon %u file %u\n", lru_cast_param_le(0), lru_cast_param_le(1));
}

/* a decay set by hand holds: the controller stops applying its own */
static ssize_t decay_store(struct kobject *kobj, struct kobj_attribute *attr,
			   const char *buf, size_t len)
{
	unsigned int v;

	if (kstrtouint(buf, 0, &v) || v < LRU_CAST_LE_MIN || v > LRU_CAST_LE_MAX)
		return -EINVAL;
	lru_cast_tune_set_auto(false);
	lru_cast_param_set_le(0, v);
	lru_cast_param_set_le(1, v);
	return len;
}

static ssize_t decay_auto_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%u\n", lru_cast_tune_auto());
}

static ssize_t decay_auto_store(struct kobject *kobj, struct kobj_attribute *attr,
				const char *buf, size_t len)
{
	bool on;

	if (kstrtobool(buf, &on))
		return -EINVAL;
	lru_cast_tune_set_auto(on);
	return len;
}

static ssize_t tune_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	int len = lru_cast_tune_show(buf);

	len = lru_cast_protect_show(buf, len);
	len = lru_cast_walk_show(buf, len);
	return lru_cast_look_around_show(buf, len);
}

static ssize_t w_permille_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "anon %u file %u\n", lru_cast_param_w(0), lru_cast_param_w(1));
}

/* a share set by hand holds: the controller stops applying its own */
static ssize_t w_permille_store(struct kobject *kobj, struct kobj_attribute *attr,
				const char *buf, size_t len)
{
	unsigned int v;

	if (kstrtouint(buf, 0, &v) || v > 1000)
		return -EINVAL;
	lru_cast_tune_set_auto(false);
	lru_cast_param_set_w(0, v);
	lru_cast_param_set_w(1, v);
	return len;
}

static ssize_t clock_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return lru_cast_clock_show(buf);
}

static ssize_t clean_min_ratio_show(struct kobject *kobj, struct kobj_attribute *attr,
				    char *buf)
{
	return sysfs_emit(buf, "%u\n", lru_cast_floor_ratio());
}

static ssize_t clean_min_ratio_store(struct kobject *kobj, struct kobj_attribute *attr,
				     const char *buf, size_t len)
{
	unsigned int v;

	if (kstrtouint(buf, 10, &v) || lru_cast_set_floor_ratio(v))
		return -EINVAL;
	return len;
}

static ssize_t estimate_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return lru_cast_tune_estimate_show(buf);
}

static ssize_t walker_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%u\n", lru_cast_walk_enabled());
}

static ssize_t walker_store(struct kobject *kobj, struct kobj_attribute *attr,
			    const char *buf, size_t len)
{
	bool on;

	if (kstrtobool(buf, &on))
		return -EINVAL;
	lru_cast_walk_enable(on);
	return len;
}

static ssize_t walker_simd_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%u %s\n", lru_cast_simd_enabled(), lru_cast_simd_name());
}

static ssize_t walker_simd_store(struct kobject *kobj, struct kobj_attribute *attr,
				 const char *buf, size_t len)
{
	bool on;

	if (kstrtobool(buf, &on))
		return -EINVAL;
	if (lru_cast_simd_enable(on))
		return -EINVAL;
	return len;
}

static ssize_t prefetch_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%u %u\n", READ_ONCE(lru_cast_pf_l3), READ_ONCE(lru_cast_pf_l1));
}

static ssize_t prefetch_store(struct kobject *kobj, struct kobj_attribute *attr,
			      const char *buf, size_t len)
{
	unsigned int l3, l1;

	if (sscanf(buf, "%u %u", &l3, &l1) != 2 || lru_cast_prefetch_set(l3, l1))
		return -EINVAL;
	return len;
}

static ssize_t balance_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return lru_cast_balance_show(buf);
}

static ssize_t floor_held_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return lru_cast_floor_held_show(buf);
}

static ssize_t sweep_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return lru_cast_sweep_show(buf);
}

#ifdef CONFIG_SWAP
DEFINE_STATIC_KEY_TRUE(kcompressd_enabled_key);
DEFINE_STATIC_KEY_FALSE(kcompressd_force_key);
int vm_kcompressd = 24;

static ssize_t kcompressd_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", READ_ONCE(vm_kcompressd));
}

static ssize_t kcompressd_store(struct kobject *kobj, struct kobj_attribute *attr,
				const char *buf, size_t len)
{
	int v;

	if (kstrtoint(buf, 10, &v) || v < -100 || v > 100)
		return -EINVAL;
	WRITE_ONCE(vm_kcompressd, v);
	if (v)
		static_branch_enable(&kcompressd_enabled_key);
	else
		static_branch_disable(&kcompressd_enabled_key);
	if (v < 0)
		static_branch_enable(&kcompressd_force_key);
	else
		static_branch_disable(&kcompressd_force_key);
	return len;
}

static struct kobj_attribute kcompressd_attr = __ATTR_RW(kcompressd);
#endif

static struct kobj_attribute sweep_attr = __ATTR_RO(sweep);
static struct kobj_attribute version_attr = __ATTR_RO(version);
static struct kobj_attribute enabled_attr = __ATTR_RW(enabled);
static struct kobj_attribute decay_attr = __ATTR_RW(decay);
static struct kobj_attribute decay_auto_attr = __ATTR_RW(decay_auto);
static struct kobj_attribute tune_attr = __ATTR_RO(tune);
static struct kobj_attribute w_permille_attr = __ATTR_RW(w_permille);
static struct kobj_attribute clock_attr = __ATTR_RO(clock);
static struct kobj_attribute clean_min_ratio_attr = __ATTR_RW(clean_min_ratio);
static struct kobj_attribute floor_held_attr = __ATTR_RO(floor_held);
static struct kobj_attribute balance_attr = __ATTR_RO(balance);
static struct kobj_attribute walker_attr = __ATTR_RW(walker);
static struct kobj_attribute walker_simd_attr = __ATTR_RW(walker_simd);
static struct kobj_attribute prefetch_attr = __ATTR_RW(prefetch);
static struct kobj_attribute estimate_attr = __ATTR_RO(estimate);

static struct attribute *lru_cast_attrs[] = {
	&version_attr.attr,
	&enabled_attr.attr,
	&decay_attr.attr,
	&decay_auto_attr.attr,
	&tune_attr.attr,
	&w_permille_attr.attr,
	&clock_attr.attr,
	&clean_min_ratio_attr.attr,
	&floor_held_attr.attr,
	&balance_attr.attr,
	&walker_attr.attr,
	&walker_simd_attr.attr,
	&prefetch_attr.attr,
	&estimate_attr.attr,
	&sweep_attr.attr,
#ifdef CONFIG_SWAP
	&kcompressd_attr.attr,
#endif
	NULL,
};

static const struct attribute_group lru_cast_group = {
	.name = "lru_cast",
	.attrs = lru_cast_attrs,
};

int lru_cast_sysfs_init(void)
{
	return sysfs_create_group(mm_kobj, &lru_cast_group);
}

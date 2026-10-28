// SPDX-License-Identifier: GPL-2.0
/*
 * CAST -- how far ahead the two prefetch legs reach (core/prefetch.h),
 * by the CPU, as Marie sets hers:
 *
 *	AVX-512F (Zen 4/5, Sapphire Rapids)	L2 MSHRs ~32	l3 32, l1 8
 *	AMD family 0x1a and later		~32		l3 32, l1 8
 *	AMD Zen 3 (family 0x19)			~24		l3 24, l1 8
 *	AMD Zen 1/2 (family 0x17)		~20		l3 20, l1 8
 *	AMD Excavator (family 0x15)		~12		l3 16, l1 6
 *	Intel Skylake and later (CLFLUSHOPT)	~24		l3 24, l1 8
 *	Intel Haswell, Broadwell		~16		l3 16, l1 6
 *	other AVX2				-		l3 16, l1 6
 *	no AVX2, or not x86			~8		l3  8, l1 2
 */
#include <linux/cache.h>
#include <linux/printk.h>
#ifdef CONFIG_X86
#include <asm/cpufeature.h>
#include <asm/processor.h>
#endif
#include "core/core.h"
#include "version.h"

unsigned int lru_cast_pf_l3 __read_mostly = 8;
unsigned int lru_cast_pf_l1 __read_mostly = 2;

void lru_cast_prefetch_init(void)
{
	unsigned int l3 = 8, l1 = 2;

#ifdef CONFIG_X86
	if (!boot_cpu_has(X86_FEATURE_AVX2))
		goto done;
	if (boot_cpu_has(X86_FEATURE_AVX512F)) {
		l3 = 32; l1 = 8;
		goto done;
	}
	switch (boot_cpu_data.x86_vendor) {
	case X86_VENDOR_AMD:
		if (boot_cpu_data.x86 >= 0x1a) {
			l3 = 32; l1 = 8;
		} else if (boot_cpu_data.x86 == 0x19) {
			l3 = 24; l1 = 8;
		} else if (boot_cpu_data.x86 == 0x17) {
			l3 = 20; l1 = 8;
		} else {
			l3 = 16; l1 = 6;
		}
		break;
	case X86_VENDOR_INTEL:
		/* CLFLUSHOPT as Skylake's mark: Haswell and Broadwell predate it */
		if (boot_cpu_has(X86_FEATURE_CLFLUSHOPT)) {
			l3 = 24; l1 = 8;
		} else {
			l3 = 16; l1 = 6;
		}
		break;
	default:
		l3 = 16; l1 = 6;
	}
done:
#endif
	lru_cast_prefetch_set(l3, l1);
	pr_info(LRU_CAST_PROGNAME ": prefetch l3 %u l1 %u ahead\n", l3, l1);
}

/* both legs set, the L1 one no further than the L3 one; 0 0: none */
int lru_cast_prefetch_set(unsigned int l3, unsigned int l1)
{
	if (l3 > LRU_CAST_PF_MAX || l1 > l3 || (l3 && !l1))
		return -EINVAL;
	WRITE_ONCE(lru_cast_pf_l3, l3);
	WRITE_ONCE(lru_cast_pf_l1, l1);
	return 0;
}

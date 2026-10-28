// SPDX-License-Identifier: GPL-2.0
/*
 * CAST -- the young-bit scan on x86-64: the widest of the three SIMD
 * kernels the CPU has, called directly (a static call, patched at boot).
 */
#include <linux/init.h>
#include <linux/static_call.h>
#include <asm/cpufeature.h>
#include <asm/fpu/api.h>
#include <asm/pgtable.h>
#include "walk/walk.h"
#include "walk/simd.h"

asmlinkage void lru_cast_simd_scan_sse2(const pte_t *table, unsigned long *bitmap);
asmlinkage void lru_cast_simd_scan_avx2(const pte_t *table, unsigned long *bitmap);
asmlinkage void lru_cast_simd_scan_avx512(const pte_t *table, unsigned long *bitmap);

DEFINE_STATIC_CALL(lru_cast_simd_scan, lru_cast_simd_scan_sse2);	/* SSE2: x86-64's floor */

static unsigned int lru_cast_simd_on = 1;
static const char *lru_cast_simd_isa = "sse2";

static void lru_cast_scan_scalar(const pte_t *pte, unsigned long *bitmap)
{
	int i;

	for (i = 0; i < 512; i++)
		if (pte_val(pte[i]) & _PAGE_ACCESSED)
			__set_bit(i, bitmap);
}

void lru_cast_young_pte_mask(const void *table, unsigned long *bitmap)
{
	if (!READ_ONCE(lru_cast_simd_on)) {
		lru_cast_scan_scalar(table, bitmap);
		return;
	}
	kernel_fpu_begin();
	static_call(lru_cast_simd_scan)(table, bitmap);
	kernel_fpu_end();
}

bool lru_cast_simd_enabled(void)
{
	return READ_ONCE(lru_cast_simd_on);
}

int lru_cast_simd_enable(bool on)
{
	WRITE_ONCE(lru_cast_simd_on, on);
	return 0;
}

const char *lru_cast_simd_name(void)
{
	return READ_ONCE(lru_cast_simd_on) ? lru_cast_simd_isa : "scalar";
}

static int __init lru_cast_simd_init(void)
{
	if (boot_cpu_has(X86_FEATURE_AVX512F)) {
		static_call_update(lru_cast_simd_scan, lru_cast_simd_scan_avx512);
		lru_cast_simd_isa = "avx512";
	} else if (boot_cpu_has(X86_FEATURE_AVX2)) {
		static_call_update(lru_cast_simd_scan, lru_cast_simd_scan_avx2);
		lru_cast_simd_isa = "avx2";
	}
	return 0;
}
arch_initcall(lru_cast_simd_init);

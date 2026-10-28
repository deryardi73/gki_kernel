# SPDX-License-Identifier: GPL-2.0
#
# CAST. Reached from mm/Makefile via obj-$(CONFIG_LRU_CAST) += lru_cast/.
# The rest of the kernel sees include/linux/lru_cast.h; everything here is
# private. Host differences live in compat/ and nowhere else, so these files
# are byte-identical on every kernel version.
#
# Components, each behind one header, each using only those below it
# (check-layers.sh):
#
#	core/	core.h		state words, the eviction clock, keys, prefetch
#	policy/	policy.h	the main cache inside reclaim, its parameters
#	walk/	walk.h		young bits of mapped pages
#	tune/	tune.h		the controllers that set the parameters
#	.	control.h	switching on and off, sysfs, and hooks.c --
#				the only files that see every component

ccflags-y += -I$(src) -I$(srctree)/mm

obj-y	+= core/store.o core/state.o core/clock.o core/key.o core/prefetch.o
obj-y	+= policy/params.o policy/ring.o policy/reclaim.o policy/ghost.o policy/balance.o
obj-y	+= walk/walk.o walk/bloom.o walk/lookaround.o
ifdef CONFIG_X86_64
obj-y	+= walk/simd_x86.o walk/simd_x86_sse2.o walk/simd_x86_avx2.o walk/simd_x86_avx512.o
else
obj-y	+= walk/simd_generic.o
endif
obj-y	+= tune/tune.o tune/sim.o tune/world.o tune/sample.o tune/readahead.o
obj-y	+= tune/evidence.o tune/decay.o tune/wshare.o tune/share.o tune/cost.o tune/protect.o
obj-y	+= hooks.o control.o sysfs.o

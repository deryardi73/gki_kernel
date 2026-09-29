/* SPDX-License-Identifier: GPL-2.0 */
#ifndef NAP_NEON_H
#define NAP_NEON_H

#include <asm/neon-intrinsics.h>

typedef float32x4_t v4sf;
typedef int32x4_t   v4si;

static inline v4sf v4sf_loadu(const float *p)
{
	return vld1q_f32(p);
}

static inline void v4sf_storeu(float *p, v4sf v)
{
	vst1q_f32(p, v);
}

static inline float fclampf(float v, float lo, float hi)
{
	if (v < lo) return lo;
	if (v > hi) return hi;
	return v;
}

static inline v4sf v4sf_clamp(v4sf v, v4sf lo, v4sf hi)
{
	return vmaxq_f32(vminq_f32(v, hi), lo);
}

static inline v4sf fast_log2f_v4(v4sf x)
{
	const v4si mask_exp  = vdupq_n_s32(0xFF);
	const v4si bias      = vdupq_n_s32(127);
	const v4si mask_mant = vdupq_n_s32(0x7FFFFF);
	const v4si exp_bias  = vdupq_n_s32(127 << 23);
	const v4sf one       = vdupq_n_f32(1.0f);

	v4si xi    = vreinterpretq_s32_f32(x);
	v4si exp_i = vsubq_s32(vandq_s32(vshrq_n_s32(xi, 23), mask_exp), bias);
	v4sf e     = vcvtq_f32_s32(exp_i);

	v4si mant_i = vorrq_s32(vandq_s32(xi, mask_mant), exp_bias);
	v4sf m      = vsubq_f32(vreinterpretq_f32_s32(mant_i), one);

	v4sf p;
	p = vmulq_f32(m, vdupq_n_f32(0.4808f));
	p = vsubq_f32(vdupq_n_f32(0.7213f), p);
	p = vmulq_f32(m, p);
	p = vsubq_f32(vdupq_n_f32(1.4425f), p);
	p = vmulq_f32(m, p);

	return vaddq_f32(e, p);
}

#endif

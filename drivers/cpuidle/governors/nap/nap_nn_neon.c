// SPDX-License-Identifier: GPL-2.0
#include "nap.h"

void nap_nn_forward_neon(const float *input,
			 float *output,
			 float *hidden_save,
			 const struct nap_weights *w)
{
	int j;

	float32x4_t acc0 = vld1q_f32(&w->b_h1[0]);
	float32x4_t acc1 = vld1q_f32(&w->b_h1[4]);

	for (j = 0; j < NAP_INPUT_SIZE; j++) {
		float32x4_t x = vdupq_n_f32(input[j]);

		acc0 = vfmaq_f32(acc0, vld1q_f32(&w->w_h1[j][0]), x);
		acc1 = vfmaq_f32(acc1, vld1q_f32(&w->w_h1[j][4]), x);
	}

	{
		float32x4_t zero = vdupq_n_f32(0.0f);

		acc0 = vmaxq_f32(acc0, zero);
		acc1 = vmaxq_f32(acc1, zero);
	}
	vst1q_f32(&hidden_save[0], acc0);
	vst1q_f32(&hidden_save[4], acc1);

	{
		float32x4_t p = vmulq_f32(vld1q_f32(&w->w_out[0]), acc0);

		p = vfmaq_f32(p, vld1q_f32(&w->w_out[4]), acc1);
		*output = vaddvq_f32(p) + w->b_out;
	}
}

void nap_nn_learn_neon(struct nap_cpu_data *d)
{
	int i;
	float d_out_scalar = d->learn_d_out;
	float *d_hid = d->learn_d_hid;
	float lr = d->learn_lr;
	float clamp_val = (float)d->max_grad_norm_millths / 1000.0f;
	float32x4_t v_lr    = vdupq_n_f32(lr);
	float32x4_t v_cl_hi = vdupq_n_f32(clamp_val);
	float32x4_t v_cl_lo = vdupq_n_f32(-clamp_val);
	float32x4_t h0 = vld1q_f32(&d->hidden_out[0]);
	float32x4_t h1 = vld1q_f32(&d->hidden_out[4]);
	float32x4_t vd = vdupq_n_f32(d_out_scalar);
	float32x4_t dh0, dh1;

	{
		float32x4_t zero = vdupq_n_f32(0.0f);
		float32x4_t g0 = vmulq_f32(vld1q_f32(&d->active_w->w_out[0]), vd);
		float32x4_t g1 = vmulq_f32(vld1q_f32(&d->active_w->w_out[4]), vd);
		uint32x4_t  m0 = vcgtq_f32(h0, zero);
		uint32x4_t  m1 = vcgtq_f32(h1, zero);

		dh0 = vreinterpretq_f32_u32(vandq_u32(vreinterpretq_u32_f32(g0), m0));
		dh1 = vreinterpretq_f32_u32(vandq_u32(vreinterpretq_u32_f32(g1), m1));
		vst1q_f32(&d_hid[0], dh0);
		vst1q_f32(&d_hid[4], dh1);
	}

	{
		float *w = &d->active_w->w_out[0];

		vst1q_f32(&w[0], vsubq_f32(vld1q_f32(&w[0]),
			vmulq_f32(v_lr, v4sf_clamp(vmulq_f32(h0, vd),
						   v_cl_lo, v_cl_hi))));
		vst1q_f32(&w[4], vsubq_f32(vld1q_f32(&w[4]),
			vmulq_f32(v_lr, v4sf_clamp(vmulq_f32(h1, vd),
						   v_cl_lo, v_cl_hi))));
	}

	d->active_w->b_out -= lr * fclampf(d_out_scalar, -clamp_val, clamp_val);

	for (i = 0; i < NAP_INPUT_SIZE; i++) {
		float32x4_t vf = vdupq_n_f32(d->features_f32[i]);
		float *w = &d->active_w->w_h1[i][0];

		vst1q_f32(&w[0], vsubq_f32(vld1q_f32(&w[0]),
			vmulq_f32(v_lr, v4sf_clamp(vmulq_f32(vf, dh0),
						   v_cl_lo, v_cl_hi))));
		vst1q_f32(&w[4], vsubq_f32(vld1q_f32(&w[4]),
			vmulq_f32(v_lr, v4sf_clamp(vmulq_f32(vf, dh1),
						   v_cl_lo, v_cl_hi))));
	}

	{
		float *b = &d->active_w->b_h1[0];

		vst1q_f32(&b[0], vsubq_f32(vld1q_f32(&b[0]),
			vmulq_f32(v_lr, v4sf_clamp(dh0, v_cl_lo, v_cl_hi))));
		vst1q_f32(&b[4], vsubq_f32(vld1q_f32(&b[4]),
			vmulq_f32(v_lr, v4sf_clamp(dh1, v_cl_lo, v_cl_hi))));
	}
}

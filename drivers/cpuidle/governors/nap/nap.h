/* SPDX-License-Identifier: GPL-2.0 */
#ifndef NAP_H
#define NAP_H

#include <linux/cpuidle.h>
#include <linux/jump_label.h>
#include <linux/ktime.h>

/* ================================================================
 * Neural network dimensions
 * ================================================================ */

#define NAP_INPUT_SIZE    8
#define NAP_HIDDEN_SIZE   8
#define NAP_NUM_CUTS      (CPUIDLE_STATE_MAX - 1)

/*
 * Neural network weights for an 8-input MLP with an ordinal survival head.
 *
 * The trunk maps input[8] → hidden[8] (ReLU), feeding a shared linear score
 *   s = w_out . hidden + b_out
 * which is the input to a proportional-odds ordinal head. For each idle-state
 * boundary k the predicted survival probability that the upcoming idle reaches
 * that state's target_residency is
 *   q_k = sigmoid(s - thr_ord[k-1]).
 * With ordered thresholds this represents the idle-duration distribution at
 * exactly the points the decision needs (the sufficient statistic), rather
 * than a single point estimate. The decision layer compares q_k against a
 * calibrated confidence level (see nap_fpu_select()).
 *
 * Column-major storage: w_h1[j][i] = weight from input j to hidden neuron i.
 * This layout enables efficient column-wise matrix-vector products where
 * each input broadcasts across all hidden neurons via SIMD FMA.
 *
 * thr_ord is appended after the SIMD-accessed fields so their offsets are
 * unchanged. __aligned(32) ensures AVX2 vmovaps (32-byte) aligned loads
 * work correctly (8 floats = 32 bytes = one ymm register).
 */
struct nap_weights {
	/* Hidden layer: input[8] → hidden[8] */
	float w_h1[NAP_INPUT_SIZE][NAP_HIDDEN_SIZE];  /* 64 params */
	float b_h1[NAP_HIDDEN_SIZE];                   /* 8 params  */
	/* Shared score head: hidden[8] → scalar s */
	float w_out[NAP_HIDDEN_SIZE];                  /* 8 params  */
	float b_out;                                   /* 1 param   */
	/* Ordinal survival head: one ordered threshold per state boundary */
	float thr_ord[NAP_NUM_CUTS];
} __aligned(32);

/* ISA-specific forward pass implementations */
void nap_nn_forward_sse2(const float *input, float *output,
			 float *hidden_save, const struct nap_weights *w);
void nap_nn_forward_avx2(const float *input, float *output,
			 float *hidden_save, const struct nap_weights *w);

/* ISA-specific online learning (backpropagation) */
struct nap_cpu_data;
void nap_nn_learn_sse2(struct nap_cpu_data *d);
void nap_nn_learn_avx2(struct nap_cpu_data *d);

/* Static key for ISA dispatch (defined in nap.c) */
DECLARE_STATIC_KEY_FALSE(nap_use_avx2);

/* ================================================================
 * SIMD type definitions and helpers (GCC vector extensions)
 *
 * Only available when compiled with FPU/SSE flags (nap_fpu.c,
 * nap_nn_*.c).  nap.c is compiled without FPU flags and must
 * not see these definitions.
 *
 * <immintrin.h> is a userspace header and cannot be used in kernel.
 * We use __attribute__((__vector_size__())) and __builtin_ia32_*.
 * ================================================================ */

#ifdef __SSE2__

typedef float v4sf  __attribute__((__vector_size__(16)));   /* xmm: 4×float  */
typedef int   v4si  __attribute__((__vector_size__(16)));   /* xmm: 4×int32  */
typedef float v8sf  __attribute__((__vector_size__(32)));   /* ymm: 8×float  */

/* Broadcast helpers */
#define V4SF_SET1(x)  ((v4sf){ (x), (x), (x), (x) })
#define V4SI_SET1(x)  ((v4si){ (x), (x), (x), (x) })
#define V8SF_SET1(x)  ((v8sf){ (x),(x),(x),(x),(x),(x),(x),(x) })
#define V8SF_ZERO     V8SF_SET1(0.0f)

/* Unaligned load/store helpers */
static inline v4sf v4sf_loadu(const float *p)
{
	v4sf result;
	__builtin_memcpy(&result, p, sizeof(result));
	return result;
}

static inline void v4sf_storeu(float *p, v4sf v)
{
	__builtin_memcpy(p, &v, sizeof(v));
}

#ifdef __AVX__
static inline v8sf v8sf_loadu(const float *p)
{
	v8sf result;
	__builtin_memcpy(&result, p, sizeof(result));
	return result;
}

static inline void v8sf_storeu(float *p, v8sf v)
{
	__builtin_memcpy(p, &v, sizeof(v));
}
#endif /* __AVX__ */

/* Scalar/vector clamp helpers */
static inline float fclampf(float v, float lo, float hi)
{
	if (v < lo) return lo;
	if (v > hi) return hi;
	return v;
}

static inline v4sf v4sf_clamp(v4sf v, v4sf lo, v4sf hi)
{
	return __builtin_ia32_maxps(__builtin_ia32_minps(v, hi), lo);
}

/* Type punning: float ↔ int reinterpret (no instruction generated) */
static inline v4si v4sf_as_v4si(v4sf v)
{
	union { v4sf f; v4si i; } u = { .f = v };
	return u.i;
}

static inline v4sf v4si_as_v4sf(v4si v)
{
	union { v4si i; v4sf f; } u = { .i = v };
	return u.f;
}

/*
 * fast_log2f_sse() — Compute log2 of 4 floats simultaneously using SSE2
 *
 * Cost: ~15 cycles for 4 values (~4 cycles per value)
 */
static inline v4sf fast_log2f_sse(v4sf x)
{
	const v4si mask_exp  = V4SI_SET1(0xFF);
	const v4si bias      = V4SI_SET1(127);
	const v4si mask_mant = V4SI_SET1(0x7FFFFF);
	const v4si exp_bias  = V4SI_SET1(127 << 23);

	v4si xi    = v4sf_as_v4si(x);
	v4si exp_i = (xi >> 23) & mask_exp;
	exp_i      = exp_i - bias;
	v4sf e     = __builtin_convertvector(exp_i, v4sf);

	v4si mant_i = (xi & mask_mant) | exp_bias;
	v4sf m      = v4si_as_v4sf(mant_i) - V4SF_SET1(1.0f);

	v4sf p;
	p = m * V4SF_SET1(0.4808f);
	p = V4SF_SET1(0.7213f) - p;
	p = m * p;
	p = V4SF_SET1(1.4425f) - p;
	p = m * p;

	return e + p;
}

#endif /* __SSE2__ */

/* ================================================================
 * Feature extraction
 * ================================================================ */

#define NAP_HISTORY_SIZE     8

/* ================================================================
 * POLL short-circuit tunables
 * ================================================================ */

/* dev->poll_limit_ns floor and safety margin written by
 * nap_compute_poll_limit().  Both 1 us: the POLL state samples its
 * own timeout only every ~1 us (POLL_IDLE_RELAX_COUNT cpu_relax()
 * iterations in poll_state.c), so finer values are indistinguishable.
 */
#define NAP_POLL_LIMIT_MIN_NS      1000ULL
#define NAP_POLL_LIMIT_MARGIN_NS   1000ULL

/* Refresh interval for the cached minimum-valid-state lookup.  HZ
 * jiffies (1 s) bounds staleness from sysfs/runtime state-disable
 * events; PM QoS latency changes are detected immediately via the
 * cached latency_req comparison.
 */
#define NAP_MIN_STATE_REFRESH_JIFFIES  HZ

struct nap_stats {
	u64 total_selects;
	u64 total_residency_ns;
	u64 overshoot_count;
	u64 learn_count;
};

struct nap_cpu_data {
	/* Ring buffer */
	u64   history[NAP_HISTORY_SIZE];
	float log_history[NAP_HISTORY_SIZE];
	int   hist_idx;
	int   hist_count;

	/* External signal tracking */
	u64     prev_idle_exit;
	s64     last_predicted_ns;
	s64     last_prediction_error;

	/* POLL short-circuit fast path */
	bool short_circuited;			/* set in select, read in reflect */
	int  cached_min_state;			/* cached shallowest valid state */
	s64  cached_min_state_latency;		/* latency_req when cache populated */
	unsigned long cached_min_state_jiffies;	/* jiffies when cache populated */

	/* Jiffies-based learning rate floor */
	unsigned long last_learn_jiffies;
	unsigned int  learn_jiffies_min;	/* 0 = disabled */

	/* select/reflect handoff */
	int   last_selected_idx;

	/* Shared ordinal score s (≈ log2 of the predicted idle duration in ns).
	 * Survival at boundary k is sigmoid(s - thr_ord[k-1]).
	 */
	float nn_output;

	/*
	 * hidden_out[], features_f32[] are written with aligned SIMD
	 * stores in nap_nn_forward_{sse2,avx2}() and
	 * nap_extract_features():
	 *   SSE2:    movaps  (16-byte aligned)
	 *   AVX2:    vmovaps (32-byte aligned)
	 * Without __aligned(32), the natural struct offset would be
	 * only 4-byte aligned, causing #GP faults in the idle task.
	 */
	float hidden_out[NAP_HIDDEN_SIZE] __aligned(32);
	float features_f32[NAP_INPUT_SIZE] __aligned(32);

	/* Backprop scratch */
	float learn_d_out;	/* score gradient g = sum_k (q_k - y_k) */
	float learn_lr;		/* effective learning rate (symmetric) */
	float learn_d_hid[NAP_HIDDEN_SIZE] __aligned(32);

	/* Precomputed per-state log2 thresholds.
	 * log2_tres[i] = log2(target_residency_ns) (ordinal thresholds, timer clamp)
	 */
	float log2_tres[CPUIDLE_STATE_MAX];

	/* Decayed per-bin idle histogram: robustness-floor survival estimate */
	float bin_count[CPUIDLE_STATE_MAX];

	/* Deferred learning data */
	bool  needs_learn;
	bool  have_sample;	/* a fresh residency awaits per-idle processing */
	u64   learn_actual_ns;

	/* Single network: 16→16 trunk + ordinal survival head */
	struct nap_weights weights;
	struct nap_weights *active_w;	/* always &weights; consumed by SIMD forward/learn */

	/* Online learning */
	unsigned int learning_rate_millths;
	unsigned int max_grad_norm_millths;
	unsigned int conf_millths;	/* decision confidence level (500 = 0.5) */
	int   learn_interval;
	int   learn_counter;
	bool reset_pending;		/* set by sysfs, consumed by nap_select */

	/* sysfs statistics */
	struct nap_stats stats;
};

DECLARE_PER_CPU(struct nap_cpu_data, nap_data);

/* FPU entry point (nap_fpu.c) — call only within kernel_fpu_begin/end */
int nap_fpu_select(struct cpuidle_driver *drv,
		   struct cpuidle_device *dev,
		   struct nap_cpu_data *d);

/* sysfs interface */
int  nap_sysfs_init(void);
void nap_sysfs_exit(void);

#endif /* NAP_H */

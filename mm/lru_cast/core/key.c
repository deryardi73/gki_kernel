// SPDX-License-Identifier: GPL-2.0
/*
 * CAST -- the key of a state word.
 *
 * The score of a folio is its read count decayed with a half-life of H
 * pages taken in (core/clock.c), H = C * 2^(8 - le), le from LRU_CAST_LE_MIN
 * (H = 128 C) to LRU_CAST_LE_MAX:
 *
 *   s = sum_j 2^-(age_j / H) + max(n - 3, 0) * 2^-(age_oldest / H)
 *
 * over the last three reads, the newest's age exact, the two before it
 * each a gap back known to within a factor of two (core/core.h) and taken
 * at its middle.
 *
 * It factors as 2^(-E/H) * K, E the clock, so the order of two folios not
 * read does not change as E grows: it is the order of the key
 *
 *   key = E/H + log2 s   (half-lives),
 *
 * which changes only when the folio is read. Keys are counted in buckets of
 * 2^-res half-lives: the main cache's sweep (policy/ring.c) in sixteenths,
 * the controllers' simulated caches (tune/) in quarters, in a ring of
 * LRU_CAST_BUCKETS of them anchored at now: [top - 255, top], top = 4 E/H
 * + 32 (a folio read just now has log2 s < 8). With no decay (le = 0,
 * which the policy no longer runs at: core/core.h) the key is the read
 * count.
 *
 * Pure arithmetic on a state word and a clock.
 */
#include "core/core.h"

/* 2^(-f/256), Q16 */
static const u32 lru_cast_exp2n[256] = {
	65536, 65359, 65182, 65006, 64830, 64655, 64480, 64306,
	64132, 63958, 63785, 63613, 63441, 63269, 63098, 62928,
	62757, 62588, 62419, 62250, 62081, 61914, 61746, 61579,
	61413, 61247, 61081, 60916, 60751, 60587, 60423, 60260,
	60097, 59934, 59772, 59611, 59449, 59289, 59128, 58968,
	58809, 58650, 58491, 58333, 58176, 58018, 57861, 57705,
	57549, 57393, 57238, 57083, 56929, 56775, 56622, 56468,
	56316, 56163, 56012, 55860, 55709, 55558, 55408, 55258,
	55109, 54960, 54811, 54663, 54515, 54368, 54221, 54074,
	53928, 53782, 53637, 53492, 53347, 53203, 53059, 52916,
	52773, 52630, 52488, 52346, 52204, 52063, 51922, 51782,
	51642, 51502, 51363, 51224, 51085, 50947, 50810, 50672,
	50535, 50399, 50262, 50126, 49991, 49856, 49721, 49586,
	49452, 49319, 49185, 49052, 48920, 48787, 48655, 48524,
	48393, 48262, 48131, 48001, 47871, 47742, 47613, 47484,
	47356, 47228, 47100, 46973, 46846, 46719, 46593, 46467,
	46341, 46216, 46091, 45966, 45842, 45718, 45594, 45471,
	45348, 45225, 45103, 44981, 44859, 44738, 44617, 44497,
	44376, 44256, 44137, 44017, 43898, 43780, 43661, 43543,
	43425, 43308, 43191, 43074, 42958, 42841, 42726, 42610,
	42495, 42380, 42265, 42151, 42037, 41923, 41810, 41697,
	41584, 41472, 41360, 41248, 41136, 41025, 40914, 40804,
	40693, 40583, 40473, 40364, 40255, 40146, 40037, 39929,
	39821, 39714, 39606, 39499, 39392, 39286, 39180, 39074,
	38968, 38863, 38757, 38653, 38548, 38444, 38340, 38236,
	38133, 38030, 37927, 37824, 37722, 37620, 37518, 37417,
	37316, 37215, 37114, 37014, 36914, 36814, 36715, 36615,
	36516, 36417, 36319, 36221, 36123, 36025, 35928, 35831,
	35734, 35637, 35541, 35445, 35349, 35253, 35158, 35063,
	34968, 34874, 34779, 34685, 34591, 34498, 34405, 34312,
	34219, 34126, 34034, 33942, 33850, 33759, 33667, 33576,
	33486, 33395, 33305, 33215, 33125, 33035, 32946, 32857,
};

/* 256 * log2(1 + m/256) */
static const u8 lru_cast_log2m[256] = {
	0, 1, 3, 4, 6, 7, 9, 10, 11, 13, 14, 16,
	17, 18, 20, 21, 22, 24, 25, 26, 28, 29, 30, 32,
	33, 34, 36, 37, 38, 40, 41, 42, 44, 45, 46, 47,
	49, 50, 51, 52, 54, 55, 56, 57, 59, 60, 61, 62,
	63, 65, 66, 67, 68, 69, 71, 72, 73, 74, 75, 77,
	78, 79, 80, 81, 82, 84, 85, 86, 87, 88, 89, 90,
	92, 93, 94, 95, 96, 97, 98, 99, 100, 102, 103, 104,
	105, 106, 107, 108, 109, 110, 111, 112, 113, 114, 116, 117,
	118, 119, 120, 121, 122, 123, 124, 125, 126, 127, 128, 129,
	130, 131, 132, 133, 134, 135, 136, 137, 138, 139, 140, 141,
	142, 143, 144, 145, 146, 147, 148, 149, 150, 151, 152, 153,
	154, 155, 155, 156, 157, 158, 159, 160, 161, 162, 163, 164,
	165, 166, 167, 168, 169, 169, 170, 171, 172, 173, 174, 175,
	176, 177, 178, 178, 179, 180, 181, 182, 183, 184, 185, 185,
	186, 187, 188, 189, 190, 191, 192, 192, 193, 194, 195, 196,
	197, 198, 198, 199, 200, 201, 202, 203, 203, 204, 205, 206,
	207, 208, 208, 209, 210, 211, 212, 212, 213, 214, 215, 216,
	216, 217, 218, 219, 220, 220, 221, 222, 223, 224, 224, 225,
	226, 227, 228, 228, 229, 230, 231, 231, 232, 233, 234, 234,
	235, 236, 237, 238, 238, 239, 240, 241, 241, 242, 243, 244,
	244, 245, 246, 247, 247, 248, 249, 249, 250, 251, 252, 252,
	253, 254, 255, 255,
};

/* tau / H in Q16 half-lives per tick: 2^(le - 10) */
static inline u64 lru_cast_dq(int le)
{
	return le ? 1ULL << (le + 6) : 0;
}

/* 2^-(age tau / H), Q16, the age in half ticks */
static u64 lru_cast_weight(u32 age2, int le)
{
	u64 x = (u64)age2 * lru_cast_dq(le) >> 1;
	u32 xi = x >> 16;

	if (xi >= 32)
		return 0;
	return lru_cast_exp2n[(x >> 8) & 255] >> xi;
}

/*
 * the gap of code @c (lru_cast_gap_code()), in half ticks: the middle of
 * [0, 1], or of [2^c, 2^(c+1) - 1]
 */
static u32 lru_cast_gap2(u32 c)
{
	return c ? 3 * (1U << c) - 1 : 1;
}

/* 256 log2(v / 2^16) */
static s64 lru_cast_log2q8(u64 v)
{
	int msb = fls64(v) - 1;
	u32 m = (msb >= 8 ? v >> (msb - 8) : v << (8 - msb)) & 255;

	return ((s64)msb - 16) * 256 + lru_cast_log2m[m];
}

/* the bucket of a state word: 2^-@res half-lives of the key, or n */
long lru_cast_key_res(u32 w, unsigned long tick, int le, int res)
{
	u32 n = w >> 24, m = min(n, 3u), j, rel = 0;
	u8 age0 = tick - (w & 0xff);
	unsigned long last = tick >= age0 ? tick - age0 : 0;
	u64 s = 0, wt = 0;

	if (!le)
		return n;
	if (!n)
		return LONG_MIN;
	/*
	 * The key is the same taken at any time; taken at the last read, its
	 * weight is 1 and the sum cannot underflow however long ago that was.
	 */
	for (j = 0; j < m; j++) {
		if (j)		/* the middle of the gap its code allows, in half ticks */
			rel += lru_cast_gap2((w >> (LRU_CAST_W_GAP + 3 * (j - 1))) & 7);
		wt = lru_cast_weight(rel, le);
		s += wt;
	}
	if (n > 3)
		s += (u64)(n - 3) * wt;		/* older reads count at the oldest stamp */
	return ((s64)((last * lru_cast_dq(le)) >> 8) + lru_cast_log2q8(s)) >> (8 - res);
}

/* quarter half-lives */
long lru_cast_key(u32 w, unsigned long tick, int le)
{
	return lru_cast_key_res(w, tick, le, 2);
}

long lru_cast_window_lo(unsigned long tick, int le)
{
	if (!le)
		return 0;
	return (long)((((tick * lru_cast_dq(le)) >> 8) + 8 * 256) >> 6) - (LRU_CAST_BUCKETS - 1);
}

/*
 * SPDX-License-Identifier: Apache-2.0
 */
#include "pod/iforest.h"

#include <math.h>
#include <string.h>

#define LN2         0.69314718f
#define EULER_GAMMA 0.57721566f

static uint32_t rng_next(uint32_t *s)
{
	uint32_t x = *s ? *s : 0x9e3779b9u; /* xorshift32 */

	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	*s = x;
	return x;
}

/* Uniform in the open interval (0, 1). */
static float rng_unit(uint32_t *s)
{
	return ((float)(rng_next(s) >> 8) + 0.5f) * (1.0f / 16777216.0f);
}

/* Uniform in 0..n-1 (n >= 1). */
static unsigned rng_below(uint32_t *s, unsigned n)
{
	return (unsigned)(((uint64_t)rng_next(s) * n) >> 32);
}

float iforest_c(unsigned n)
{
	if (n <= 1) {
		return 0.0f;
	}
	if (n == 2) {
		return 1.0f;
	}
	return 2.0f * (logf((float)(n - 1)) + EULER_GAMMA) - 2.0f * (float)(n - 1) / (float)n;
}

static unsigned effective_samples(unsigned n, unsigned max_samples)
{
	unsigned s = max_samples ? max_samples : IFOREST_DEFAULT_SAMPLES;

	return s < n ? s : n;
}

size_t iforest_scratch_size(unsigned n, unsigned d, unsigned max_samples)
{
	return IFOREST_SCRATCH_SIZE(n, d, effective_samples(n, max_samples));
}

struct node {
	uint16_t lo, hi;  /* range in idx[] */
	uint16_t nsub;    /* subsample points in the range */
	uint16_t depth;
};

int iforest_score(const float *x, unsigned n, unsigned d, unsigned stride,
		  const struct iforest_params *p, uint32_t *rng,
		  void *scratch, size_t scratch_size, float *score)
{
	if (!x || !p || !rng || !scratch || !score || n == 0 || d == 0 || stride < d ||
	    n > IFOREST_MAX_POINTS || d > IFOREST_MAX_FEATURES || p->n_trees == 0) {
		return -1;
	}
	const unsigned s = effective_samples(n, p->max_samples);

	if (scratch_size < IFOREST_SCRATCH_SIZE(n, d, s)) {
		return -1;
	}

	/* Carve scratch: floats first, then uint16, then bytes (keeps alignment). */
	float *path = (float *)scratch;
	float *c_tab = path + n;                       /* s + 1 */
	uint16_t *idx = (uint16_t *)(c_tab + s + 1u);  /* n */
	uint16_t *perm = idx + n;                      /* d */
	struct node *stack = (struct node *)(perm + d); /* s + 1 nodes of 4 x u16 */
	uint8_t *insub = (uint8_t *)(stack + s + 1u);   /* n */

	for (unsigned k = 0; k <= s; k++) {
		c_tab[k] = iforest_c(k);
	}
	for (unsigned f = 0; f < d; f++) {
		perm[f] = (uint16_t)f;
	}
	memset(path, 0, n * sizeof(float));

	for (unsigned t = 0; t < p->n_trees; t++) {
		unsigned sp = 0;

		/* Random subsample of s points: partial Fisher-Yates on idx, then
		 * mark membership by point. */
		for (unsigned i = 0; i < n; i++) {
			idx[i] = (uint16_t)i;
		}
		if (s < n) {
			memset(insub, 0, n);
			for (unsigned i = 0; i < s; i++) {
				unsigned j = i + rng_below(rng, n - i);
				uint16_t tmp = idx[i];

				idx[i] = idx[j];
				idx[j] = tmp;
				insub[idx[i]] = 1;
			}
		} else {
			memset(insub, 1, n);
		}

		stack[sp++] = (struct node){ 0, (uint16_t)n, (uint16_t)s, 0 };

		while (sp > 0) {
			struct node nd = stack[--sp];
			unsigned f = 0;
			float lo = 0.0f, hi = 0.0f;
			int found = 0;

			if (nd.nsub > 1) {
				/* Try features in random order (lazy Fisher-Yates) until one
				 * varies across the node's subsample points. The first such
				 * feature is uniform among the varying ones, and usually the
				 * first try succeeds, so a node costs one column scan. */
				for (unsigned k = 0; k < d && !found; k++) {
					unsigned j = k + rng_below(rng, d - k);
					uint16_t tmp = perm[k];
					int first = 1;

					perm[k] = perm[j];
					perm[j] = tmp;
					f = perm[k];
					for (unsigned m = nd.lo; m < nd.hi; m++) {
						if (!insub[idx[m]]) {
							continue;
						}
						float v = x[(size_t)idx[m] * stride + f];

						if (first) {
							lo = hi = v;
							first = 0;
						} else {
							lo = v < lo ? v : lo;
							hi = v > hi ? v : hi;
						}
					}
					found = hi > lo;
				}
			}

			if (!found) {
				/* Leaf: subsample isolated, or its points identical. */
				float h = (float)nd.depth + c_tab[nd.nsub];

				for (unsigned m = nd.lo; m < nd.hi; m++) {
					path[idx[m]] += h;
				}
				continue;
			}

			float split = lo + rng_unit(rng) * (hi - lo);

			if (!(split > lo)) {
				split = hi; /* rounding on a tiny range; keep both sides non-empty */
			}
			unsigned i = nd.lo, j = nd.hi, nsub_left = 0;

			while (i < j) {
				if (x[(size_t)idx[i] * stride + f] < split) {
					nsub_left += insub[idx[i]];
					i++;
				} else {
					uint16_t tmp = idx[i];

					idx[i] = idx[--j];
					idx[j] = tmp;
				}
			}
			/* Both children hold at least one subsample point, so the
			 * stack never exceeds s + 1 entries. */
			stack[sp++] = (struct node){ nd.lo, (uint16_t)i, (uint16_t)nsub_left,
						     (uint16_t)(nd.depth + 1u) };
			stack[sp++] = (struct node){ (uint16_t)i, nd.hi, (uint16_t)(nd.nsub - nsub_left),
						     (uint16_t)(nd.depth + 1u) };
		}
	}

	const float cn = c_tab[s];

	for (unsigned i = 0; i < n; i++) {
		score[i] = cn > 0.0f ? expf(-LN2 * (path[i] / (float)p->n_trees) / cn) : 0.5f;
	}
	return 0;
}

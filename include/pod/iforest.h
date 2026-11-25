/*
 * Isolation Forest, fit-and-score in one pass, no stored model.
 *
 * Scores n points of d features each. Every tree is grown on a random
 * subsample of at most max_samples points, all n points are routed through it
 * while it is built, and the tree is then discarded, so memory does not grow
 * with the number of trees. Trees are grown until the subsample is fully
 * isolated (no depth limit), which keeps the score discriminating for small n.
 *
 * Splits are drawn uniformly between a feature's min and max in the node, so
 * scores do not depend on the units or scale of any feature.
 *
 * Pure C99 plus <math.h>: no globals, no allocation, no I/O. The caller
 * provides scratch memory of iforest_scratch_size(n, d, max_samples) bytes.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef POD_IFOREST_H
#define POD_IFOREST_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IFOREST_MAX_POINTS   65535u
#define IFOREST_MAX_FEATURES 65535u
#define IFOREST_DEFAULT_SAMPLES 256u

struct iforest_params {
	uint16_t n_trees;     /* number of trees, e.g. 100 */
	uint16_t max_samples; /* subsample per tree; 0 = 256 (the usual choice) */
};

/*
 * Scratch bytes needed. A compile-time version for static buffers:
 * IFOREST_SCRATCH_SIZE(n, d, s) with s = effective subsample (min(n, max_samples)).
 */
#define IFOREST_SCRATCH_SIZE(n, d, s)                                                  \
	(sizeof(float) * ((size_t)(n) + (size_t)(s) + 1u) +   /* path, c table   */  \
	 sizeof(uint16_t) * ((size_t)(n) + (size_t)(d) + 4u * ((size_t)(s) + 1u)) +      \
	 (size_t)(n) + 8u)                                    /* in-sample flags */

size_t iforest_scratch_size(unsigned n, unsigned d, unsigned max_samples);

/*
 * Scores rows 0..n-1 of x. Row i starts at x + i * stride (stride >= d,
 * counted in floats), so a column subset of a wider table can be scored in
 * place. score[i] is in 0..1: about 0.5 is ordinary, near 1 is easily isolated.
 *
 * rng is the caller's random state (any non-zero value), advanced in place.
 * Returns 0, or -1 for bad arguments or too little scratch.
 */
int iforest_score(const float *x, unsigned n, unsigned d, unsigned stride,
		  const struct iforest_params *p, uint32_t *rng,
		  void *scratch, size_t scratch_size, float *score);

/* Average path length of an unsuccessful BST search over n points: c(n). */
float iforest_c(unsigned n);

#ifdef __cplusplus
}
#endif

#endif /* POD_IFOREST_H */

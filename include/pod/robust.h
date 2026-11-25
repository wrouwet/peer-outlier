/*
 * Robust statistics for small samples: median, MAD, modified z-scores.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef POD_ROBUST_H
#define POD_ROBUST_H

#ifdef __cplusplus
extern "C" {
#endif

#define ROBUST_Z_MAX 99.0f

struct robust_stats {
	float med;    /* median */
	float mad;    /* median absolute deviation from the median */
	float meanad; /* mean absolute deviation from the median */
};

/* Median of v[0..n-1]; sorts v in place. n >= 1. */
float robust_median_inplace(float *v, unsigned n);

/* Stats of v[0..n-1]; tmp is n floats of scratch. v is not modified. */
void robust_compute(const float *v, unsigned n, float *tmp, struct robust_stats *s);

/* Modified z-score (Iglewicz and Hoaglin), with their mean-absolute-deviation
 * fallback when the MAD is zero, clamped to +-ROBUST_Z_MAX. */
float robust_z(float v, const struct robust_stats *s);

/* z with P(Z > z) = p for a standard normal, 0 < p <= 0.5 (error < 4.5e-4). */
float robust_normal_upper_quantile(float p);

#ifdef __cplusplus
}
#endif

#endif /* POD_ROBUST_H */

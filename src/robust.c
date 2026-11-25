/*
 * SPDX-License-Identifier: Apache-2.0
 */
#include "pod/robust.h"

#include <math.h>
#include <string.h>

float robust_median_inplace(float *v, unsigned n)
{
	/* Insertion sort: n is a group size (tens to hundreds). */
	for (unsigned i = 1; i < n; i++) {
		float key = v[i];
		unsigned j = i;

		while (j > 0 && v[j - 1] > key) {
			v[j] = v[j - 1];
			j--;
		}
		v[j] = key;
	}
	return (n & 1u) ? v[n / 2] : 0.5f * (v[n / 2 - 1] + v[n / 2]);
}

void robust_compute(const float *v, unsigned n, float *tmp, struct robust_stats *s)
{
	float sum = 0.0f;

	memcpy(tmp, v, n * sizeof(float));
	s->med = robust_median_inplace(tmp, n);
	for (unsigned i = 0; i < n; i++) {
		tmp[i] = fabsf(v[i] - s->med);
		sum += tmp[i];
	}
	s->meanad = sum / (float)n;
	s->mad = robust_median_inplace(tmp, n);
}

float robust_z(float v, const struct robust_stats *s)
{
	float z;

	if (s->mad > 0.0f) {
		z = 0.6745f * (v - s->med) / s->mad;
	} else if (s->meanad > 0.0f) {
		z = (v - s->med) / (1.253314f * s->meanad);
	} else {
		return 0.0f;
	}
	if (z > ROBUST_Z_MAX) {
		return ROBUST_Z_MAX;
	}
	if (z < -ROBUST_Z_MAX) {
		return -ROBUST_Z_MAX;
	}
	return z;
}

float robust_normal_upper_quantile(float p)
{
	/* Abramowitz and Stegun 26.2.23 */
	float t = sqrtf(-2.0f * logf(p));

	return t - (2.515517f + t * (0.802853f + t * 0.010328f)) /
		       (1.0f + t * (1.432788f + t * (0.189269f + t * 0.001308f)));
}

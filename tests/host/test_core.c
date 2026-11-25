/*
 * Host tests for the generic layers: iforest, robust, and the pod engine on a
 * non-optical population. Also the test runner's main().
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pod/iforest.h"
#include "pod/pod.h"
#include "pod/robust.h"

int failures;
int test_cmis_main(void);

#define CHECK(cond, ...)                                                  \
	do {                                                              \
		if (!(cond)) {                                            \
			failures++;                                       \
			printf("FAIL %s:%d: ", __FILE__, __LINE__);       \
			printf(__VA_ARGS__);                              \
			printf("\n");                                     \
		}                                                         \
	} while (0)

static uint32_t trng = 777u;

static float gauss(void)
{
	float u1, u2;

	trng = trng * 1664525u + 1013904223u;
	u1 = ((float)(trng >> 8) + 0.5f) / 16777216.0f;
	trng = trng * 1664525u + 1013904223u;
	u2 = ((float)(trng >> 8) + 0.5f) / 16777216.0f;
	return sqrtf(-2.0f * logf(u1)) * cosf(6.2831853f * u2);
}

static unsigned argmax(const float *v, unsigned n)
{
	unsigned b = 0;

	for (unsigned i = 1; i < n; i++) {
		if (v[i] > v[b]) {
			b = i;
		}
	}
	return b;
}

/* ---- iforest ---- */

#define BIG_N 1000
#define BIG_D 12
static float xs[BIG_N * 16];
static float sc[BIG_N], sc2[BIG_N];
static uint8_t scratch[IFOREST_SCRATCH_SIZE(BIG_N, 16, 256) + 16];

static void test_iforest(void)
{
	const struct iforest_params p = { 100, 0 };
	uint32_t rng = 1;

	/* 200 points x 12 features, one outlier offset in every feature. */
	for (unsigned i = 0; i < 200; i++) {
		for (unsigned j = 0; j < BIG_D; j++) {
			xs[i * BIG_D + j] = gauss() + (i == 42 ? 3.0f : 0.0f);
		}
	}
	CHECK(iforest_score(xs, 200, BIG_D, BIG_D, &p, &rng, scratch, sizeof(scratch), sc) == 0, "score");
	CHECK(argmax(sc, 200) == 42, "outlier not top: %u", argmax(sc, 200));
	CHECK(sc[42] > 0.6f, "outlier score %.3f", (double)sc[42]);
	printf("  n=200 d=12: outlier score %.3f (median point ~0.4-0.5)\n", (double)sc[42]);

	/* Scale invariance: rescaling one feature leaves scores (nearly) unchanged. */
	for (unsigned i = 0; i < 200; i++) {
		xs[i * BIG_D + 3] *= 1000.0f;
	}
	rng = 1;
	iforest_score(xs, 200, BIG_D, BIG_D, &p, &rng, scratch, sizeof(scratch), sc2);
	float worst = 0.0f;

	for (unsigned i = 0; i < 200; i++) {
		float dlt = fabsf(sc[i] - sc2[i]);

		worst = dlt > worst ? dlt : worst;
	}
	CHECK(worst < 1e-3f, "scaling a feature changed scores by %.4f", (double)worst);

	/* n > max_samples: subsampled trees, all points still scored. */
	for (unsigned i = 0; i < BIG_N; i++) {
		for (unsigned j = 0; j < 4; j++) {
			xs[i * 4 + j] = gauss();
		}
	}
	xs[777 * 4 + 2] = 9.0f;
	CHECK(iforest_score(xs, BIG_N, 4, 4, &p, &rng, scratch, sizeof(scratch), sc) == 0, "big n");
	/* With 256-point subsamples the single-feature outlier is NOT reliably
	 * top: in the ~75% of trees whose subsample misses it, it is isolated
	 * together with the subsample's extreme point, and a 4-D Gaussian cloud
	 * of 1000 has many points that are moderately extreme in several
	 * features. Isolation Forest measures ease of isolation, not distance;
	 * pod's per-feature test exists for this. With every point in every tree
	 * (max_samples = n) the outlier is top. */
	unsigned above = 0;

	for (unsigned i = 0; i < BIG_N; i++) {
		above += sc[i] > sc[777];
	}
	printf("  n=1000 d=4, subsample 256: single-feature 9-sigma outlier ranked %u\n", above + 1);
	{
		const struct iforest_params full = { 100, BIG_N };
		static uint8_t big_scratch[IFOREST_SCRATCH_SIZE(BIG_N, 4, BIG_N)];

		CHECK(iforest_score(xs, BIG_N, 4, 4, &full, &rng, big_scratch, sizeof(big_scratch), sc) == 0,
		      "full sample");
		CHECK(argmax(sc, BIG_N) == 777, "full-sample outlier not top: %u", argmax(sc, BIG_N));
		printf("  n=1000 d=4, all points per tree: outlier ranked 1, score %.3f\n", (double)sc[777]);
	}

	/* Strided rows: score columns 0..2 of a 16-wide table. */
	for (unsigned i = 0; i < 100; i++) {
		for (unsigned j = 0; j < 16; j++) {
			xs[i * 16 + j] = gauss();
		}
		xs[i * 16 + 10] = (i == 5) ? 50.0f : xs[i * 16 + 10]; /* outlier outside the subset */
	}
	xs[60 * 16 + 1] = 8.0f;
	iforest_score(xs, 100, 3, 16, &p, &rng, scratch, sizeof(scratch), sc);
	CHECK(argmax(sc, 100) == 60, "strided: top %u", argmax(sc, 100));

	/* d = 1 and degenerate inputs. */
	for (unsigned i = 0; i < 50; i++) {
		xs[i] = (float)(i % 5);
	}
	xs[17] = 100.0f;
	iforest_score(xs, 50, 1, 1, &p, &rng, scratch, sizeof(scratch), sc);
	CHECK(argmax(sc, 50) == 17, "d=1: top %u", argmax(sc, 50));
	for (unsigned i = 0; i < 20; i++) {
		xs[i] = 1.0f;
	}
	CHECK(iforest_score(xs, 20, 1, 1, &p, &rng, scratch, sizeof(scratch), sc) == 0, "all equal");
	CHECK(fabsf(sc[0] - sc[19]) < 1e-6f, "all-equal points must score the same");
	CHECK(iforest_score(xs, 1, 1, 1, &p, &rng, scratch, sizeof(scratch), sc) == 0, "n=1");

	CHECK(iforest_score(xs, 200, 12, 11, &p, &rng, scratch, sizeof(scratch), sc) != 0, "stride < d");
	CHECK(iforest_score(xs, 200, 12, 12, &p, &rng, scratch, 10, sc) != 0, "scratch too small");
}

static void test_robust(void)
{
	float v[] = { 5, 1, 4, 2, 3 }, tmp[8];
	struct robust_stats s;

	robust_compute(v, 5, tmp, &s);
	CHECK(s.med == 3.0f && s.mad == 1.0f, "median %f mad %f", (double)s.med, (double)s.mad);
	CHECK(fabsf(robust_z(5.0f, &s) - 1.349f) < 1e-3f, "z %f", (double)robust_z(5.0f, &s));
	CHECK(fabsf(robust_normal_upper_quantile(0.025f) - 1.96f) < 2e-3f, "q(0.025)");
	CHECK(fabsf(robust_normal_upper_quantile(1e-5f) - 4.265f) < 5e-3f, "q(1e-5) %f",
	      (double)robust_normal_upper_quantile(1e-5f));
}

/* ---- pod on something that isn't an optical module ----
 * 50 cooling fans, 3 features each: rpm, motor current (A), bearing
 * temperature. All share the room; fan 33's bearing starts to wear on day 20:
 * current and temperature creep up while rpm stays put. */

#define FANS 50
#define FAN_D 3

struct fan_world {
	unsigned hour;
	float offset[FANS][FAN_D];
};

static bool fan_read(void *user, unsigned i, float *out)
{
	const struct fan_world *w = user;
	float day = (float)w->hour / 24.0f;
	float room = 22.0f + 3.0f * sinf(6.2831853f * day);
	float wear = (i == 33 && day > 20.0f) ? (day - 20.0f) / 40.0f : 0.0f;

	if (i == 12) {
		return false; /* a fan that never reports */
	}
	out[0] = 3000.0f + 50.0f * w->offset[i][0] + 5.0f * gauss();
	out[1] = 0.40f + 0.02f * w->offset[i][1] + 0.002f * gauss() + 0.05f * wear;
	out[2] = room + 8.0f + 1.0f * w->offset[i][2] + 0.2f * gauss() + 6.0f * wear;
	return true;
}

static struct fan_world world;

static void test_fans(void)
{
	static struct pod fans;
	static uint32_t heap[8192]; /* sized at runtime, not by the macro */
	size_t need = pod_mem_size(FANS, FAN_D, NULL);
	int found_hour = -1;
	unsigned false_alerts = 0;

	for (unsigned i = 0; i < FANS; i++) {
		for (unsigned j = 0; j < FAN_D; j++) {
			world.offset[i][j] = gauss();
		}
	}
	CHECK(need <= sizeof(heap), "need %zu bytes", need);
	CHECK(need == POD_MEM_SIZE(FANS, FAN_D), "macro %zu vs function %zu", (size_t)POD_MEM_SIZE(FANS, FAN_D),
	      need);
	CHECK(pod_init(&fans, NULL, FANS, FAN_D, fan_read, &world, heap, need - 1) != 0, "short memory");
	CHECK(pod_init(&fans, NULL, FANS, FAN_D, fan_read, &world, heap, need) == 0, "init");

	for (world.hour = 0; world.hour < 60u * 24u; world.hour++) {
		pod_update(&fans);
		for (unsigned i = 0; i < FANS; i++) {
			const struct pod_result *r = pod_result(&fans, i);

			if (!(r->flags & POD_FLAG_ALERT_CHANGING)) {
				continue;
			}
			if (i == 33 && found_hour < 0) {
				found_hour = (int)world.hour;
				printf("  fan 33 bearing wear flagged on day %.1f (wear began day 20), driven by "
				       "feature %u (z %.1f)\n",
				       world.hour / 24.0, r->top_feature[POD_VIEW_CHANGING],
				       (double)r->top_feature_z[POD_VIEW_CHANGING]);
			} else if (i != 33) {
				false_alerts++;
			}
		}
	}
	CHECK(found_hour > 20 * 24 && found_hour < 30 * 24, "fan 33 found at hour %d", found_hour);
	CHECK(false_alerts == 0, "%u false fan alerts", false_alerts);
	CHECK(!pod_result(&fans, 12)->ready, "silent fan is left out");
	CHECK(pod_level(&fans, 0, 0) > 2800.0f, "level accessor");
}

int main(void)
{
	printf("iforest\n");
	test_iforest();
	printf("robust\n");
	test_robust();
	printf("generic engine: fan fleet\n");
	test_fans();
	printf("--- CMIS adapter ---\n");
	test_cmis_main();

	printf(failures ? "\n%d FAILED\n" : "\nall passed\n", failures);
	return failures ? 1 : 0;
}

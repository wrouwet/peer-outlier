/*
 * Minimal pod example, runs on any host.
 *
 * 40 pumps, each reporting 4 features once per (simulated) hour: flow,
 * outlet pressure, motor current and vibration. They share the same daily
 * demand cycle. On day 15, pump 27's impeller starts to wear: flow and
 * pressure sag slowly, current and vibration creep up. pod is told nothing
 * about pumps, units or limits; it only sees 40 rows of 4 numbers.
 *
 *   cmake -B build && cmake --build build && ./build/pod_example_basic
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <math.h>
#include <stdio.h>

#include "pod/pod.h"

#define N_PUMPS    40
#define N_FEATURES 4

static const char *const feature_names[N_FEATURES] = { "flow", "pressure", "current", "vibration" };

/* ---- a tiny simulated plant ---- */

struct plant {
	unsigned hour;
	float offset[N_PUMPS][N_FEATURES]; /* each pump's own, fixed character */
	uint32_t rng;
};

static float noise(uint32_t *s)
{
	float u1, u2;

	*s = *s * 1664525u + 1013904223u;
	u1 = ((float)(*s >> 8) + 0.5f) / 16777216.0f;
	*s = *s * 1664525u + 1013904223u;
	u2 = ((float)(*s >> 8) + 0.5f) / 16777216.0f;
	return sqrtf(-2.0f * logf(u1)) * cosf(6.2831853f * u2);
}

/* The read callback: fill out[] with pump i's current features. Here the
 * data is computed; on a device it would come from memory, registers or a
 * bus transaction. Return false if the pump has nothing valid to report. */
static bool read_pump(void *user, unsigned i, float *out)
{
	struct plant *p = user;
	float day = (float)p->hour / 24.0f;
	float demand = 1.0f + 0.2f * sinf(6.2831853f * day);
	float wear = (i == 27 && day > 15.0f) ? (day - 15.0f) / 60.0f : 0.0f;

	out[0] = 100.0f * demand * (1.0f + 0.03f * p->offset[i][0]) * (1.0f - 0.15f * wear) +
		 0.5f * noise(&p->rng);
	out[1] = 4.0f * demand * (1.0f + 0.02f * p->offset[i][1]) * (1.0f - 0.10f * wear) +
		 0.02f * noise(&p->rng);
	out[2] = 12.0f * demand * (1.0f + 0.03f * p->offset[i][2]) * (1.0f + 0.20f * wear) +
		 0.05f * noise(&p->rng);
	out[3] = 1.5f * (1.0f + 0.10f * p->offset[i][3]) * (1.0f + 1.5f * wear) + 0.03f * noise(&p->rng);
	return true;
}

/* ---- using pod ---- */

POD_MEM_DEFINE(pod_mem, N_PUMPS, N_FEATURES);
static struct pod pod;
static struct plant plant = { .rng = 42 };

int main(void)
{
	uint8_t last[N_PUMPS] = { 0 };

	for (unsigned i = 0; i < N_PUMPS; i++) {
		for (unsigned j = 0; j < N_FEATURES; j++) {
			plant.offset[i][j] = noise(&plant.rng);
		}
	}

	/* Init: defaults suit one update per hour. */
	if (pod_init(&pod, NULL, N_PUMPS, N_FEATURES, read_pump, &plant, pod_mem, sizeof(pod_mem)) != 0) {
		fprintf(stderr, "pod_init failed\n");
		return 1;
	}
	printf("pod %d.%d.%d: %d pumps x %d features, %zu bytes of state\n", POD_VERSION_MAJOR,
	       POD_VERSION_MINOR, POD_VERSION_PATCH, N_PUMPS, N_FEATURES, sizeof(pod_mem));

	/* Runtime: one pod_update() per hour for 60 days. */
	for (plant.hour = 0; plant.hour < 60u * 24u; plant.hour++) {
		pod_update(&pod);

		for (unsigned i = 0; i < N_PUMPS; i++) {
			const struct pod_result *r = pod_result(&pod, i);
			uint8_t alerts = r->flags & (POD_FLAG_ALERT_DIFFERENT | POD_FLAG_ALERT_CHANGING);

			for (unsigned v = 0; v < POD_VIEW_COUNT; v++) {
				uint8_t bit = (uint8_t)POD_FLAG_ALERT(v);

				if ((alerts ^ last[i]) & bit) {
					printf("day %5.1f  pump %2u  %s %-9s  %s, robust z %+.1f\n",
					       plant.hour / 24.0, i, (alerts & bit) ? "ALERT" : "clear",
					       pod_view_name(v), feature_names[r->top_feature[v]],
					       (double)r->top_feature_z[v]);
				}
			}
			last[i] = alerts;
		}
	}
	return 0;
}

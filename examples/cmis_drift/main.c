/*
 * Drift demo: one aging optical module among 32, caught by pod.
 *
 * Simulates a year of 32 CMIS modules (sim/fleet_sim.c): daily and seasonal
 * temperature swings, per-module and per-lane spread, measurement noise, a
 * healthy module on a long fibre (7), a 3-hour Rx glitch (module 3, day 150)
 * and a 2-day cooling failure that heats every module (day 200). From day
 * 120, module 19's laser starts to age: its bias current creeps up, slowly
 * at first.
 *
 * pod runs once per simulated hour, reading the 256-byte CMIS records
 * straight from memory. The program writes:
 *
 *   <dir>/timeseries.csv  every 6 h: each module's Tx bias, and the robust z
 *                         of its Tx bias change among its peers (the quantity
 *                         pod's per-feature test thresholds), plus the cutoff
 *   <dir>/alerts.csv      every alert set/clear, at hourly resolution
 *
 * plot.py turns these into a picture.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "fleet_sim.h"
#include "pod/pod.h"
#include "pod/robust.h"
#include "pod_cmis/pod_cmis.h"

#define N     32
#define HOURS (365u * 24u)
#define EVERY 6u

static struct cmis_record records[N];
static struct pod_cmis_source src = { .records = records };
POD_MEM_DEFINE(mem, N, POD_CMIS_N_FEATURES);
static struct pod pod;
static struct fleet_sim sim;

int main(int argc, char **argv)
{
	const char *dir = argc > 1 ? argv[1] : ".";
	char path[512];
	FILE *ts, *al;
	struct fleet_sim_cfg sc;
	uint8_t last[N] = { 0 };
	float chg[N], tmp[N], z[N];
	unsigned target;

	fleet_sim_default(&sc);
	fleet_sim_init(&sim, &sc);
	target = sc.aging_module;
	if (pod_init(&pod, NULL, N, POD_CMIS_N_FEATURES, pod_cmis_read, &src, mem, sizeof(mem)) != 0) {
		fprintf(stderr, "pod_init failed\n");
		return 1;
	}

	snprintf(path, sizeof(path), "%s/timeseries.csv", dir);
	ts = fopen(path, "w");
	snprintf(path, sizeof(path), "%s/alerts.csv", dir);
	al = fopen(path, "w");
	if (!ts || !al) {
		fprintf(stderr, "cannot write to %s\n", dir);
		return 1;
	}

	/* The per-feature cutoff pod applies: Bonferroni over n x d values. */
	float cutoff = robust_normal_upper_quantile(0.01f / (2.0f * N * POD_CMIS_N_FEATURES));

	if (cutoff < 3.5f) {
		cutoff = 3.5f;
	}

	fprintf(ts, "hour,day,degradation,cutoff");
	for (unsigned m = 0; m < N; m++) {
		fprintf(ts, ",bias_%u", m);
	}
	for (unsigned m = 0; m < N; m++) {
		fprintf(ts, ",z_%u", m);
	}
	fprintf(ts, "\n");
	fprintf(al, "hour,day,module,view,state,top_feature,top_feature_z,degradation\n");

	for (unsigned h = 0; h < HOURS; h++) {
		fleet_sim_step(&sim, h, records);
		pod_update(&pod);

		/* Alert transitions, every hour. */
		for (unsigned m = 0; m < N; m++) {
			const struct pod_result *r = pod_result(&pod, m);
			uint8_t a = r->flags & (POD_FLAG_ALERT_DIFFERENT | POD_FLAG_ALERT_CHANGING);

			for (unsigned v = 0; v < POD_VIEW_COUNT; v++) {
				uint8_t bit = (uint8_t)POD_FLAG_ALERT(v);

				if ((a ^ last[m]) & bit) {
					fprintf(al, "%u,%.3f,%u,%s,%s,%s,%.2f,%.4f\n", h, h / 24.0, m,
						pod_view_name(v), (a & bit) ? "set" : "clear",
						pod_cmis_feature_name(r->top_feature[v]),
						(double)r->top_feature_z[v],
						(double)fleet_sim_degradation(&sim, m, h));
				}
			}
			last[m] = a;
		}

		if (h % EVERY) {
			continue;
		}

		/* Robust z of each module's Tx bias change among its peers. */
		for (unsigned m = 0; m < N; m++) {
			chg[m] = pod_change(&pod, m, POD_CMIS_TX_BIAS_MA);
		}
		struct robust_stats s;

		robust_compute(chg, N, tmp, &s);
		for (unsigned m = 0; m < N; m++) {
			z[m] = robust_z(chg[m], &s);
		}

		fprintf(ts, "%u,%.3f,%.4f,%.2f", h, h / 24.0, (double)fleet_sim_degradation(&sim, target, h),
			(double)cutoff);
		for (unsigned m = 0; m < N; m++) {
			float f[POD_CMIS_N_FEATURES];

			pod_cmis_extract(&records[m], f); /* the raw reading, as measured */
			fprintf(ts, ",%.3f", (double)f[POD_CMIS_TX_BIAS_MA]);
		}
		for (unsigned m = 0; m < N; m++) {
			fprintf(ts, ",%.2f", h < 24u ? 0.0 : (double)z[m]);
		}
		fprintf(ts, "\n");
	}
	fclose(ts);
	fclose(al);
	printf("wrote %s/timeseries.csv and %s/alerts.csv (aging module: %u, from day %u)\n", dir, dir,
	       target, sc.aging_start_day);
	return 0;
}

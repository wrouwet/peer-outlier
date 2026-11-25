/*
 * Host tests: CMIS decoding, detector behaviour, and a simulated year.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "pod/pod.h"
#include "pod_cmis/pod_cmis.h"
#include "fleet_sim.h"

extern int failures;

#define CHECK(cond, ...)                                                  \
	do {                                                              \
		if (!(cond)) {                                            \
			failures++;                                       \
			printf("FAIL %s:%d: ", __FILE__, __LINE__);       \
			printf(__VA_ARGS__);                              \
			printf("\n");                                     \
		}                                                         \
	} while (0)

#define N 32
static struct cmis_record records[N];
static struct pod cod;
POD_MEM_DEFINE(mem, N, POD_CMIS_N_FEATURES);
static struct pod_cmis_source src = { .records = records };

static void test_decode(void)
{
	struct cmis_record r;
	float f[POD_CMIS_N_FEATURES];

	memset(&r, 0, sizeof(r));
	r.lower[CMIS_ADDR_MODULE_STATE] = CMIS_MODULE_READY << 1;
	r.lower[CMIS_ADDR_TEMPERATURE] = 0xfe; /* -1.5 degC = -384 */
	r.lower[CMIS_ADDR_TEMPERATURE + 1] = 0x80;
	r.lower[CMIS_ADDR_VCC] = 0x80;         /* 33000 x 100 uV = 3.3 V */
	r.lower[CMIS_ADDR_VCC + 1] = 0xe8;
	r.page11h[CMIS_ADDR_DP_STATE - 128] = 0x14; /* lane 1 activated, lane 2 deactivated */
	/* lane 1: Tx 1 mW (10000), bias 40 mA (20000), Rx 0.5 mW (5000) */
	r.page11h[CMIS_ADDR_TX_POWER - 128] = 0x27;
	r.page11h[CMIS_ADDR_TX_POWER - 127] = 0x10;
	r.page11h[CMIS_ADDR_TX_BIAS - 128] = 0x4e;
	r.page11h[CMIS_ADDR_TX_BIAS - 127] = 0x20;
	r.page11h[CMIS_ADDR_RX_POWER - 128] = 0x13;
	r.page11h[CMIS_ADDR_RX_POWER - 127] = 0x88;
	/* lane 2 is not active: its values must be ignored */
	r.page11h[CMIS_ADDR_RX_POWER - 126] = 0x00;
	r.page11h[CMIS_ADDR_RX_POWER - 125] = 0x01;

	CHECK(cmis_module_state(&r) == CMIS_MODULE_READY, "module state");
	CHECK(fabsf(cmis_temperature_c(&r) + 1.5f) < 1e-6f, "temp %f", (double)cmis_temperature_c(&r));
	CHECK(fabsf(cmis_vcc_v(&r) - 3.3f) < 1e-5f, "vcc %f", (double)cmis_vcc_v(&r));
	CHECK(cmis_dp_state(&r, 0) == CMIS_DP_ACTIVATED, "lane 1 dp state");
	CHECK(cmis_dp_state(&r, 1) == CMIS_DP_DEACTIVATED, "lane 2 dp state");
	CHECK(cmis_byte(&r, CMIS_ADDR_TX_BIAS) == 0x4e, "page 11h addressing");

	CHECK(pod_cmis_extract(&r, f), "extract");
	CHECK(fabsf(f[POD_CMIS_TX_POWER_DBM] - 0.0f) < 1e-3f, "tx dBm %f", (double)f[POD_CMIS_TX_POWER_DBM]);
	CHECK(fabsf(f[POD_CMIS_RX_POWER_DBM] + 3.0103f) < 1e-3f, "rx dBm %f", (double)f[POD_CMIS_RX_POWER_DBM]);
	CHECK(fabsf(f[POD_CMIS_TX_BIAS_MA] - 40.0f) < 1e-3f, "bias %f", (double)f[POD_CMIS_TX_BIAS_MA]);
	CHECK(f[POD_CMIS_RX_SPREAD_DB] == 0.0f, "one lane: no spread");

	r.lower[CMIS_ADDR_MODULE_STATE] = CMIS_MODULE_LOW_PWR << 1;
	CHECK(!pod_cmis_extract(&r, f), "LowPwr module must not be used");
	r.lower[CMIS_ADDR_MODULE_STATE] = CMIS_MODULE_READY << 1;
	r.page11h[CMIS_ADDR_DP_STATE - 128] = 0x11;
	CHECK(!pod_cmis_extract(&r, f), "no active lane must not be used");
}

static void test_init_args(void)
{
	struct pod_config c;

	CHECK(pod_init(&cod, NULL, 0, POD_CMIS_N_FEATURES, pod_cmis_read, &src, mem, sizeof(mem)) != 0, "n=0");
	CHECK(pod_init(&cod, NULL, N + 1, POD_CMIS_N_FEATURES, pod_cmis_read, &src, mem, sizeof(mem)) != 0,
	      "memory too small for n+1");
	CHECK(pod_mem_size(N, POD_CMIS_N_FEATURES, NULL) <= sizeof(mem), "POD_MEM_SIZE covers pod_mem_size");
	pod_config_default(&c);
	c.persist_count = (uint8_t)(c.persist_window + 1);
	CHECK(pod_init(&cod, &c, N, POD_CMIS_N_FEATURES, pod_cmis_read, &src, mem, sizeof(mem)) != 0, "persist_count > window");
	CHECK(pod_init(&cod, NULL, N, POD_CMIS_N_FEATURES, pod_cmis_read, &src, mem, sizeof(mem)) == 0, "defaults");
}

/* A single snapshot: the long-fibre module is the odd one out, at once,
 * with no history and no training. */
static void test_snapshot(void)
{
	struct fleet_sim_cfg sc;
	struct fleet_sim sim;
	const struct pod_result *r;

	fleet_sim_default(&sc);
	sc.aging_module = FLEET_SIM_NONE;
	fleet_sim_init(&sim, &sc);
	fleet_sim_step(&sim, 0, records);
	pod_init(&cod, NULL, N, POD_CMIS_N_FEATURES, pod_cmis_read, &src, mem, sizeof(mem));
	pod_update(&cod);

	unsigned others = 0;

	for (unsigned i = 0; i < N; i++) {
		if (i != sc.long_fibre_module && (pod_result(&cod, i)->flags & POD_FLAG_CAND_DIFFERENT)) {
			others++;
		}
	}
	r = pod_result(&cod, sc.long_fibre_module);
	CHECK(r->flags & POD_FLAG_CAND_DIFFERENT, "long fibre should be a candidate");
	CHECK(others == 0, "%u other modules flagged", others);
	CHECK(r->top_feature[POD_VIEW_DIFFERENT] == POD_CMIS_RX_POWER_DBM, "top feature %s",
	      pod_cmis_feature_name(r->top_feature[POD_VIEW_DIFFERENT]));
	CHECK(!r->scored[POD_VIEW_CHANGING], "no CHANGING view before warm-up");
	printf("  snapshot: module %u stands out on %s (robust z %.1f), no other module flagged\n",
	       sc.long_fibre_module, pod_cmis_feature_name(r->top_feature[0]), (double)r->top_feature_z[0]);
}

/* Modules that are not Ready, and groups that are too small, are skipped. */
static void test_exclusion(void)
{
	struct fleet_sim_cfg sc;
	struct fleet_sim sim;

	fleet_sim_default(&sc);
	fleet_sim_init(&sim, &sc);
	fleet_sim_step(&sim, 0, records);
	records[5].lower[CMIS_ADDR_MODULE_STATE] = CMIS_MODULE_FAULT << 1;
	pod_init(&cod, NULL, N, POD_CMIS_N_FEATURES, pod_cmis_read, &src, mem, sizeof(mem));
	pod_set_enabled(&cod, 6, false);
	for (unsigned i = 20; i < N; i++) {
		pod_set_group(&cod, i, 1); /* group 1 has 12 modules */
	}
	pod_set_group(&cod, 0, 2); /* a group of one */
	pod_update(&cod);
	CHECK(!pod_result(&cod, 5)->ready, "faulted module must be skipped");
	CHECK(!pod_result(&cod, 6)->scored[0], "disabled module must be skipped");
	CHECK(!pod_result(&cod, 0)->scored[0], "group of one cannot be compared");
	CHECK(pod_result(&cod, 25)->scored[0], "group 1 is compared");
	CHECK(pod_result(&cod, 10)->scored[0], "group 0 is compared");
}

struct year_stats {
	int aging_first_alert_hour[POD_VIEW_COUNT];
	unsigned false_alert_hours[POD_VIEW_COUNT];
	unsigned long_fibre_different_hours;
	unsigned long_fibre_changing_hours;
	unsigned glitch_alert_hours;
	unsigned hvac_alert_hours;
	double seconds;
};

static void run_year(const struct fleet_sim_cfg *simcfg, unsigned target, struct year_stats *st, bool verbose)
{
	struct fleet_sim_cfg sc = *simcfg;
	struct fleet_sim sim;
	clock_t t0;

	fleet_sim_init(&sim, &sc);
	pod_init(&cod, NULL, N, POD_CMIS_N_FEATURES, pod_cmis_read, &src, mem, sizeof(mem));
	memset(st, 0, sizeof(*st));
	st->aging_first_alert_hour[0] = st->aging_first_alert_hour[1] = -1;

	t0 = clock();
	for (unsigned h = 0; h < 365u * 24u; h++) {
		fleet_sim_step(&sim, h, records);
		pod_update(&cod);

		for (unsigned i = 0; i < N; i++) {
			const struct pod_result *r = pod_result(&cod, i);

			for (unsigned v = 0; v < POD_VIEW_COUNT; v++) {
				bool alert = r->flags & POD_FLAG_ALERT(v);

				if (!alert) {
					continue;
				}
				if (i == target) {
					if (st->aging_first_alert_hour[v] < 0) {
						st->aging_first_alert_hour[v] = (int)h;
						if (verbose) {
							printf("  day %5.1f: module %u ALERT %s "
							       "(degradation %.0f%%, driven by %s, z %.1f)\n",
							       h / 24.0, i, pod_view_name(v),
							       100.0 * (double)fleet_sim_degradation(&sim, i, h),
							       pod_cmis_feature_name(r->top_feature[v]),
							       (double)r->top_feature_z[v]);
						}
					}
				} else if (i == sc.long_fibre_module) {
					if (v == POD_VIEW_DIFFERENT) {
						st->long_fibre_different_hours++;
					} else {
						st->long_fibre_changing_hours++;
					}
				} else {
					st->false_alert_hours[v]++;
					if (i == sc.glitch_module) {
						st->glitch_alert_hours++;
					}
					if (verbose && st->false_alert_hours[v] <= 5) {
						printf("  day %5.1f: module %u false ALERT %s (%s z %.1f)\n",
						       h / 24.0, i, pod_view_name(v),
						       pod_cmis_feature_name(r->top_feature[v]),
						       (double)r->top_feature_z[v]);
					}
				}
				if (h / 24u >= sc.hvac_day && h / 24u < sc.hvac_day + 3u && i != target &&
				    i != sc.long_fibre_module) {
					st->hvac_alert_hours++;
				}
			}
		}
	}
	st->seconds = (double)(clock() - t0) / CLOCKS_PER_SEC;
}

static void check_year(const struct fleet_sim_cfg *sc, unsigned target, unsigned start_day,
		       const char *what)
{
	struct year_stats st;

	run_year(sc, target, &st, true);

	int first = st.aging_first_alert_hour[POD_VIEW_CHANGING];
	float days = first < 0 ? -1.0f : (float)first / 24.0f - (float)start_day;

	printf("  %s detected %.1f days into its degradation (of %u)\n", what, (double)days,
	       365 - start_day);
	printf("  false alert module-hours: different %u, changing %u (of %u)\n",
	       st.false_alert_hours[0], st.false_alert_hours[1], (N - 2) * 365 * 24);
	printf("  long fibre: DIFFERENT %u h, CHANGING %u h\n", st.long_fibre_different_hours,
	       st.long_fibre_changing_hours);
	printf("  %.0f us per pod_update() on this host\n", st.seconds * 1e6 / (365.0 * 24.0));

	CHECK(first >= 0, "%s never flagged as CHANGING", what);
	CHECK(days >= 0.0f && days < 60.0f, "%s found late: %.1f days", what, (double)days);
	CHECK(st.false_alert_hours[POD_VIEW_CHANGING] == 0, "false CHANGING alerts: %u",
	      st.false_alert_hours[POD_VIEW_CHANGING]);
	CHECK(st.false_alert_hours[POD_VIEW_DIFFERENT] == 0, "false DIFFERENT alerts: %u",
	      st.false_alert_hours[POD_VIEW_DIFFERENT]);
	CHECK(st.long_fibre_different_hours > 300u * 24u, "long fibre DIFFERENT only %u h",
	      st.long_fibre_different_hours);
	CHECK(st.long_fibre_changing_hours == 0, "long fibre is not changing, flagged %u h",
	      st.long_fibre_changing_hours);
	CHECK(st.glitch_alert_hours == 0, "3-hour glitch raised an alert");
	CHECK(st.hvac_alert_hours == 0, "cooling failure (common mode) raised alerts");
}

static void test_year(void)
{
	struct fleet_sim_cfg sc;

	fleet_sim_default(&sc);
	printf("  laser aging: %u modules, module %u ages from day %u, module %u has a long fibre,\n"
	       "  module %u glitches on day %u, cooling fails on day %u\n",
	       N, sc.aging_module, sc.aging_start_day, sc.long_fibre_module, sc.glitch_module,
	       sc.glitch_day, sc.hvac_day);
	check_year(&sc, sc.aging_module, sc.aging_start_day, "laser aging");

	fleet_sim_default(&sc);
	sc.aging_module = FLEET_SIM_NONE;
	sc.fibre_module = 25;
	printf("  fibre degradation: module %u loses 3 dB of Rx power from day %u to day 365\n",
	       sc.fibre_module, sc.fibre_start_day);
	check_year(&sc, sc.fibre_module, sc.fibre_start_day, "fibre degradation");
}

/* Swapping a module: its history is forgotten, it rejoins after warm-up. */
static void test_swap(void)
{
	struct fleet_sim_cfg sc;
	struct fleet_sim sim;

	fleet_sim_default(&sc);
	sc.aging_module = FLEET_SIM_NONE;
	fleet_sim_init(&sim, &sc);
	pod_init(&cod, NULL, N, POD_CMIS_N_FEATURES, pod_cmis_read, &src, mem, sizeof(mem));
	for (unsigned h = 0; h < 48; h++) {
		fleet_sim_step(&sim, h, records);
		pod_update(&cod);
	}
	CHECK(pod_result(&cod, 9)->scored[POD_VIEW_CHANGING], "in CHANGING view after warm-up");
	pod_reset_entity(&cod, 9);
	fleet_sim_step(&sim, 48, records);
	pod_update(&cod);
	CHECK(!pod_result(&cod, 9)->scored[POD_VIEW_CHANGING], "swapped module waits for warm-up");
	CHECK(pod_result(&cod, 9)->scored[POD_VIEW_DIFFERENT], "swapped module still compared on levels");
}

int test_cmis_main(void);
int test_cmis_main(void)
{
	printf("decode\n");
	test_decode();
	printf("init\n");
	test_init_args();
	printf("snapshot\n");
	test_snapshot();
	printf("exclusion\n");
	test_exclusion();
	printf("swap\n");
	test_swap();
	printf("year\n");
	test_year();

	return failures;
}

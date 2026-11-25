/*
 * Runs the outlier detector on a simulated fleet of 32 modules, one
 * pod_update() per simulated hour, and prints every alert change plus the
 * time each pod_update() takes on this MCU.
 *
 * The records array is exactly what an I2C poller would keep in SRAM: lower
 * memory and page 11h of each module, as read.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include "pod/pod.h"
#include "pod_cmis/pod_cmis.h"
#include "fleet_sim.h"

#define N_MODULES 32
#define DAYS CONFIG_FLEET_SIM_DAYS

static struct cmis_record records[N_MODULES];
static struct pod_cmis_source src = { .records = records };
POD_MEM_DEFINE(mem, N_MODULES, POD_CMIS_N_FEATURES);
static struct pod cod;
static struct fleet_sim sim;

int main(void)
{
	struct fleet_sim_cfg sc;
	uint8_t last_flags[N_MODULES] = { 0 };
	uint64_t total_cycles = 0;
	uint32_t worst_cycles = 0;

	fleet_sim_default(&sc);
	fleet_sim_init(&sim, &sc);
	if (pod_init(&cod, NULL, N_MODULES, POD_CMIS_N_FEATURES, pod_cmis_read, &src, mem,
		     sizeof(mem)) != 0) {
		printk("pod_init failed\n");
		return 0;
	}

	printk("pod: %d modules, state %u + %u bytes, records %u bytes, FPU %s\n", N_MODULES,
	       (unsigned)sizeof(cod), (unsigned)sizeof(mem), (unsigned)sizeof(records),
	       IS_ENABLED(CONFIG_FPU) ? "on" : "off (soft float)");
	printk("module %u ages from day %u, module %u has a long fibre, module %u glitches on day %u,"
	       " cooling fails on day %u\n",
	       sc.aging_module, sc.aging_start_day, sc.long_fibre_module, sc.glitch_module,
	       sc.glitch_day, sc.hvac_day);

	for (unsigned h = 0; h < DAYS * 24u; h++) {
		fleet_sim_step(&sim, h, records);

		uint32_t t0 = k_cycle_get_32();

		pod_update(&cod);

		uint32_t dt = k_cycle_get_32() - t0;

		total_cycles += dt;
		worst_cycles = dt > worst_cycles ? dt : worst_cycles;

		for (unsigned i = 0; i < N_MODULES; i++) {
			const struct pod_result *r = pod_result(&cod, i);
			uint8_t alerts = r->flags & (POD_FLAG_ALERT_DIFFERENT | POD_FLAG_ALERT_CHANGING);

			if (alerts == last_flags[i]) {
				continue;
			}
			for (unsigned v = 0; v < POD_VIEW_COUNT; v++) {
				uint8_t bit = POD_FLAG_ALERT(v);

				if ((alerts ^ last_flags[i]) & bit) {
					printk("day %3u h%02u  module %2u  %s %-9s  %s (z %d.%d)\n", h / 24u,
					       h % 24u, i, (alerts & bit) ? "ALERT" : "clear",
					       pod_view_name(v), pod_cmis_feature_name(r->top_feature[v]),
					       (int)r->top_feature_z[v],
					       (int)(10.0f * (r->top_feature_z[v] < 0 ? -r->top_feature_z[v]
										      : r->top_feature_z[v])) % 10);
				}
			}
			last_flags[i] = alerts;
		}
		if (h % ((DAYS > 60 ? 30u : 1u) * 24u) == 0) {
			printk("day %3u: %u us per pod_update() so far (worst %u us)\n", h / 24u,
			       (unsigned)k_cyc_to_us_floor64(total_cycles / (h + 1u)),
			       (unsigned)k_cyc_to_us_floor32(worst_cycles));
		}
	}
	printk("done: %u us mean, %u us worst per pod_update()\n",
	       (unsigned)k_cyc_to_us_floor64(total_cycles / (DAYS * 24u)),
	       (unsigned)k_cyc_to_us_floor32(worst_cycles));

#ifdef CONFIG_FLEET_SIM_TREE_SWEEP
	/* Both views active (after warm-up), 32 modules in one group. */
	static const uint16_t trees[] = { 25, 50, 100, 200 };

	for (unsigned t = 0; t < ARRAY_SIZE(trees); t++) {
		struct pod_config cfg;
		uint64_t sum = 0;
		const unsigned warm = 30, timed = 24;

		pod_config_default(&cfg);
		cfg.n_trees = trees[t];
		pod_init(&cod, &cfg, N_MODULES, POD_CMIS_N_FEATURES, pod_cmis_read, &src, mem, sizeof(mem));
		for (unsigned h = 0; h < warm + timed; h++) {
			fleet_sim_step(&sim, h, records);
			uint32_t t0 = k_cycle_get_32();

			pod_update(&cod);
			if (h >= warm) {
				sum += k_cycle_get_32() - t0;
			}
		}
		printk("sweep: %3u trees: %u us per pod_update()\n", trees[t],
		       (unsigned)k_cyc_to_us_floor64(sum / timed));
	}
#endif
	printk("benchmark finished\n");
	return 0;
}

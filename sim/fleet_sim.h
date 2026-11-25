/*
 * Simulated fleet of CMIS optical modules, for testing the detector without
 * hardware. Writes struct cmis_record bytes exactly as a poller would.
 *
 * Model, one step per simulated hour:
 *  - room temperature with a daily and a seasonal cycle, plus an optional
 *    two-day cooling failure that heats every module (common mode);
 *  - every module and lane gets its own fixed offsets (slot airflow, laser
 *    spread, fibre loss), plus measurement noise;
 *  - laser bias follows temperature, as an APC loop does;
 *  - one module can have a long fibre (permanently lower Rx power);
 *  - one module can age: bias, temperature and finally Tx power drift;
 *  - one module's fibre can degrade: only its Rx power drifts down;
 *  - one module can have a short Rx glitch.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef FLEET_SIM_H
#define FLEET_SIM_H

#include <stdint.h>

#include "pod_cmis/cmis_record.h"

#define FLEET_SIM_NONE 0xffffu

struct fleet_sim_cfg {
	unsigned n_modules;
	uint32_t seed;
	unsigned aging_module;    /* FLEET_SIM_NONE to disable */
	unsigned aging_start_day; /* degradation runs from here to day 365 */
	unsigned long_fibre_module;
	unsigned fibre_module;    /* fibre degrades: Rx loss grows 3 dB from fibre_start_day to day 365 */
	unsigned fibre_start_day;
	unsigned glitch_module;
	unsigned glitch_day;      /* 3-hour, 6 dB Rx drop */
	unsigned hvac_day;        /* 0 = no cooling failure */
};

struct fleet_sim {
	struct fleet_sim_cfg cfg;
	uint32_t rng;
};

void fleet_sim_default(struct fleet_sim_cfg *cfg);
void fleet_sim_init(struct fleet_sim *s, const struct fleet_sim_cfg *cfg);

/* Writes every module's record for the given simulated hour. */
void fleet_sim_step(struct fleet_sim *s, unsigned hour, struct cmis_record *records);

/* Aging progress of a module at a given hour, 0..1. */
float fleet_sim_degradation(const struct fleet_sim *s, unsigned module, unsigned hour);

#endif /* FLEET_SIM_H */

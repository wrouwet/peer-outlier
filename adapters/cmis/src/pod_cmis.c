/*
 * SPDX-License-Identifier: Apache-2.0
 */
#include "pod_cmis/pod_cmis.h"

#include <math.h>

static const char *const names[POD_CMIS_N_FEATURES] = {
	[POD_CMIS_TEMP_C] = "temperature",
	[POD_CMIS_VCC_V] = "vcc",
	[POD_CMIS_TX_BIAS_MA] = "tx_bias",
	[POD_CMIS_TX_POWER_DBM] = "tx_power",
	[POD_CMIS_RX_POWER_DBM] = "rx_power",
	[POD_CMIS_RX_SPREAD_DB] = "rx_lane_spread",
	[POD_CMIS_TX_EFFICIENCY] = "tx_efficiency",
};

const char *pod_cmis_feature_name(unsigned feature)
{
	return feature < POD_CMIS_N_FEATURES ? names[feature] : "?";
}

static float raw_power_to_dbm(uint16_t raw)
{
	/* 0.1 uW units; floor at 0.1 uW (-40 dBm) so a dark lane stays finite. */
	return 10.0f * log10f((float)(raw ? raw : 1u)) - 40.0f;
}

bool pod_cmis_extract(const struct cmis_record *rec, float out[POD_CMIS_N_FEATURES])
{
	unsigned lanes = 0;
	float tx_sum = 0.0f, rx_sum = 0.0f, bias_sum = 0.0f;
	float rx_min = 0.0f, rx_max = 0.0f;

	if (cmis_module_state(rec) != CMIS_MODULE_READY) {
		return false;
	}
	for (unsigned l = 0; l < CMIS_MAX_LANES; l++) {
		float rx;

		if (cmis_dp_state(rec, l) != CMIS_DP_ACTIVATED) {
			continue;
		}
		rx = raw_power_to_dbm(cmis_rx_power_raw(rec, l));
		tx_sum += raw_power_to_dbm(cmis_tx_power_raw(rec, l));
		bias_sum += (float)cmis_tx_bias_raw(rec, l) * 0.002f;
		rx_sum += rx;
		if (lanes == 0 || rx < rx_min) {
			rx_min = rx;
		}
		if (lanes == 0 || rx > rx_max) {
			rx_max = rx;
		}
		lanes++;
	}
	if (lanes == 0) {
		return false;
	}

	float bias = bias_sum / (float)lanes;

	out[POD_CMIS_TEMP_C] = cmis_temperature_c(rec);
	out[POD_CMIS_VCC_V] = cmis_vcc_v(rec);
	out[POD_CMIS_TX_BIAS_MA] = bias;
	out[POD_CMIS_TX_POWER_DBM] = tx_sum / (float)lanes;
	out[POD_CMIS_RX_POWER_DBM] = rx_sum / (float)lanes;
	out[POD_CMIS_RX_SPREAD_DB] = rx_max - rx_min;
	out[POD_CMIS_TX_EFFICIENCY] = out[POD_CMIS_TX_POWER_DBM] - 10.0f * log10f(bias > 0.002f ? bias : 0.002f);
	return true;
}

bool pod_cmis_read(void *user, unsigned entity, float *out)
{
	const struct pod_cmis_source *s = user;
	bool ok;

	if (s->lock) {
		s->lock(s->user, entity);
	}
	ok = pod_cmis_extract(&s->records[entity], out);
	if (s->unlock) {
		s->unlock(s->user, entity);
	}
	return ok;
}

/*
 * CMIS adapter for pod: turns cached CMIS records (lower memory + page 11h,
 * stored as read) into pod features for optical modules.
 *
 *   static struct cmis_record records[32];          // filled by your I2C poller
 *   static struct pod_cmis_source src = { .records = records };
 *   POD_MEM_DEFINE(mem, 32, POD_CMIS_N_FEATURES);
 *   static struct pod pod;
 *
 *   pod_init(&pod, NULL, 32, POD_CMIS_N_FEATURES, pod_cmis_read, &src, mem, sizeof(mem));
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef POD_CMIS_POD_CMIS_H
#define POD_CMIS_POD_CMIS_H

#include <stdbool.h>

#include "pod_cmis/cmis_record.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Features, computed over the lanes in DPActivated state. */
enum pod_cmis_feature {
	POD_CMIS_TEMP_C,        /* module temperature, degC */
	POD_CMIS_VCC_V,         /* supply voltage, V */
	POD_CMIS_TX_BIAS_MA,    /* mean laser bias, mA (before the page 01h multiplier) */
	POD_CMIS_TX_POWER_DBM,  /* mean Tx optical power, dBm */
	POD_CMIS_RX_POWER_DBM,  /* mean Rx optical power, dBm */
	POD_CMIS_RX_SPREAD_DB,  /* strongest minus weakest Rx lane, dB */
	POD_CMIS_TX_EFFICIENCY, /* Tx dBm minus 10*log10(bias mA): falls as a laser ages */
	POD_CMIS_N_FEATURES
};

struct pod_cmis_source {
	const struct cmis_record *records; /* one per entity, consecutive */
	/* Optional: called around each record read, e.g. the poller's lock. */
	void (*lock)(void *user, unsigned idx);
	void (*unlock)(void *user, unsigned idx);
	void *user;
};

/* pod_read_fn: user is a struct pod_cmis_source *. */
bool pod_cmis_read(void *user, unsigned entity, float *out);

/* Decodes one record. False if the module is not Ready or has no active lane. */
bool pod_cmis_extract(const struct cmis_record *rec, float out[POD_CMIS_N_FEATURES]);

const char *pod_cmis_feature_name(unsigned feature);

#ifdef __cplusplus
}
#endif

#endif /* POD_CMIS_POD_CMIS_H */

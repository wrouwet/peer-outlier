/*
 * pod: peer outlier detection.
 *
 * Watches a population of similar entities (optical modules, fans, power
 * supplies, motors, servers, ...) each described by the same set of numeric
 * features, and finds the one that stands out from its peers. Nothing is
 * pretrained and no limits are programmed: each call to pod_update() pulls
 * the current features through a callback and compares every entity with the
 * others in its group, in two views:
 *
 *   DIFFERENT  unlike its peers right now                  (current values)
 *   CHANGING   changing unlike its peers                   (fast minus slow average)
 *
 * Per view and group, an entity is a candidate when
 *   - an Isolation Forest fitted on the group isolates it unusually easily
 *     (catches combinations of small deviations), or
 *   - one feature alone is far from the group, by a robust z-score that is
 *     Bonferroni-corrected for the number of values tested (catches the
 *     single-feature outliers an Isolation Forest dilutes).
 * It is an alert when it stays a candidate over several cycles.
 *
 * The number of entities and features is chosen at runtime. All memory is
 * supplied by the caller: pod_mem_size() at runtime, or POD_MEM_DEFINE() for
 * a static buffer.
 *
 *   static bool read(void *user, unsigned i, float *out) { ...fill out[0..d-1]...; return true; }
 *
 *   POD_MEM_DEFINE(mem, 64, 6);                 // 64 entities, 6 features
 *   static struct pod pod;
 *
 *   pod_init(&pod, NULL, 64, 6, read, NULL, mem, sizeof(mem));
 *   for (;;) {                                  // e.g. hourly
 *           pod_update(&pod);
 *           if (pod_result(&pod, i)->flags & POD_FLAG_ALERT_CHANGING) ...
 *   }
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef POD_POD_H
#define POD_POD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pod/iforest.h"

#ifdef __cplusplus
extern "C" {
#endif

#define POD_VERSION_MAJOR 0
#define POD_VERSION_MINOR 1
#define POD_VERSION_PATCH 0

#define POD_MAX_ENTITIES 65535u
#define POD_MAX_FEATURES 1024u

enum pod_view {
	POD_VIEW_DIFFERENT,
	POD_VIEW_CHANGING,
	POD_VIEW_COUNT
};

#define POD_FLAG_CAND_DIFFERENT  0x01u /* stood out this cycle */
#define POD_FLAG_CAND_CHANGING   0x02u
#define POD_FLAG_ALERT_DIFFERENT 0x04u /* stood out persistently */
#define POD_FLAG_ALERT_CHANGING  0x08u
#define POD_FLAG_CAND(view)  (0x01u << (view))
#define POD_FLAG_ALERT(view) (0x04u << (view))

/*
 * None of these is a limit on a measured quantity: they set timescales, the
 * evidence an alert needs, statistical constants and model size.
 * pod_config_default() suits one pod_update() per hour.
 */
struct pod_config {
	uint16_t n_trees;       /* trees per Isolation Forest (100) */
	uint16_t max_samples;   /* Isolation Forest subsample per tree (256) */
	uint16_t min_group;     /* smallest group compared with peers (8) */
	uint16_t fast_cycles;   /* smoothing of the current value (4) */
	uint16_t slow_cycles;   /* memory of the "before" reference (168 = 1 week hourly) */
	uint16_t warmup_cycles; /* samples before an entity joins the CHANGING view (24) */
	uint8_t persist_window; /* cycles looked back for an alert, 1..32 (24) */
	uint8_t persist_count;  /* candidate cycles in the window for an alert (18) */
	float z_cutoff;         /* robust z of an isolation score for a candidate (3.5) */
	float family_alpha;     /* false-alarm rate per group snapshot, single-feature test (0.01) */
	uint32_t seed;          /* random seed */
};

/*
 * Fills out[0..n_features-1] with entity's current features. Return false if
 * the entity has no valid data this cycle (absent, not ready, ...); it is then
 * left out. Called from pod_update() for each enabled entity, in order.
 */
typedef bool (*pod_read_fn)(void *user, unsigned entity, float *out);

struct pod_result {
	bool ready;                          /* read() returned data this cycle */
	bool scored[POD_VIEW_COUNT];         /* compared with peers in this view */
	float score[POD_VIEW_COUNT];         /* isolation score 0..1 (~0.5 ordinary) */
	float score_z[POD_VIEW_COUNT];       /* robust z of the score in its group */
	uint16_t top_feature[POD_VIEW_COUNT];/* feature that differs most from peers */
	float top_feature_z[POD_VIEW_COUNT]; /* its robust z in the group */
	uint8_t flags;                       /* POD_FLAG_* */
};

/* Internal per-entity state. */
struct pod_entity {
	uint32_t n_samples;
	uint32_t history[POD_VIEW_COUNT];
	uint16_t group;
	uint8_t enabled;
	struct pod_result res;
};

/* Detector state; treat as opaque. Lives wherever you like; its arrays live
 * in the memory passed to pod_init(). */
struct pod {
	struct pod_config cfg;
	pod_read_fn read;
	void *user;
	unsigned n, d;
	uint32_t rng;
	uint32_t cycle;
	struct pod_entity *ent;
	float *fast, *slow, *dev, *x; /* n * d each */
	float *score, *tmp, *tmp2;    /* n each */
	float *sample;                /* d */
	void *if_scratch;
	size_t if_scratch_size;
	uint16_t *members;            /* n */
	uint8_t *done;                /* n */
};

#define POD__ALIGN4(x) (((size_t)(x) + 3u) & ~(size_t)3u)
#define POD__MIN(a, b) ((a) < (b) ? (a) : (b))

/* Memory for n entities and d features with the default max_samples. */
#define POD_MEM_SIZE(n, d)                                                             \
	(POD__ALIGN4(sizeof(struct pod_entity) * (size_t)(n)) +                        \
	 sizeof(float) * (4u * (size_t)(n) * (size_t)(d) + 3u * (size_t)(n) + (size_t)(d)) + \
	 POD__ALIGN4(IFOREST_SCRATCH_SIZE((n), (d), POD__MIN((size_t)(n), (size_t)IFOREST_DEFAULT_SAMPLES))) + \
	 sizeof(uint16_t) * (size_t)(n) + (size_t)(n) + 4u)

/* Declares a suitably aligned static buffer: POD_MEM_DEFINE(mem, 32, 7); */
#define POD_MEM_DEFINE(name, n, d) static uint32_t name[(POD_MEM_SIZE((n), (d)) + 3u) / 4u]

void pod_config_default(struct pod_config *cfg);

/* Bytes of memory pod_init() needs (cfg may be NULL for defaults). */
size_t pod_mem_size(unsigned n_entities, unsigned n_features, const struct pod_config *cfg);

/*
 * mem must be 4-byte aligned and at least pod_mem_size() bytes; it is used
 * for the life of the detector. cfg may be NULL for defaults. All entities
 * start enabled, in group 0. Returns 0 or -1 for bad arguments.
 */
int pod_init(struct pod *p, const struct pod_config *cfg, unsigned n_entities,
	     unsigned n_features, pod_read_fn read, void *user, void *mem, size_t mem_size);

/* Entities are only compared within a group. Changing it resets history. */
void pod_set_group(struct pod *p, unsigned entity, uint16_t group);

/* Excludes an entity (an empty slot) or brings it back. */
void pod_set_enabled(struct pod *p, unsigned entity, bool enabled);

/* Forgets an entity's history, e.g. when the physical unit was replaced. */
void pod_reset_entity(struct pod *p, unsigned entity);

/* One detection cycle. Returns the number of entities with an alert, or -1. */
int pod_update(struct pod *p);

const struct pod_result *pod_result(const struct pod *p, unsigned entity);

/* Smoothed current value, and its change against the slow reference. */
float pod_level(const struct pod *p, unsigned entity, unsigned feature);
float pod_change(const struct pod *p, unsigned entity, unsigned feature);

const char *pod_view_name(unsigned view);

#ifdef __cplusplus
}
#endif

#endif /* POD_POD_H */

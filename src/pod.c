/*
 * pod: peer outlier detection. See pod.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "pod/pod.h"

#include <math.h>
#include <string.h>

#include "pod/robust.h"

static const char *const view_names[POD_VIEW_COUNT] = {
	[POD_VIEW_DIFFERENT] = "different",
	[POD_VIEW_CHANGING] = "changing",
};

const char *pod_view_name(unsigned view)
{
	return view < POD_VIEW_COUNT ? view_names[view] : "?";
}

void pod_config_default(struct pod_config *cfg)
{
	memset(cfg, 0, sizeof(*cfg));
	cfg->n_trees = 100;
	cfg->max_samples = IFOREST_DEFAULT_SAMPLES;
	cfg->min_group = 8;
	cfg->fast_cycles = 4;
	cfg->slow_cycles = 168;
	cfg->warmup_cycles = 24;
	cfg->persist_window = 24;
	cfg->persist_count = 18;
	cfg->z_cutoff = 3.5f;
	cfg->family_alpha = 0.01f;
	cfg->seed = 0x2545f491u;
}

static size_t align4(size_t x)
{
	return (x + 3u) & ~(size_t)3u;
}

size_t pod_mem_size(unsigned n, unsigned d, const struct pod_config *cfg)
{
	unsigned ms = cfg && cfg->max_samples ? cfg->max_samples : IFOREST_DEFAULT_SAMPLES;

	return align4(sizeof(struct pod_entity) * n) +
	       sizeof(float) * (4u * (size_t)n * d + 3u * (size_t)n + d) +
	       align4(iforest_scratch_size(n, d, ms)) + sizeof(uint16_t) * n + n + 4u;
}

static void reset_entity(struct pod *p, unsigned i)
{
	struct pod_entity *e = &p->ent[i];
	size_t off = (size_t)i * p->d;

	e->n_samples = 0;
	memset(e->history, 0, sizeof(e->history));
	memset(&e->res, 0, sizeof(e->res));
	memset(&p->fast[off], 0, p->d * sizeof(float));
	memset(&p->slow[off], 0, p->d * sizeof(float));
	memset(&p->dev[off], 0, p->d * sizeof(float));
}

int pod_init(struct pod *p, const struct pod_config *cfg, unsigned n, unsigned d,
	     pod_read_fn read, void *user, void *mem, size_t mem_size)
{
	struct pod_config c;

	if (!p || !read || !mem || ((uintptr_t)mem & 3u) || n < 1 || n > POD_MAX_ENTITIES ||
	    d < 1 || d > POD_MAX_FEATURES) {
		return -1;
	}
	if (cfg) {
		c = *cfg;
	} else {
		pod_config_default(&c);
	}
	if (c.n_trees == 0 || c.min_group < 3 || c.fast_cycles == 0 ||
	    c.slow_cycles < c.fast_cycles || c.persist_window == 0 || c.persist_window > 32 ||
	    c.persist_count == 0 || c.persist_count > c.persist_window || !(c.z_cutoff > 0.0f) ||
	    !(c.family_alpha > 0.0f && c.family_alpha <= 0.5f)) {
		return -1;
	}
	if (mem_size < pod_mem_size(n, d, &c)) {
		return -1;
	}

	memset(p, 0, sizeof(*p));
	memset(mem, 0, mem_size);
	p->cfg = c;
	p->read = read;
	p->user = user;
	p->n = n;
	p->d = d;
	p->rng = c.seed ? c.seed : 1u;

	uint8_t *m = mem;

	p->ent = (struct pod_entity *)m;
	m += align4(sizeof(struct pod_entity) * n);
	p->fast = (float *)m;
	p->slow = p->fast + (size_t)n * d;
	p->dev = p->slow + (size_t)n * d;
	p->x = p->dev + (size_t)n * d;
	p->score = p->x + (size_t)n * d;
	p->tmp = p->score + n;
	p->tmp2 = p->tmp + n;
	p->sample = p->tmp2 + n;
	m = (uint8_t *)(p->sample + d);
	p->if_scratch = m;
	p->if_scratch_size = iforest_scratch_size(n, d, c.max_samples);
	m += align4(p->if_scratch_size);
	p->members = (uint16_t *)m;
	p->done = (uint8_t *)(p->members + n);

	for (unsigned i = 0; i < n; i++) {
		p->ent[i].enabled = 1;
	}
	return 0;
}

void pod_set_group(struct pod *p, unsigned i, uint16_t group)
{
	if (i < p->n && p->ent[i].group != group) {
		p->ent[i].group = group;
		reset_entity(p, i);
	}
}

void pod_set_enabled(struct pod *p, unsigned i, bool enabled)
{
	if (i < p->n) {
		p->ent[i].enabled = enabled;
		if (!enabled) {
			reset_entity(p, i);
		}
	}
}

void pod_reset_entity(struct pod *p, unsigned i)
{
	if (i < p->n) {
		reset_entity(p, i);
	}
}

const struct pod_result *pod_result(const struct pod *p, unsigned i)
{
	return i < p->n ? &p->ent[i].res : NULL;
}

float pod_level(const struct pod *p, unsigned i, unsigned f)
{
	return i < p->n && f < p->d ? p->fast[(size_t)i * p->d + f] : 0.0f;
}

float pod_change(const struct pod *p, unsigned i, unsigned f)
{
	size_t k = (size_t)i * p->d + f;

	return i < p->n && f < p->d ? p->fast[k] - p->slow[k] : 0.0f;
}

/* Pulls every entity's features and updates its smoothed values. */
static void sample_entities(struct pod *p)
{
	const struct pod_config *c = &p->cfg;
	const unsigned d = p->d;

	for (unsigned i = 0; i < p->n; i++) {
		struct pod_entity *e = &p->ent[i];
		struct pod_result *r = &e->res;

		r->ready = false;
		r->scored[POD_VIEW_DIFFERENT] = false;
		r->scored[POD_VIEW_CHANGING] = false;
		r->flags &= (uint8_t)~(POD_FLAG_CAND_DIFFERENT | POD_FLAG_CAND_CHANGING);
		if (!e->enabled || !p->read(p->user, i, p->sample)) {
			continue;
		}

		uint32_t k = e->n_samples + 1u;
		float af = 1.0f / (float)(k < c->fast_cycles ? k : c->fast_cycles);
		float as = 1.0f / (float)(k < c->slow_cycles ? k : c->slow_cycles);
		float *fast = &p->fast[(size_t)i * d];
		float *slow = &p->slow[(size_t)i * d];
		float *dev = &p->dev[(size_t)i * d];

		for (unsigned j = 0; j < d; j++) {
			float v = p->sample[j];
			float step = v - slow[j];

			if (!isfinite(v)) {
				v = fast[j]; /* ignore a bad reading, keep the last value */
				step = 0.0f;
			}
			fast[j] += af * (v - fast[j]);
			/* Huber-style robust EWMA: after warm-up a sample moves the
			 * reference by at most 3x the entity's typical deviation, so a
			 * short glitch barely shifts it, while a lasting change is still
			 * absorbed as the deviation estimate grows. */
			if (k > c->warmup_cycles) {
				float lim = 3.0f * dev[j];

				step = step > lim ? lim : (step < -lim ? -lim : step);
			}
			dev[j] += as * (fabsf(v - slow[j]) - dev[j]);
			slow[j] += as * step;
		}
		if (k != 0) {
			e->n_samples = k;
		}
		r->ready = true;
	}
}

/* Scores the entities in p->members[0..n-1] in one view. */
static void score_view(struct pod *p, unsigned view, unsigned n)
{
	const unsigned d = p->d;
	const struct iforest_params ifp = { p->cfg.n_trees, p->cfg.max_samples };
	struct robust_stats s;

	for (unsigned q = 0; q < n; q++) {
		size_t src = (size_t)p->members[q] * d;
		float *row = &p->x[(size_t)q * d];

		for (unsigned j = 0; j < d; j++) {
			row[j] = view == POD_VIEW_DIFFERENT ? p->fast[src + j]
							    : p->fast[src + j] - p->slow[src + j];
		}
	}

	(void)iforest_score(p->x, n, d, d, &ifp, &p->rng, p->if_scratch, p->if_scratch_size,
			    p->score);

	/* Which feature sets each entity apart most from its peers. */
	for (unsigned q = 0; q < n; q++) {
		struct pod_result *r = &p->ent[p->members[q]].res;

		r->top_feature[view] = 0;
		r->top_feature_z[view] = 0.0f;
	}
	for (unsigned j = 0; j < d; j++) {
		for (unsigned q = 0; q < n; q++) {
			p->tmp2[q] = p->x[(size_t)q * d + j];
		}
		robust_compute(p->tmp2, n, p->tmp, &s);
		for (unsigned q = 0; q < n; q++) {
			struct pod_result *r = &p->ent[p->members[q]].res;
			float z = robust_z(p->tmp2[q], &s);

			if (fabsf(z) > fabsf(r->top_feature_z[view])) {
				r->top_feature[view] = (uint16_t)j;
				r->top_feature_z[view] = z;
			}
		}
	}

	/* Single-feature cutoff: significant at family_alpha after a Bonferroni
	 * correction for the n * d values tested; never below z_cutoff. */
	float feat_cut = robust_normal_upper_quantile(p->cfg.family_alpha / (2.0f * (float)n * (float)d));

	if (feat_cut < p->cfg.z_cutoff) {
		feat_cut = p->cfg.z_cutoff;
	}

	robust_compute(p->score, n, p->tmp, &s);
	for (unsigned q = 0; q < n; q++) {
		struct pod_result *r = &p->ent[p->members[q]].res;
		float z = robust_z(p->score[q], &s);

		r->scored[view] = true;
		r->score[view] = p->score[q];
		r->score_z[view] = z;
		if ((p->score[q] > 0.5f && z >= p->cfg.z_cutoff) ||
		    fabsf(r->top_feature_z[view]) >= feat_cut) {
			r->flags |= (uint8_t)POD_FLAG_CAND(view);
		}
	}
}

int pod_update(struct pod *p)
{
	const struct pod_config *c;
	int alerts = 0;

	if (!p || !p->ent) {
		return -1;
	}
	c = &p->cfg;
	p->cycle++;
	sample_entities(p);

	memset(p->done, 0, p->n);
	for (unsigned i = 0; i < p->n; i++) {
		if (p->done[i] || !p->ent[i].res.ready) {
			continue;
		}
		uint16_t group = p->ent[i].group;

		for (unsigned view = 0; view < POD_VIEW_COUNT; view++) {
			unsigned n = 0;

			for (unsigned j = i; j < p->n; j++) {
				const struct pod_entity *e = &p->ent[j];

				if (!e->res.ready || e->group != group) {
					continue;
				}
				p->done[j] = 1;
				if (view == POD_VIEW_CHANGING && e->n_samples < c->warmup_cycles) {
					continue;
				}
				p->members[n++] = (uint16_t)j;
			}
			if (n >= c->min_group) {
				score_view(p, view, n);
			}
		}
	}

	const uint32_t mask = c->persist_window >= 32 ? 0xffffffffu : ((1u << c->persist_window) - 1u);

	for (unsigned i = 0; i < p->n; i++) {
		struct pod_entity *e = &p->ent[i];

		for (unsigned view = 0; view < POD_VIEW_COUNT; view++) {
			uint32_t cand = (e->res.flags & POD_FLAG_CAND(view)) ? 1u : 0u;
			uint32_t h = ((e->history[view] << 1) | cand) & mask;
			unsigned count = 0;

			e->history[view] = h;
			while (h) {
				h &= h - 1u;
				count++;
			}
			if (count >= c->persist_count) {
				e->res.flags |= (uint8_t)POD_FLAG_ALERT(view);
			} else {
				e->res.flags &= (uint8_t)~POD_FLAG_ALERT(view);
			}
		}
		if (e->res.flags & (POD_FLAG_ALERT_DIFFERENT | POD_FLAG_ALERT_CHANGING)) {
			alerts++;
		}
	}
	return alerts;
}

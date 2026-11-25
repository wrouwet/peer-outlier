/*
 * SPDX-License-Identifier: Apache-2.0
 */
#include "fleet_sim.h"

#include <math.h>
#include <string.h>

#define TWO_PI 6.2831853f

void fleet_sim_default(struct fleet_sim_cfg *cfg)
{
	memset(cfg, 0, sizeof(*cfg));
	cfg->n_modules = 32;
	cfg->seed = 12345u;
	cfg->aging_module = 19;
	cfg->aging_start_day = 120;
	cfg->long_fibre_module = 7;
	cfg->fibre_module = FLEET_SIM_NONE;
	cfg->fibre_start_day = 90;
	cfg->glitch_module = 3;
	cfg->glitch_day = 150;
	cfg->hvac_day = 200;
}

void fleet_sim_init(struct fleet_sim *s, const struct fleet_sim_cfg *cfg)
{
	s->cfg = *cfg;
	s->rng = cfg->seed ? cfg->seed : 1u;
}

static uint32_t hash32(uint32_t x)
{
	x ^= x >> 16;
	x *= 0x7feb352du;
	x ^= x >> 15;
	x *= 0x846ca68bu;
	x ^= x >> 16;
	return x;
}

static float unit_from(uint32_t h)
{
	return ((float)(h >> 8) + 0.5f) * (1.0f / 16777216.0f);
}

static float box_muller(float u1, float u2)
{
	return sqrtf(-2.0f * logf(u1)) * cosf(TWO_PI * u2);
}

/* Fixed per-module/per-lane offset, N(0,1), derived from a hash so the
 * simulator needs no per-module storage. */
static float fixed_gauss(const struct fleet_sim *s, unsigned module, unsigned lane, unsigned kind)
{
	uint32_t k = s->cfg.seed ^ (module * 0x9e3779b9u) ^ (lane * 0x85ebca6bu) ^ (kind * 0xc2b2ae35u);
	uint32_t a = hash32(k), b = hash32(a ^ 0x68e31da4u);

	return box_muller(unit_from(a), unit_from(b));
}

static float noise(struct fleet_sim *s)
{
	uint32_t a = s->rng = hash32(s->rng + 0x9e3779b9u);
	uint32_t b = s->rng = hash32(s->rng + 0x9e3779b9u);

	return box_muller(unit_from(a), unit_from(b));
}

static float progress(unsigned hour, unsigned start_day)
{
	float day = (float)hour / 24.0f;
	float start = (float)start_day;
	float d;

	if (day <= start || start >= 365.0f) {
		return 0.0f;
	}
	d = (day - start) / (365.0f - start);
	return d > 1.0f ? 1.0f : d;
}

float fleet_sim_degradation(const struct fleet_sim *s, unsigned module, unsigned hour)
{
	if (module == s->cfg.aging_module) {
		return progress(hour, s->cfg.aging_start_day);
	}
	if (module == s->cfg.fibre_module) {
		return progress(hour, s->cfg.fibre_start_day);
	}
	return 0.0f;
}

static void put_u16(struct cmis_record *r, unsigned addr, uint16_t v)
{
	uint8_t *p = addr < CMIS_PAGE_BYTES ? &r->lower[addr] : &r->page11h[addr - CMIS_PAGE_BYTES];

	p[0] = (uint8_t)(v >> 8);
	p[1] = (uint8_t)v;
}

static uint16_t clamp_u16(float v)
{
	if (v < 0.0f) {
		return 0;
	}
	if (v > 65535.0f) {
		return 65535;
	}
	return (uint16_t)(v + 0.5f);
}

static uint16_t dbm_to_raw(float dbm)
{
	return clamp_u16(10000.0f * powf(10.0f, dbm / 10.0f)); /* 0.1 uW units */
}

void fleet_sim_step(struct fleet_sim *s, unsigned hour, struct cmis_record *records)
{
	const struct fleet_sim_cfg *c = &s->cfg;
	float day = (float)hour / 24.0f;
	float room = 24.0f + 2.0f * sinf(TWO_PI * day) + 4.0f * sinf(TWO_PI * day / 365.0f);

	if (c->hvac_day && day >= (float)c->hvac_day && day < (float)c->hvac_day + 2.0f) {
		room += 8.0f;
	}

	for (unsigned m = 0; m < c->n_modules; m++) {
		struct cmis_record *r = &records[m];
		float d = m == c->aging_module ? fleet_sim_degradation(s, m, hour) : 0.0f;
		float fd = m == c->fibre_module ? fleet_sim_degradation(s, m, hour) : 0.0f;
		float temp = room + 14.0f + 2.0f * fixed_gauss(s, m, 0, 1) + 0.25f * noise(s) + 3.0f * d;
		float vcc = 3.30f + 0.01f * fixed_gauss(s, m, 0, 2) + 0.002f * noise(s);
		float fibre_loss = 2.5f + 0.5f * fixed_gauss(s, m, 0, 3) + (m == c->long_fibre_module ? 4.0f : 0.0f) +
				   3.0f * fd;
		bool glitch = m == c->glitch_module && day >= (float)c->glitch_day &&
			      day < (float)c->glitch_day + 0.125f;

		memset(r, 0, sizeof(*r));
		r->lower[CMIS_ADDR_IDENTIFIER] = 0x18;   /* QSFP-DD */
		r->lower[CMIS_ADDR_REVISION] = 0x52;     /* CMIS 5.2 */
		r->lower[CMIS_ADDR_MODULE_STATE] = CMIS_MODULE_READY << 1;
		put_u16(r, CMIS_ADDR_TEMPERATURE, (uint16_t)(int16_t)lroundf(temp * 256.0f));
		put_u16(r, CMIS_ADDR_VCC, clamp_u16(vcc * 1e4f));
		for (unsigned b = 0; b < 4; b++) {
			r->page11h[CMIS_ADDR_DP_STATE - CMIS_PAGE_BYTES + b] =
				(CMIS_DP_ACTIVATED << 4) | CMIS_DP_ACTIVATED;
		}

		for (unsigned l = 0; l < CMIS_MAX_LANES; l++) {
			float bias = 38.0f + 3.0f * fixed_gauss(s, m, l, 4) + 0.35f * (temp - 38.0f) +
				     0.2f * noise(s) + 14.0f * d;
			float tx = 0.5f + 0.4f * fixed_gauss(s, m, l, 5) + 0.05f * noise(s) - 1.5f * d * d * d;
			float rx = tx - fibre_loss + 0.3f * fixed_gauss(s, m, l, 6) + 0.08f * noise(s);

			if (glitch) {
				rx -= 6.0f;
			}
			put_u16(r, CMIS_ADDR_TX_BIAS + 2u * l, clamp_u16(bias / 0.002f));
			put_u16(r, CMIS_ADDR_TX_POWER + 2u * l, dbm_to_raw(tx));
			put_u16(r, CMIS_ADDR_RX_POWER + 2u * l, dbm_to_raw(rx));
		}
	}
}

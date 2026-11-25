/*
 * CMIS record as cached in SRAM, plus decoders for the fields the outlier
 * detector uses.
 *
 * The record holds the module's bytes exactly as read over I2C: lower memory
 * (bytes 0..127) followed by the upper half of page 11h (bytes 128..255).
 * All accessors take CMIS byte addresses as written in the spec (0..255), so
 * offsets can be checked against the CMIS document directly.
 *
 * Offsets follow CMIS 5.x (OIF-CMIS-05.x). Check them against the revision
 * your modules report in byte 1 before relying on them.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef POD_CMIS_CMIS_RECORD_H
#define POD_CMIS_CMIS_RECORD_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CMIS_PAGE_BYTES 128u
#define CMIS_MAX_LANES  8u

/* One module's cached CMIS memory, 256 bytes, stored as read. */
struct cmis_record {
	uint8_t lower[CMIS_PAGE_BYTES];   /* bytes 0..127 */
	uint8_t page11h[CMIS_PAGE_BYTES]; /* page 11h; page11h[0] is byte 128 */
};

_Static_assert(sizeof(struct cmis_record) == 256, "cmis_record must be 256 bytes");

/* ---- Lower memory (bytes 0..127) ---- */
#define CMIS_ADDR_IDENTIFIER   0   /* SFF-8024 identifier (0x18 QSFP-DD, 0x19 OSFP, ...) */
#define CMIS_ADDR_REVISION     1   /* CMIS revision, major.minor nibbles */
#define CMIS_ADDR_MODULE_STATE 3   /* bits 3..1: module state */
#define CMIS_ADDR_TEMPERATURE  14  /* s16 big-endian, 1/256 degC */
#define CMIS_ADDR_VCC          16  /* u16 big-endian, 100 uV */

/* ---- Page 11h (bytes 128..255), lanes 1..8 ---- */
#define CMIS_ADDR_DP_STATE     128 /* 4 bytes, one nibble per lane, lane 1 = low nibble */
#define CMIS_ADDR_TX_FAULT     135 /* latched, bit n = lane n+1 */
#define CMIS_ADDR_TX_LOS       136
#define CMIS_ADDR_TX_LOL       137
#define CMIS_ADDR_RX_LOS       147
#define CMIS_ADDR_RX_LOL       148
#define CMIS_ADDR_TX_POWER     154 /* 8 x u16 big-endian, 0.1 uW */
#define CMIS_ADDR_TX_BIAS      170 /* 8 x u16 big-endian, 2 uA x multiplier (page 01h) */
#define CMIS_ADDR_RX_POWER     186 /* 8 x u16 big-endian, 0.1 uW */

enum cmis_module_state {
	CMIS_MODULE_LOW_PWR = 1,
	CMIS_MODULE_PWR_UP = 2,
	CMIS_MODULE_READY = 3,
	CMIS_MODULE_PWR_DN = 4,
	CMIS_MODULE_FAULT = 5,
};

enum cmis_dp_state {
	CMIS_DP_DEACTIVATED = 1,
	CMIS_DP_INIT = 2,
	CMIS_DP_DEINIT = 3,
	CMIS_DP_ACTIVATED = 4,
	CMIS_DP_TX_TURN_ON = 5,
	CMIS_DP_TX_TURN_OFF = 6,
	CMIS_DP_INITIALIZED = 7,
};

/* Byte at CMIS address addr (0..255). */
static inline uint8_t cmis_byte(const struct cmis_record *r, unsigned addr)
{
	return addr < CMIS_PAGE_BYTES ? r->lower[addr] : r->page11h[(addr - CMIS_PAGE_BYTES) & 0x7fu];
}

static inline uint16_t cmis_u16(const struct cmis_record *r, unsigned addr)
{
	return (uint16_t)((cmis_byte(r, addr) << 8) | cmis_byte(r, addr + 1));
}

static inline enum cmis_module_state cmis_module_state(const struct cmis_record *r)
{
	return (enum cmis_module_state)((cmis_byte(r, CMIS_ADDR_MODULE_STATE) >> 1) & 0x7u);
}

static inline float cmis_temperature_c(const struct cmis_record *r)
{
	return (float)(int16_t)cmis_u16(r, CMIS_ADDR_TEMPERATURE) / 256.0f;
}

static inline float cmis_vcc_v(const struct cmis_record *r)
{
	return (float)cmis_u16(r, CMIS_ADDR_VCC) * 1e-4f;
}

/* lane is 0-based (0 = CMIS lane 1). */
static inline enum cmis_dp_state cmis_dp_state(const struct cmis_record *r, unsigned lane)
{
	uint8_t b = cmis_byte(r, CMIS_ADDR_DP_STATE + lane / 2u);

	return (enum cmis_dp_state)((lane & 1u) ? (b >> 4) : (b & 0xfu));
}

static inline uint16_t cmis_tx_power_raw(const struct cmis_record *r, unsigned lane)
{
	return cmis_u16(r, CMIS_ADDR_TX_POWER + 2u * lane);
}

static inline uint16_t cmis_tx_bias_raw(const struct cmis_record *r, unsigned lane)
{
	return cmis_u16(r, CMIS_ADDR_TX_BIAS + 2u * lane);
}

static inline uint16_t cmis_rx_power_raw(const struct cmis_record *r, unsigned lane)
{
	return cmis_u16(r, CMIS_ADDR_RX_POWER + 2u * lane);
}

static inline bool cmis_lane_flag(const struct cmis_record *r, unsigned addr, unsigned lane)
{
	return (cmis_byte(r, addr) >> lane) & 1u;
}

#ifdef __cplusplus
}
#endif

#endif /* POD_CMIS_CMIS_RECORD_H */

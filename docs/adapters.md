# Adapters

pod works on rows of floats. An **adapter** is just the `pod_read_fn` that
turns your data into those floats. Writing one is usually a few lines.

## Writing an adapter

```c
struct my_unit { volatile uint16_t raw_temp, raw_speed, raw_current; bool online; };

static bool my_read(void *user, unsigned i, float *out)
{
        const struct my_unit *u = &((const struct my_unit *)user)[i];

        if (!u->online)
                return false;                         /* left out this cycle */
        out[0] = u->raw_temp * 0.0625f;               /* any units */
        out[1] = (float)u->raw_speed;
        out[2] = u->raw_current * 0.001f;
        out[3] = out[1] / (out[2] > 0.01f ? out[2] : 0.01f); /* a derived ratio */
        return true;
}

pod_init(&pod, NULL, n_units, 4, my_read, units, mem, sizeof(mem));
```

Guidelines:

- **Units don't matter.** Isolation Forest is scale-free and the robust
  z-score is relative, so scaling factors only need to be consistent across
  entities, not correct.
- **Derived features help.** A ratio such as "output per unit of drive"
  often reveals wear earlier than either input on its own.
- **Use log scale for quantities spanning orders of magnitude.**
- **Return `false` rather than inventing values** for an entity without
  valid data.
- **Keep it fast and non-blocking.** It's called once per entity per cycle.
  Read from a cache; don't do bus transactions here.

## The CMIS optical-module adapter

`adapters/cmis` turns cached CMIS memory into features for pluggable optical
modules (QSFP-DD, OSFP, QSFP112, and other modules that implement the OIF
Common Management Interface Specification).

### The record

```c
#include "pod_cmis/cmis_record.h"

struct cmis_record {
        uint8_t lower[128];     /* bytes 0..127 */
        uint8_t page11h[128];   /* page 11h, bytes 128..255 */
};
```

This is 256 bytes per module: the module's bytes stored exactly as read over
I²C. Your poller keeps an array of these, and pod reads them in place.
`cmis_record.h` also provides decoders that take CMIS byte addresses exactly
as the specification writes them:

| Function | Field |
|---|---|
| `cmis_module_state()` | byte 3, bits 3..1 |
| `cmis_temperature_c()` | bytes 14–15, s16, 1/256 °C |
| `cmis_vcc_v()` | bytes 16–17, u16, 100 µV |
| `cmis_dp_state(r, lane)` | page 11h, bytes 128–131, one nibble per lane |
| `cmis_tx_power_raw(r, lane)` | page 11h, 154 + 2·lane, 0.1 µW |
| `cmis_tx_bias_raw(r, lane)` | page 11h, 170 + 2·lane, 2 µA × multiplier |
| `cmis_rx_power_raw(r, lane)` | page 11h, 186 + 2·lane, 0.1 µW |
| `cmis_lane_flag(r, addr, lane)` | latched lane flags (LOS, LOL, fault, ...) |

### Features

`pod_cmis_read()` produces `POD_CMIS_N_FEATURES` (7) features, computed over
the lanes in DPActivated state. It skips modules that aren't in the Ready
state, or that have no active lane.

| Index | Name | Meaning |
|---|---|---|
| `POD_CMIS_TEMP_C` | temperature | module temperature, °C |
| `POD_CMIS_VCC_V` | vcc | supply voltage, V |
| `POD_CMIS_TX_BIAS_MA` | tx_bias | mean laser bias, mA (before the page 01h multiplier) |
| `POD_CMIS_TX_POWER_DBM` | tx_power | mean Tx optical power, dBm |
| `POD_CMIS_RX_POWER_DBM` | rx_power | mean Rx optical power, dBm |
| `POD_CMIS_RX_SPREAD_DB` | rx_lane_spread | strongest minus weakest Rx lane, dB |
| `POD_CMIS_TX_EFFICIENCY` | tx_efficiency | Tx dBm − 10·log10(bias mA); falls as a laser ages |

`pod_cmis_feature_name(i)` returns the name.

### Use

```c
#include "pod/pod.h"
#include "pod_cmis/pod_cmis.h"

static struct cmis_record records[32];                 /* filled by your I2C poller */
static struct pod_cmis_source src = {
        .records = records,
        .lock = poller_lock, .unlock = poller_unlock,  /* optional */
};
POD_MEM_DEFINE(mem, 32, POD_CMIS_N_FEATURES);
static struct pod pod;

pod_init(&pod, NULL, 32, POD_CMIS_N_FEATURES, pod_cmis_read, &src, mem, sizeof(mem));
for (unsigned i = 0; i < 32; i++)
        pod_set_group(&pod, i, part_number_id(i));    /* from page 00h */
```

**Group IDs and swaps come from page 00h.** That page holds the part number
and serial number, and this record doesn't include it, so the application
supplies them. Call `pod_reset_entity()` when a cage's serial number
changes.

### What it catches (simulated)

| Fault | Signature | Typically flagged as |
|---|---|---|
| laser aging | bias rising at constant power, efficiency falling, slight heating | CHANGING, tx_bias / tx_efficiency |
| fibre or connector degradation | Rx power falling, nothing else | CHANGING, rx_power |
| long fibre run (healthy) | permanently low Rx power | DIFFERENT only |
| site-wide cooling failure | every module heats up | nothing (shared by peers) |

### Caveats

- **Byte offsets** follow CMIS 5.x. Check them against the CMIS revision your
  modules report in byte 1.
- **Tx bias** is reported before the page 01h multiplier. That's fine for
  comparing identical part numbers within a group.
- **Modules with more than 8 lanes** need page 11h of bank 1 as well, which
  this adapter doesn't read.
- **VDM observables** (pages 20h–2Fh: laser age, eSNR, pre-FEC BER) would
  make strong additional features. A future adapter could add them.

## Other domains

The same pattern works for any population of similar units:

| Domain | Possible features |
|---|---|
| fans | rpm, motor current, temperature, current/rpm |
| pumps | flow, pressure, motor current, vibration |
| power supplies | output ripple, efficiency, temperature, fan speed |
| disks | latency percentiles, error counts (log), temperature |
| servers or network ports | CPU temperature, power, error rates, utilisation |

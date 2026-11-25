# API reference

Public headers live in `include/pod/`. Most applications only need `pod.h`.

| Header | Purpose |
|---|---|
| `pod/pod.h` | the detection engine |
| `pod/iforest.h` | Isolation Forest scoring of any float matrix (used by the engine; also usable on its own) |
| `pod/robust.h` | median, MAD, robust z, normal quantile |
| `pod_cmis/pod_cmis.h` | optical-module adapter (see [adapters.md](adapters.md)) |

Version macros in `pod.h`: `POD_VERSION_MAJOR`, `POD_VERSION_MINOR`,
`POD_VERSION_PATCH`.

---

## pod.h: the engine

### Limits

| Macro | Value |
|---|---|
| `POD_MAX_ENTITIES` | 65535 |
| `POD_MAX_FEATURES` | 1024 |

### `pod_read_fn`

```c
typedef bool (*pod_read_fn)(void *user, unsigned entity, float *out);
```

Supplies one entity's features. Write `out[0 .. n_features-1]` and return
`true`, or return `false` if the entity has nothing valid this cycle. The
entity is then left out of this cycle and its history is untouched.

- It's called from `pod_update()` once per enabled entity, in index order.
- `user` is the pointer given to `pod_init()`.
- A non-finite value in `out` is ignored for that feature, and the previous
  value is kept.
- If other threads write the underlying data, do any locking inside the
  callback.

### `struct pod_config`

Fill it with `pod_config_default()` and change what you need, or pass `NULL`
for all defaults. The defaults suit one `pod_update()` per hour.

| Field | Default | Meaning |
|---|---|---|
| `n_trees` | 100 | trees per Isolation Forest; cost is linear in this |
| `max_samples` | 256 | Isolation Forest subsample per tree |
| `min_group` | 8 | smallest group compared with peers (≥ 3) |
| `fast_cycles` | 4 | smoothing of the current value |
| `slow_cycles` | 168 | length of the "before" reference (≥ `fast_cycles`) |
| `warmup_cycles` | 24 | samples before an entity joins the CHANGING view |
| `persist_window` | 24 | cycles looked back for an alert (1–32) |
| `persist_count` | 18 | candidate cycles within the window needed for an alert |
| `z_cutoff` | 3.5 | robust z an isolation score must reach; also the floor of the per-feature cutoff |
| `family_alpha` | 0.01 | false-alarm rate per group snapshot for the per-feature test (0–0.5) |
| `seed` | fixed | random seed (any value) |

### Memory

pod never allocates. Everything except the small `struct pod` lives in a
buffer you provide, which must be 4-byte aligned.

```c
size_t pod_mem_size(unsigned n_entities, unsigned n_features,
                    const struct pod_config *cfg);   /* cfg may be NULL */

POD_MEM_SIZE(n, d)          /* compile-time constant, for the default max_samples */
POD_MEM_DEFINE(name, n, d); /* declares: static uint32_t name[...] */
```

The size is roughly 16 bytes × n × d, plus about 60 bytes per entity. For
example:

| n × d | bytes |
|---|---|
| 32 × 7 | 6 404 |
| 64 × 6 | 11 708 |
| 128 × 7 | 25 412 |
| 256 × 16 | 87 672 |

If you raise `max_samples` above 256, size the buffer with `pod_mem_size()`,
not the macro.

### Functions

```c
void pod_config_default(struct pod_config *cfg);
```
Fills `cfg` with the defaults above.

```c
int pod_init(struct pod *p, const struct pod_config *cfg,
             unsigned n_entities, unsigned n_features,
             pod_read_fn read, void *user, void *mem, size_t mem_size);
```
Prepares a detector.
- `cfg` may be `NULL` for the defaults.
- `mem` must stay valid for the detector's lifetime.
- All entities start enabled, in group 0.
- Returns 0, or −1 for invalid arguments, an invalid configuration, or too
  little memory.

```c
void pod_set_group(struct pod *p, unsigned entity, uint16_t group);
```
Entities are only compared with others in the same group. Give like entities
(same model or part number) the same group. Changing an entity's group resets
its history.

```c
void pod_set_enabled(struct pod *p, unsigned entity, bool enabled);
```
Excludes an entity, e.g. an empty slot, or brings it back. Disabling resets
its history.

```c
void pod_reset_entity(struct pod *p, unsigned entity);
```
Forgets an entity's history. Call it when the physical unit is replaced.

```c
int pod_update(struct pod *p);
```
Runs one detection cycle: read, smooth, compare, decide. Returns the number
of entities currently in alert (in either view), or −1 if `p` is invalid.
Call it periodically from one thread.

```c
const struct pod_result *pod_result(const struct pod *p, unsigned entity);
```
The entity's result from the latest cycle, or `NULL` for a bad index. The
pointer stays valid; its contents change on every `pod_update()`.

```c
float pod_level(const struct pod *p, unsigned entity, unsigned feature);
float pod_change(const struct pod *p, unsigned entity, unsigned feature);
```
The smoothed current value, and its change against the slow reference: the
inputs to the DIFFERENT and CHANGING views. Useful for logging and
dashboards.

```c
const char *pod_view_name(unsigned view);   /* "different", "changing" */
```

### `struct pod_result`

| Field | Meaning |
|---|---|
| `ready` | the read callback returned data this cycle |
| `scored[view]` | compared with peers in this view (false if the group is too small, or CHANGING is still warming up) |
| `score[view]` | isolation score 0..1; about 0.5 is ordinary, near 1 is isolated |
| `score_z[view]` | robust z of that score within the group |
| `top_feature[view]` | index of the feature that differs most from peers |
| `top_feature_z[view]` | its robust z; the sign gives the direction (negative = lower than peers) |
| `flags` | see below |

`view` is `POD_VIEW_DIFFERENT` or `POD_VIEW_CHANGING`.

### Flags

| Flag | Meaning |
|---|---|
| `POD_FLAG_CAND_DIFFERENT` | stood out this cycle, DIFFERENT view |
| `POD_FLAG_CAND_CHANGING` | stood out this cycle, CHANGING view |
| `POD_FLAG_ALERT_DIFFERENT` | stood out persistently, DIFFERENT view |
| `POD_FLAG_ALERT_CHANGING` | stood out persistently, CHANGING view |
| `POD_FLAG_CAND(view)`, `POD_FLAG_ALERT(view)` | the same bits, by view index |

Candidate flags reflect this cycle only. Alert flags are set and cleared by
the persistence rule.

### Reading results

```c
if (pod_update(&pod) > 0) {
        for (unsigned i = 0; i < n; i++) {
                const struct pod_result *r = pod_result(&pod, i);

                if (r->flags & POD_FLAG_ALERT_CHANGING)
                        /* entity i is drifting unlike its peers, mostly in
                         * feature r->top_feature[POD_VIEW_CHANGING] */;
                else if (r->flags & POD_FLAG_ALERT_DIFFERENT)
                        /* entity i is persistently unlike its peers */;
        }
}
```

---

## iforest.h: Isolation Forest on its own

```c
struct iforest_params { uint16_t n_trees; uint16_t max_samples; /* 0 = 256 */ };

size_t iforest_scratch_size(unsigned n, unsigned d, unsigned max_samples);
IFOREST_SCRATCH_SIZE(n, d, s)   /* compile time; s = min(n, max_samples) */

int iforest_score(const float *x, unsigned n, unsigned d, unsigned stride,
                  const struct iforest_params *p, uint32_t *rng,
                  void *scratch, size_t scratch_size, float *score);

float iforest_c(unsigned n);    /* the normaliser c(n) */
```

`iforest_score()` scores the rows of `x` and writes `score[0..n-1]`.
- **Layout:** row *i* starts at `x + i * stride`, where `stride ≥ d` is
  counted in floats, so you can score a column subset of a wider table in
  place.
- **Random state:** `rng` is the caller's state; it's advanced in place.
- **Scratch:** must be at least `iforest_scratch_size()` bytes and 4-byte
  aligned.
- **Returns** 0, or −1 for bad arguments or too little scratch.

Scores are deterministic for a given `rng` value and input.

---

## robust.h

```c
struct robust_stats { float med, mad, meanad; };

float robust_median_inplace(float *v, unsigned n);         /* sorts v */
void  robust_compute(const float *v, unsigned n, float *tmp, struct robust_stats *s);
float robust_z(float v, const struct robust_stats *s);     /* clamped to ±99 */
float robust_normal_upper_quantile(float p);               /* z with P(Z > z) = p */
```

---

## Thread safety and reentrancy

- **No global state.** Separate `struct pod` instances are fully
  independent.
- **One instance, one thread.** A single instance must not be used from
  several threads at once.
- **Read results from the same thread.** `pod_result()` reads what
  `pod_update()` writes; read them from that thread, or synchronise.

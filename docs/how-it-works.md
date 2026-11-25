# How pod works

pod answers two questions about every entity, every cycle, by comparing it
with its peers:

| View | Question | Input per entity |
|---|---|---|
| **DIFFERENT** | Is it unlike its peers right now? | smoothed current values |
| **CHANGING** | Is it changing in a way its peers are not? | smoothed current minus a robust long average |

CHANGING is usually the one to act on. Slow degradation, such as wear, aging
or a growing leak, shows up as a change that only one entity has. Things that
happen to everyone at once (season, load, the room warming up) show up as a
change every entity shares, and cancel out. DIFFERENT also flags entities
that are permanently unusual but healthy, such as a unit in a hot corner or
on a longer cable. Treat it as information.

One call to `pod_update()` runs the five steps below.

## 1. Read

For each enabled entity, pod calls your `pod_read_fn` to fill in that
entity's `d` features as floats. If the callback returns `false` (absent, not
ready, faulted), the entity sits out the cycle. A non-finite value (NaN or
inf) is ignored and the previous value is kept.

## 2. Smooth and remember

pod keeps three running values per entity and feature. It stores no time
series.

| Value | Meaning | Update |
|---|---|---|
| `fast` | "now", with noise smoothed | EWMA over `fast_cycles` (4) |
| `slow` | "before" | robust EWMA over `slow_cycles` (168) |
| `dev` | typical deviation from "before" | EWMA of the absolute deviation over `slow_cycles` |

**Warm-up:** while an entity has fewer samples than a window, the weight is
1/k, so the averages start as plain running means.

**Robust `slow`:** once warmed up, a sample moves `slow` by at most
3 × `dev` (a Huber-style clip). This has two effects:
- a short glitch barely moves the reference, so it doesn't leave a fake
  "change" behind for days afterwards;
- a genuine, lasting shift is still absorbed, because `dev` grows while the
  shift persists.

The values used later are:
- **level** = `fast`, used by DIFFERENT;
- **change** = `fast − slow`, used by CHANGING.

## 3. Compare with peers

Entities are only compared within their **group** (`pod_set_group()`, e.g. by
model or part number). For each group with at least `min_group` (8) members,
and for each view, pod builds an n × d table and runs two tests on it.

An entity joins the CHANGING view after `warmup_cycles` (24) samples, so a
freshly added or replaced unit isn't judged on a half-formed reference.

### 3a. Isolation Forest, fitted on the spot

An *isolation tree* splits the group repeatedly. At each node it:
1. picks a random feature that varies within the node;
2. cuts it at a uniformly random point between that feature's minimum and
   maximum within the node;
3. sends each entity left or right.

An entity unlike the others gets isolated in few cuts; one inside the crowd
needs many. pod averages the path length *h(x)* over `n_trees` (100) random
trees and normalises it:

```
score(x) = 2^( −E[h(x)] / c(n) ),   c(n) = 2(ln(n−1) + γ) − 2(n−1)/n
```

*c(n)* is the average path length of an unsuccessful binary-search-tree
search. A score of about 0.5 is ordinary; close to 1 is easily isolated.

Properties of the implementation (`src/iforest.c`):

- **Fit and score in one pass.** Each tree is built depth first, every
  entity's path length is added up at its leaf, and the tree is then thrown
  away. Memory doesn't grow with the number of trees, and no model is kept
  between cycles.
- **Scale-free.** Cuts are drawn between each feature's own min and max, so
  units and scales are irrelevant. Multiplying a feature by 1000 doesn't
  change the scores.
- **Cheap feature choice.** Features are tried in random order until one
  varies within the node. That's uniform among the varying features, and
  usually costs a single column scan.
- **Grown to isolation.** There's no depth limit. With the small groups pod
  typically sees, the usual log₂(n) limit makes scores indistinct.
- **Subsampling.** For groups larger than `max_samples` (256), each tree is
  grown on a random subsample, and every entity is still routed through it
  and scored. This is standard Isolation Forest practice.

**What it's good at:** an entity slightly off on several features at once,
where no single feature looks alarming.

**Where it's weak:** an entity far out on *one* feature only. The forest
picks that feature only about 1 time in d, so the outlier is diluted, and
the score measures ease of isolation rather than distance. A degrading
fibre, for example, shows only in received power. Hence 3b.

### 3b. Robust per-feature z-score

For each feature, pod computes the group's median and median absolute
deviation (MAD), and each entity's modified z-score (Iglewicz–Hoaglin):

```
z = 0.6745 × (value − median) / MAD
```

If more than half the values are identical (MAD = 0), the mean absolute
deviation is used instead, scaled by 1.2533. Unlike a mean and standard
deviation, the median and MAD are not dragged along by the outlier itself.

Testing n × d values per snapshot invites false hits by chance, so the
cutoff is **Bonferroni-corrected**:

```
cutoff = Φ⁻¹( 1 − family_alpha / (2 · n · d) ),   never below z_cutoff
```

With `family_alpha` = 0.01, that's about 4.2 for 32 entities × 7 features:
a 1% chance per group snapshot of any false hit. The cutoff adapts to the
group size and the number of features automatically.

pod also records, per entity and view, the feature with the largest |z|.
That's `top_feature` and `top_feature_z`, the "why" behind an alert.

### 3c. Candidate

In each view, an entity is a **candidate** this cycle if either of these
holds:

- its isolation score is above 0.5 *and* the robust z of that score within
  the group is at least `z_cutoff` (3.5);
- its `top_feature_z` exceeds the corrected per-feature cutoff.

## 4. Persistence

Each entity keeps a bit history per view. It's in **alert** when it was a
candidate in at least `persist_count` (18) of the last `persist_window` (24)
cycles. Noise and one-off glitches rarely last that long; real drift lasts
days. The alert clears by itself once the entity stops standing out.

## 5. Lifecycle

| Event | Call | Effect |
|---|---|---|
| unit replaced | `pod_reset_entity()` | history cleared; rejoins CHANGING after warm-up |
| slot emptied or filled | `pod_set_enabled()` | excluded or included; disabling clears history |
| regrouped | `pod_set_group()` | history cleared |

## Why these choices

- **Peers instead of thresholds.** Setting limits needs domain knowledge for
  every feature, and limits only fire once something is already bad.
  Comparing with peers needs neither, and it cancels out shared conditions.
- **Two tests instead of one.** Isolation Forest and the per-feature z cover
  each other's blind spots. In testing, either one alone missed scenarios
  the pair catches.
- **Robust statistics throughout.** The median, MAD and clipped averages keep
  one bad entity, or one bad sample, from skewing the reference everyone is
  judged against.
- **No neural network or NPU.** The model is refitted from live data every
  cycle, which NPUs (inference-only, compiled offline) can't do. The workload
  is also tiny. A pretrained network is worthwhile when the inputs are raw
  signals (waveforms, spectra) or there's labelled failure data, and it could
  then run alongside pod.

## Defaults and timescales

The defaults assume one `pod_update()` per hour: "now" is about 4 hours,
"before" is about a week, and an alert needs about 18 hours of evidence. At a
different rate, scale `fast_cycles`, `slow_cycles`, `warmup_cycles` and the
persistence settings accordingly. None of the settings is a limit on a
measured quantity.

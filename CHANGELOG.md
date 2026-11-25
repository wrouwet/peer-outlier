# Changelog

## 0.1.0 (2026-11-25)

First release.

- Core engine (`pod.h`):
  - entities × features chosen at runtime, with caller-supplied memory;
  - DIFFERENT and CHANGING views, groups, persistence, attribution.
- Isolation Forest (`iforest.h`):
  - fit and score in one pass, with no stored model;
  - scale-free, with subsampling for large groups and strided input.
- Robust statistics (`robust.h`).
- CMIS optical-module adapter (`pod_cmis`).
- Builds: CMake, a Zephyr module (with `CONFIG_POD_FPU`), and plain
  compilation.
- Examples, host tests, a simulated CMIS fleet, a Zephyr sample and
  benchmark, and CI.

# Integration

pod's core is portable C99 plus `<math.h>`. It uses `float` arithmetic, and
the functions `logf`, `expf`, `sqrtf` and `fabsf`. It does no dynamic
allocation, I/O or OS calls and has no global state. It runs anywhere a C
compiler does: a host PC, bare-metal Cortex-M or RISC-V, an RTOS, or Linux.

## CMake (any toolchain)

```sh
cmake -S . -B build                    # add your toolchain file for cross builds
cmake --build build
```

| Target | Contents |
|---|---|
| `pod` | the core library (`include/pod/`) |
| `pod_cmis` | the CMIS optical-module adapter (`adapters/cmis/include/pod_cmis/`) |
| `pod_example_basic` | host example |
| `test_pod` | host tests (`ctest`) |

| Option | Default | |
|---|---|---|
| `POD_BUILD_CMIS` | ON | build the CMIS adapter |
| `POD_BUILD_TESTS` | ON if top level | build the tests |
| `POD_BUILD_EXAMPLES` | ON if top level | build the examples |

As a subproject:

```cmake
add_subdirectory(third_party/pod)
target_link_libraries(app PRIVATE pod)       # and pod_cmis if needed
```

`cmake --install` installs the headers, the libraries and a `pod::` CMake
package.

## Without a build system

```sh
cc -std=c99 -O2 -Iinclude -c src/iforest.c src/robust.c src/pod.c
```

Link with `-lm` where the platform needs it.

## Zephyr

The repository is a Zephyr module (`zephyr/module.yml`). Add it with
`-DZEPHYR_EXTRA_MODULES=/path/to/pod`, or a west manifest entry, then:

```
CONFIG_POD=y
CONFIG_POD_CMIS=y            # optional: optical-module adapter
```

| Kconfig | |
|---|---|
| `CONFIG_POD` | the library |
| `CONFIG_POD_FPU` | defaults to y on SoCs with an FPU: selects `CONFIG_FPU` and implies `CONFIG_FPU_SHARING`. Hidden on SoCs without an FPU |
| `CONFIG_POD_CMIS` | the CMIS adapter |
| `CONFIG_POD_CMIS_FLEET_SIM` | simulated CMIS fleet, for demos and testing |

`samples/fleet_sim` runs pod on a simulated fleet of 32 optical modules and
reports µs per `pod_update()`. It's a quick on-target benchmark for any
board:

```sh
make zephyr BOARD=<your_board>          # ZEPHYR_BASE must be set
```

Sample Kconfig: `CONFIG_FLEET_SIM_DAYS` (default 365) and
`CONFIG_FLEET_SIM_TREE_SWEEP` (times 25/50/100/200 trees).

## Floating point

pod computes in single precision.

- **With an FPU** (e.g. Cortex-M4F/M7/M33F, or a RISC-V F extension), build
  with the hardware float ABI. Under Zephyr, `CONFIG_POD_FPU` does this.
- **Without an FPU**, the compiler's software float works; it's just slower.
  At typical update rates (minutes to hours) that rarely matters.

## Memory

All state lives in one caller-supplied buffer: `pod_mem_size(n, d)` bytes,
or `POD_MEM_DEFINE()` for a static one. That's roughly 16 × n × d bytes,
plus about 60 bytes per entity. `struct pod` itself is about 100 bytes.

The scratch for the Isolation Forest is included and sized for the largest
possible group. Stack use is small: a few local variables, and no recursion.

## Timing

`pod_update()` is linear in `n_trees`, roughly linear in the number of
entities per group times log of that, and linear in the number of features
for the statistics. Measured:

| Platform | Configuration | Time per cycle |
|---|---|---|
| x86-64 host | 32 × 7, 100 trees, ASan/UBSan build | ~1.7 ms |
| Cortex-M33, 96 MHz, no FPU (software float) | 32 × 7, 100 trees, earlier version (~2.4× slower than current) | ~520 ms |

To cut the time, reduce `n_trees`: 50 trees give very similar results for
groups of tens. You can also call `pod_update()` less often; it's designed
for slow telemetry. `pod_update()` runs to completion. If that's too long for
your system, call it from a low-priority thread.

## Threading

pod takes no locks and starts no threads. Typical arrangements:

- **One worker thread or work item** calls `pod_update()` periodically and
  reads the results.
- **The data producer**, such as a bus poller or an ISR filling a buffer,
  writes the entity data. If an update can be torn, make the read callback
  consistent: take a lock around the copy, or use a sequence counter and
  retry.

The CMIS adapter has optional `lock` and `unlock` hooks for exactly this.

## Choosing groups and features

- **Group like with like:** same model, part number or role. Differences
  between models would otherwise dominate.
- **Prefer a few meaningful features over many raw ones.** Ratios that
  capture health are useful, for example output per unit of drive.
- **Use log scale for quantities that vary by orders of magnitude**, such as
  optical power in dBm rather than µW.
- **Features shouldn't depend on the absolute time of day.** That's shared
  across peers anyway, and cancels out.

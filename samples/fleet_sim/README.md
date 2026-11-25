# Zephyr sample: fleet_sim

Runs pod with the CMIS adapter on a simulated fleet of 32 optical modules,
on any Zephyr board, and prints:
- every alert change;
- the time per `pod_update()`;
- optionally, a sweep over forest sizes.

Use it as an on-target benchmark.

```sh
west build -b <board> samples/fleet_sim -- -DZEPHYR_EXTRA_MODULES=/path/to/pod
# or, from the repository root:
make zephyr BOARD=<board>
```

| Kconfig | Default | |
|---|---|---|
| `CONFIG_FLEET_SIM_DAYS` | 365 | simulated days (one `pod_update()` per simulated hour) |
| `CONFIG_FLEET_SIM_TREE_SWEEP` | y | afterwards, time 25/50/100/200 trees |

RAM: about 20 KB in total (8 KB of CMIS records, 6.4 KB of pod state, plus
Zephyr and printk). On SoCs with an FPU, `CONFIG_POD_FPU` enables it
automatically.

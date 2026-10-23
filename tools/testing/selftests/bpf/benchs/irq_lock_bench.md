# IRQ save/restore shared-lock microbenchmark

`irq-lock` measures an empty critical section on one shared
`bpf_res_spin_lock`. Every iteration performs:

1. `bpf_local_irq_save(&flags)`
2. `bpf_res_spin_lock(&shared_lock)`
3. `bpf_res_spin_unlock(&shared_lock)`
4. `bpf_local_irq_restore(&flags)`

Producer threads are pinned to separate guest CPUs and repeatedly run the same
TC BPF program with `BPF_PROG_TEST_RUN`. Each producer has its own sample buffer;
the lock is shared across all producers. Producers continue running after their
buffers fill, keeping contention present until every producer has finished.
Any lock failure or incomplete sample buffer fails the benchmark.

Two `bpf_ktime_get_ns()` calls bracket a batch of 256 lock/unlock pairs. Syscall,
packet setup, and sample-publication overhead are outside this interval. The
reported units are nanoseconds per complete pair, including IRQ save/restore
and loop overhead. Average, sample standard deviation, and maximum describe
batch-averaged costs, not individual worst-case lock acquisitions. No outliers
are removed. Use `--irq-lock-batch 1` for individual-pair samples, at the cost of
two timing helper calls per pair.

## Running

From `tools/testing/selftests/bpf`:

```bash
make -j$(nproc) bench
KERNEL_CMDLINE_EXTRA='clocksource=tsc tsc=reliable nmi_watchdog=0 idle=poll loglevel=3' \
    ./vmtest.sh -- bash ./benchs/run_bench_irq_lock.sh fixup
```

The sweep runs every producer count from 1 through 8 three times. Each run warms
up for one second, then collects 4096 samples per producer. These settings are
adjustable with `IRQ_LOCK_RUNS`, `IRQ_LOCK_BATCH`, and `IRQ_LOCK_SAMPLES`. The
runner temporarily disables BPF JIT constant blinding, restoring the previous
sysctl value on exit.

For comparisons, use identical kernel configurations and the same benchmark
binary for both kernels. On x86-64, disable `CONFIG_TRACE_IRQFLAGS` and
`CONFIG_PARAVIRT_XXL` so the fixup kernel actually inlines IRQ operations; the
standard selftest configuration can otherwise select the kfunc fallback.
Keep VM clocksource, host CPU placement, and vCPU count identical, and run the
VMs sequentially rather than concurrently.

The `irq_insns` output counts kernel-private IRQ instructions in the translated
program. Set `IRQ_LOCK_EXPECT_INLINE=0` for the unmodified kernel or `1` for the
fixup kernel to make the sweep check its selection. Expected counts are zero
and three respectively: one save, one normal restore, and one error-path
restore. For example:

```bash
KERNEL_CMDLINE_EXTRA='clocksource=tsc tsc=reliable nmi_watchdog=0 idle=poll loglevel=3' \
    ./vmtest.sh -- bash -c \
    'IRQ_LOCK_EXPECT_INLINE=1 bash ./benchs/run_bench_irq_lock.sh fixup'
```

`KBUILD_OUTPUT` and `vmtest.sh -d` select a kernel build and isolated VM directory.
Ensure that the VM directory's `latest.config` contains the intended native-JIT
configuration before invoking `vmtest.sh`. When using an out-of-tree selftests
`OUTPUT`, stage the resulting `bench` binary in the source selftests directory:
`vmtest.sh` copies that source directory into the guest.

Pool repeated runs without averaging their standard deviations:

```bash
python3 benchs/summarize_irq_lock.py /path/to/bpf-next-*.log /path/to/fixup-*.log
```

The summarizer combines sample-weighted means and within/between-run variance,
and takes the largest observed batch-averaged cost as the maximum. Input
summaries are rounded to two decimal places by the benchmark.

## Results: 2026-10-23

Baseline: unmodified `bpf-next/master` at
`b116ac6087541d3d98031e786adbe9c733cd6df3`. The fixup kernel uses the same
revision plus the IRQ-inlining patch. Kernel configurations and benchmark
binaries match. Measurements run on x86-64 KVM, with eight guest vCPUs on host
physical CPUs 16-23 of an AMD EPYC 9D64; both VMs use the command line above.

Three fresh-boot sweeps per kernel run sequentially, in paired order
baseline/fixup, fixup/baseline, baseline/fixup. Each producer count therefore
has `3 * producers * 4096` samples per kernel, each averaging 256 pairs.
All six VM runs complete successfully with no lock errors, incomplete buffers,
clocksource changes, or kernel panics. All baseline loads have zero private IRQ
instructions; all fixup loads have three.

Batch size: 256 pairs. All values are ns/pair.

| Producers | bpf-next avg / stddev / max | fixup avg / stddev / max | Average reduction |
| ---: | ---: | ---: | ---: |
| 1 | 47.16 / 6.00 / 140.64 | 24.85 / 4.22 / 80.04 | 47.3% |
| 2 | 59.03 / 19.09 / 172.18 | 41.38 / 11.33 / 127.15 | 29.9% |
| 3 | 197.06 / 55.46 / 401.24 | 181.03 / 90.99 / 453.00 | 8.1% |
| 4 | 373.15 / 27.32 / 595.21 | 371.12 / 27.55 / 621.22 | 0.5% |
| 5 | 463.25 / 22.14 / 1282.24 | 455.54 / 24.65 / 726.86 | 1.7% |
| 6 | 544.82 / 24.12 / 1328.91 | 546.74 / 29.21 / 1308.41 | -0.4% |
| 7 | 633.64 / 24.77 / 1207.20 | 635.99 / 29.20 / 1099.34 | -0.4% |
| 8 | 722.44 / 29.43 / 1177.23 | 727.17 / 33.55 / 1233.34 | -0.7% |

The uncontended average falls from 47.16 to 24.85 ns/pair (47.3%). Two- and
three-producer measurements vary substantially across runs. With four or more
producers, shared-lock contention dominates, and differences in average cost
are small relative to the observed variation; these measurements do not
establish a consistent improvement under high contention.

Raw per-run summaries are saved in [irq_lock_results.csv](irq_lock_results.csv).
Full boot logs are `/tmp/irq-lock-bench/bpf-next-round{1,2,3}.log` and
`/tmp/irq-lock-bench/fixup-round{1,2,3}.log`. Earlier exploratory runs that switched
clocksource or panicked are excluded in their entirety. Fresh boots and
`idle=poll` avoid the idle/watchdog failure observed in an exploratory upstream
VM.


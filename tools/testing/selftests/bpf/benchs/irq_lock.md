# IRQ save/restore shared-lock benchmark

The `irq-lock` benchmark measures an empty critical section protected by
one shared `bpf_res_spin_lock`. Each iteration saves local IRQ state,
acquires and releases the shared lock, and restores IRQ state.

One to eight producer threads run the same TC BPF program through
`BPF_PROG_TEST_RUN`, pinned to separate CPUs. Each producer has a separate
sample buffer. Producers keep running after their own buffers fill so that
contention remains present until every producer has collected its samples.

## Running

Build `bench` with the normal BPF selftests build settings. From
`tools/testing/selftests/bpf`, run a single-producer measurement in the VM:

```bash
./vmtest.sh -- ./bench -q -a -p 1 -w 1 -d 60 irq-lock
```

Set `-p` to the desired producer count, up to eight. The guest must have at
least one CPU per producer. To sweep all eight counts three times, use an
eight-vCPU guest:

```bash
./vmtest.sh -- bash ./benchs/run_bench_irq_lock.sh fixup
```

The sweep supports `IRQ_LOCK_RUNS`, `IRQ_LOCK_BATCH` and
`IRQ_LOCK_SAMPLES`; their defaults are 3, 256 and 4096. The runner restores
the original JIT-hardening sysctl after temporarily disabling constant
blinding.

## Timing and output

Two `bpf_ktime_get_ns()` calls bracket each batch of lock/unlock pairs.
The reported units are ns per complete pair, including IRQ save/restore
and loop overhead. Syscall, packet setup and sample-publication costs are
outside the timed batch.

By default, each producer collects 4096 samples, each averaging 256 pairs.
`avg_ns`, `stddev_ns` and `max_ns` describe these batch-averaged costs,
not individual worst-case acquisitions. No samples are discarded.
Use `--irq-lock-batch 1` for individual-pair measurements; two timestamp
helper calls then contribute to every pair. The options
`--irq-lock-batch` and `--irq-lock-samples` accept 1-1024 pairs and
2-4096 samples respectively.

A lock failure, failed BPF invocation or incomplete sample buffer is an
error. There are no latency thresholds or performance pass/fail assertions.

## Comparing kernels

Use identical kernel configurations, benchmark binaries, CPU placement
and VM settings. Run the VMs sequentially. On x86-64, IRQ inlining requires
`CONFIG_TRACE_IRQFLAGS=n` and `CONFIG_PARAVIRT_XXL=n`; on arm64, it
requires `CONFIG_TRACE_IRQFLAGS=n` and
`CONFIG_ARM64_DEBUG_PRIORITY_MASKING=n`.

The `irq_insns` field counts translated kernel-private IRQ instructions:
zero for the kfunc fallback or three for inlining (one save and two restore
sites, including the lock-error branch). Set `IRQ_LOCK_EXPECT_INLINE` to
0 or 1 to have the sweep validate which implementation it loaded:

```bash
./vmtest.sh -- bash -c \
  'IRQ_LOCK_EXPECT_INLINE=1 bash ./benchs/run_bench_irq_lock.sh fixup'
```

For the baseline, use the baseline kernel and
`IRQ_LOCK_EXPECT_INLINE=0`, with label `bpf-next`.

Pool repeated VM logs with:

```bash
python3 benchs/summarize_irq_lock.py baseline-*.log fixup-*.log
```

Use `--baseline` and `--fixup` to override the default `bpf-next` and
`fixup` labels. The summarizer requires matching sample counts, equal
numbers of runs and a common batch size. It combines sample-weighted
means and within/between-run variance, and reports the largest observed
batch-averaged cost as the maximum.

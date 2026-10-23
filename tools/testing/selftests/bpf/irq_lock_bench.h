#ifndef __IRQ_LOCK_BENCH_H
#define __IRQ_LOCK_BENCH_H

#define IRQ_LOCK_MAX_PRODUCERS 8
#define IRQ_LOCK_MAX_SAMPLES 4096
#define IRQ_LOCK_MAX_BATCH 1024
#define IRQ_LOCK_DEFAULT_BATCH 256

struct irq_lock_samples {
	__u32 count;
	__u32 errors;
	__u64 elapsed_ns[IRQ_LOCK_MAX_SAMPLES];
} __attribute__((aligned(128)));

#endif

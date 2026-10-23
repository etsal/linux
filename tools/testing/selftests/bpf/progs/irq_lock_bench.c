#include <vmlinux.h>
#include <bpf/bpf_helpers.h>
#include "../irq_lock_bench.h"

extern void bpf_local_irq_save(unsigned long *) __ksym;
extern void bpf_local_irq_restore(unsigned long *) __ksym;
extern int bpf_res_spin_lock(struct bpf_res_spin_lock *) __ksym;
extern void bpf_res_spin_unlock(struct bpf_res_spin_lock *) __ksym;

const volatile __u32 batch_iters = IRQ_LOCK_DEFAULT_BATCH;
const volatile __u32 sample_limit = IRQ_LOCK_MAX_SAMPLES;
volatile __u32 timing_enabled;
struct irq_lock_samples samples[IRQ_LOCK_MAX_PRODUCERS];
struct bpf_res_spin_lock shared_lock __hidden SEC(".data.lock");

SEC("tc")
int irq_lock_benchmark(struct __sk_buff *ctx)
{
	struct irq_lock_samples *producer;
	unsigned long flags;
	__u64 start, elapsed;
	__u32 producer_idx = ctx->mark, sample_idx, iter;

	if (producer_idx >= IRQ_LOCK_MAX_PRODUCERS)
		return 1;
	producer = &samples[producer_idx];
	start = bpf_ktime_get_ns();
	for (iter = 0; iter < batch_iters && iter < IRQ_LOCK_MAX_BATCH; iter++) {
		bpf_local_irq_save(&flags);
		if (bpf_res_spin_lock(&shared_lock)) {
			bpf_local_irq_restore(&flags);
			producer->errors++;
			return 2;
		}
		bpf_res_spin_unlock(&shared_lock);
		bpf_local_irq_restore(&flags);
	}
	elapsed = bpf_ktime_get_ns() - start;
	if (!timing_enabled)
		return 0;
	sample_idx = producer->count;
	if (sample_idx >= sample_limit || sample_idx >= IRQ_LOCK_MAX_SAMPLES)
		return 0;
	producer->elapsed_ns[sample_idx] = elapsed;
	__sync_fetch_and_add(&producer->count, 1);
	return 0;
}

char _license[] SEC("license") = "GPL";

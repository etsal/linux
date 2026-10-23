#include <argp.h>
#include "bench.h"
#include "network_helpers.h"
#include "irq_lock_bench.h"
#include "irq_lock_bench.skel.h"

static const struct ipv4_packet packet = {
	.eth.h_proto = __bpf_constant_htons(ETH_P_IP),
	.iph.version = 4,
	.iph.ihl = 5,
	.iph.protocol = IPPROTO_TCP,
	.iph.tot_len = __bpf_constant_htons(sizeof(struct iphdr) + sizeof(struct tcphdr)),
	.tcp.doff = 5,
};

static struct {
	struct irq_lock_bench *skel;
	int prog_fd;
	int irq_insns;
	int ticks;
} ctx;

static struct {
	__u32 batch_iters;
	__u32 samples;
} args = {
	.batch_iters = IRQ_LOCK_DEFAULT_BATCH,
	.samples = IRQ_LOCK_MAX_SAMPLES,
};

enum {
	ARG_IRQ_LOCK_BATCH = 13000,
	ARG_IRQ_LOCK_SAMPLES,
};

static const struct argp_option opts[] = {
	{ "irq-lock-batch", ARG_IRQ_LOCK_BATCH, "ITERS", 0,
	  "Lock/unlock pairs per timing sample (1-1024)" },
	{ "irq-lock-samples", ARG_IRQ_LOCK_SAMPLES, "SAMPLES", 0,
	  "Timing samples per producer (2-4096)" },
	{},
};

static error_t parse_arg(int key, char *arg, struct argp_state *state)
{
	unsigned long value, limit;
	char *end;

	if (key != ARG_IRQ_LOCK_BATCH && key != ARG_IRQ_LOCK_SAMPLES)
		return ARGP_ERR_UNKNOWN;
	limit = key == ARG_IRQ_LOCK_BATCH ? IRQ_LOCK_MAX_BATCH : IRQ_LOCK_MAX_SAMPLES;
	errno = 0;
	value = strtoul(arg, &end, 10);
	if (errno || !*arg || *end || value > limit ||
	    value < (key == ARG_IRQ_LOCK_BATCH ? 1 : 2))
		argp_error(state, "invalid IRQ lock benchmark argument: %s", arg);
	if (key == ARG_IRQ_LOCK_BATCH)
		args.batch_iters = value;
	else
		args.samples = value;
	return 0;
}

const struct argp bench_irq_lock_argp = {
	.options = opts,
	.parser = parse_arg,
};

static void validate(void)
{
	if (env.consumer_cnt || env.producer_cnt < 1 ||
	    env.producer_cnt > IRQ_LOCK_MAX_PRODUCERS || env.producer_cnt > env.nr_cpus) {
		fprintf(stderr, "irq-lock needs 1-8 producers, one CPU each, and no consumers\n");
		exit(1);
	}
	env.affinity = true;
}

static int count_irq_insns(int prog_fd)
{
	struct bpf_prog_info info = {};
	__u32 info_len = sizeof(info), xlated_len;
	struct bpf_insn *insns;
	int count = 0;

	if (bpf_prog_get_info_by_fd(prog_fd, &info, &info_len))
		goto error;
	xlated_len = info.xlated_prog_len;
	insns = malloc(xlated_len);
	if (!insns)
		goto error;
	memset(&info, 0, sizeof(info));
	info.xlated_prog_len = xlated_len;
	info.xlated_prog_insns = (__u64)(unsigned long)insns;
	if (bpf_prog_get_info_by_fd(prog_fd, &info, &info_len)) {
		free(insns);
		goto error;
	}
	for (size_t insn_idx = 0; insn_idx < info.xlated_prog_len / sizeof(*insns); insn_idx++)
		if (insns[insn_idx].code == (BPF_ST | 0xe0))
			count++;
	free(insns);
	return count;
error:
	perror("read translated IRQ lock program");
	exit(1);
}

static void setup(void)
{
	setup_libbpf();
	ctx.skel = irq_lock_bench__open();
	if (!ctx.skel)
		goto error;
	ctx.skel->rodata->batch_iters = args.batch_iters;
	ctx.skel->rodata->sample_limit = args.samples;
	if (irq_lock_bench__load(ctx.skel))
		goto error;
	ctx.prog_fd = bpf_program__fd(ctx.skel->progs.irq_lock_benchmark);
	ctx.irq_insns = count_irq_insns(ctx.prog_fd);
	return;
error:
	fprintf(stderr, "failed to open/load IRQ lock benchmark\n");
	irq_lock_bench__destroy(ctx.skel);
	exit(1);
}

static void *producer(void *input)
{
	struct __sk_buff skb = { .mark = (long)input };
	int err;

	LIBBPF_OPTS(bpf_test_run_opts, opts,
		.data_in = &packet,
		.data_size_in = sizeof(packet),
		.ctx_in = &skb,
		.ctx_size_in = sizeof(skb),
	);
	while (true) {
		err = bpf_prog_test_run_opts(ctx.prog_fd, &opts);
		if (err || opts.retval) {
			fprintf(stderr, "IRQ lock batch failed: err=%d retval=%u\n", err, opts.retval);
			exit(1);
		}
	}
	return NULL;
}

static void measure(struct bench_res *res)
{
	if (++ctx.ticks <= env.warmup_sec) {
		if (ctx.ticks == env.warmup_sec)
			__atomic_store_n(&ctx.skel->bss->timing_enabled, 1, __ATOMIC_RELEASE);
		return;
	}
	for (int producer_idx = 0; producer_idx < env.producer_cnt; producer_idx++)
		if (__atomic_load_n(&ctx.skel->bss->samples[producer_idx].count,
				    __ATOMIC_ACQUIRE) < args.samples)
			return;
	__atomic_store_n(&ctx.skel->bss->timing_enabled, 0, __ATOMIC_RELEASE);
	bench_force_done();
}

static void report_final(struct bench_res res[], int res_cnt)
{
	double mean = 0, variance = 0, max_ns = 0;
	unsigned long count = 0;

	for (int producer_idx = 0; producer_idx < env.producer_cnt; producer_idx++) {
		struct irq_lock_samples *samples = &ctx.skel->bss->samples[producer_idx];

		if (__atomic_load_n(&samples->count, __ATOMIC_ACQUIRE) != args.samples ||
		    samples->errors) {
			fprintf(stderr, "incomplete or failed producer %d: samples=%u errors=%u\n",
				producer_idx, samples->count, samples->errors);
			exit(1);
		}
		for (__u32 sample_idx = 0; sample_idx < args.samples; sample_idx++) {
			double ns = (double)samples->elapsed_ns[sample_idx] / args.batch_iters;
			double delta = ns - mean;

			mean += delta / ++count;
			variance += delta * (ns - mean);
			if (ns > max_ns)
				max_ns = ns;
		}
	}
	printf("RESULT producers=%d batch=%u samples=%lu irq_insns=%d "
	       "avg_ns=%.2f stddev_ns=%.2f max_ns=%.2f\n",
	       env.producer_cnt, args.batch_iters, count, ctx.irq_insns,
	       mean, sqrt(variance / (count - 1)), max_ns);
}

const struct bench bench_irq_lock = {
	.name = "irq-lock",
	.argp = &bench_irq_lock_argp,
	.validate = validate,
	.setup = setup,
	.producer_thread = producer,
	.measure = measure,
	.report_final = report_final,
};

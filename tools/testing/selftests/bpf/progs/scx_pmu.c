#include <vmlinux.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include <asm-generic/errno.h>

char _license[] SEC("license") = "GPL";

#define SCX_MAX_PMU_COUNTERS (2)

struct scx_pmu_counters {
	u64 start[SCX_MAX_PMU_COUNTERS];
	u64 agg[SCX_MAX_PMU_COUNTERS];
	bool switched;
	u32 gen;
};

struct {
	__uint(type, BPF_MAP_TYPE_TASK_STORAGE);
	__uint(map_flags, BPF_F_NO_PREALLOC);
	__type(key, u32);
	__type(value, struct scx_pmu_counters);
} scx_pmu_tasks SEC(".maps");

/* Constant-index array access to satisfy the BPF verifier (no dynamic <<=) */
#define SCX_EVENT_IDX_GET(arr, idx) ({		\
	typeof((arr)[0]) __v;			\
	switch (idx) {				\
	case 0: __v = (arr)[0]; break;		\
	case 1: __v = (arr)[1]; break;		\
	}					\
	__v;					\
})

#define SCX_EVENT_IDX_SET(arr, idx, val)	\
do {						\
	switch (idx) {				\
	case 0: (arr)[0] = (val); break;	\
	case 1: (arr)[1] = (val); break;	\
	}					\
} while (0)

u64 scx_event_idx[SCX_MAX_PMU_COUNTERS];

static
int scx_pmu_event_to_idx(u64 event)
{
	int i;

	bpf_for(i, 0, SCX_MAX_PMU_COUNTERS) {
		if (scx_event_idx[i] == event)
			break;
	}

	/* i == SCX_MAX_PMU_COUNTERS means NOT_FOUND. */
	return i;
}


SEC("syscall")
int scx_pmu_read(struct task_struct __arg_trusted *p, u64 event, u64 *value, bool clear)
{
	struct scx_pmu_counters *cntrs;
	int idx;

	idx = scx_pmu_event_to_idx(event);
	if (idx == SCX_MAX_PMU_COUNTERS)
		return -EINVAL;

	cntrs = bpf_task_storage_get(&scx_pmu_tasks, p, 0, 0);
	if (!cntrs)
		return -ENOENT;

	if (unlikely(!value))
		return -EINVAL;

	if (unlikely(idx < 0 || idx >= SCX_MAX_PMU_COUNTERS))
		return -EINVAL;

	*value = SCX_EVENT_IDX_GET(cntrs->agg, idx);
	if (clear)
		SCX_EVENT_IDX_SET(cntrs->agg, idx, 0);

	return 0;
}


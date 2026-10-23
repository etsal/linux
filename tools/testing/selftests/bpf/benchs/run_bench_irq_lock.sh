#!/bin/bash

set -euo pipefail

release=$(</proc/sys/kernel/osrelease)
label=${1:-$release}
runs=${IRQ_LOCK_RUNS:-3}
batch=${IRQ_LOCK_BATCH:-256}
samples=${IRQ_LOCK_SAMPLES:-4096}
old_harden=$(</proc/sys/net/core/bpf_jit_harden)
trap 'echo "$old_harden" > /proc/sys/net/core/bpf_jit_harden' EXIT
cpus=0
while IFS=: read -r field value; do
	if [[ $field == processor* ]]; then
		((cpus += 1))
	fi
done < /proc/cpuinfo

echo "KERNEL label=$label release=$release cpus=$cpus"
echo 0 > /proc/sys/net/core/bpf_jit_harden
echo "JIT enable=$(</proc/sys/net/core/bpf_jit_enable) harden=$(</proc/sys/net/core/bpf_jit_harden)"

for ((run = 1; run <= runs; run++)); do
	for ((producers = 1; producers <= 8; producers++)); do
		result=$(./bench -q -a -w 1 -d 60 -p "$producers" \
			--irq-lock-batch "$batch" --irq-lock-samples "$samples" irq-lock)
		if [[ -n ${IRQ_LOCK_EXPECT_INLINE:-} ]]; then
			if [[ $IRQ_LOCK_EXPECT_INLINE == 1 && $result != *"irq_insns=3 "* ]] ||
			   [[ $IRQ_LOCK_EXPECT_INLINE == 0 && $result != *"irq_insns=0 "* ]]; then
				echo "Unexpected IRQ fixup selection: $result" >&2
				exit 1
			fi
		fi
		printf 'RUN label=%s run=%d %s\n' "$label" "$run" "$result"
	done
done

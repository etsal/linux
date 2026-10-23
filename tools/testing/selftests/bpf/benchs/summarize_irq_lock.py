#!/usr/bin/env python3

import argparse
import math
import re
from pathlib import Path


def aggregate(samples):
    count = sum(sample["samples"] for sample in samples)
    mean = math.fsum(sample["samples"] * sample["avg_ns"] for sample in samples) / count
    variance = math.fsum(
        (sample["samples"] - 1) * sample["stddev_ns"] ** 2
        + sample["samples"] * (sample["avg_ns"] - mean) ** 2
        for sample in samples
    ) / (count - 1)
    maximum = max(sample["max_ns"] for sample in samples)
    return mean, math.sqrt(variance), maximum


def main():
    parser = argparse.ArgumentParser(description="Pool IRQ lock benchmark VM results")
    parser.add_argument("logs", nargs="+", type=Path)
    parser.add_argument("--baseline", default="bpf-next")
    parser.add_argument("--fixup", default="fixup")
    args = parser.parse_args()
    results = {}
    batches = set()
    pattern = re.compile(r"^RUN label=(\S+) run=\d+ RESULT (.*)$", re.MULTILINE)

    for log in args.logs:
        for match in pattern.finditer(log.read_text()):
            label = match.group(1)
            if label not in (args.baseline, args.fixup):
                continue
            fields = dict(field.split("=", 1) for field in match.group(2).split())
            producers = int(fields["producers"])
            expected_insns = 0 if label == args.baseline else 3
            if int(fields["irq_insns"]) != expected_insns:
                parser.error(f"unexpected IRQ fixup selection in {log}")
            batches.add(int(fields["batch"]))
            sample = {name: float(fields[name]) for name in ("avg_ns", "stddev_ns", "max_ns")}
            sample["samples"] = int(fields["samples"])
            if sample["samples"] < 2:
                parser.error(f"insufficient samples in {log}")
            results.setdefault((label, producers), []).append(sample)

    if len(batches) != 1:
        parser.error("need one consistent batch size across all results")
    for producers in range(1, 9):
        baseline = results.get((args.baseline, producers), [])
        fixup = results.get((args.fixup, producers), [])
        if not baseline or len(baseline) != len(fixup):
            parser.error(f"need equal numbers of runs for {producers} producers")
        if sorted(sample["samples"] for sample in baseline) != sorted(
            sample["samples"] for sample in fixup
        ):
            parser.error(f"sample counts differ for {producers} producers")

    print(f"Batch size: {next(iter(batches))} pairs. All values are ns/pair.")
    print()
    print(f"| Producers | {args.baseline} avg / stddev / max | "
          f"{args.fixup} avg / stddev / max | Average reduction |")
    print("| ---: | ---: | ---: | ---: |")
    for producers in range(1, 9):
        baseline = aggregate(results[(args.baseline, producers)])
        fixup = aggregate(results[(args.fixup, producers)])
        reduction = (1 - fixup[0] / baseline[0]) * 100
        baseline_text = " / ".join(f"{value:.2f}" for value in baseline)
        fixup_text = " / ".join(f"{value:.2f}" for value in fixup)
        print(f"| {producers} | {baseline_text} | {fixup_text} | {reduction:.1f}% |")


if __name__ == "__main__":
    main()

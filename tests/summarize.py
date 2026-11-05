#!/usr/bin/env python3
"""Print a side-by-side median summary of complete benchmark CSV files."""

import argparse
import csv
import json
import statistics
import sys
from collections import defaultdict
from pathlib import Path


def summarize(paths):
    groups = defaultdict(list)
    required = {"write_open", "write", "fsync", "write_close", "read_open",
                "read_first", "read_repeat", "random_read", "read_close"}
    workload = None
    for path in paths:
        metadata_path = Path(path).with_suffix(".json")
        if not metadata_path.exists():
            raise ValueError("Missing run metadata: " + str(metadata_path))
        metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
        if metadata.get("status") != "complete":
            raise ValueError("Run did not complete successfully: " + str(path))
        settings = tuple(metadata[key] for key in (
            "size_mib", "sequential_block_bytes", "random_block_bytes", "random_reads", "random_seed"))
        if workload is not None and settings != workload:
            raise ValueError("Cannot compare different workloads")
        workload = settings
        trials = defaultdict(dict)
        with open(path, newline="", encoding="utf-8") as stream:
            for row in csv.DictReader(stream):
                phase = row["phase"]
                trial = trials[(row["label"], row["trial"])]
                if phase in trial:
                    raise ValueError("Duplicate phase in " + str(path))
                trial[phase] = (float(row["seconds"]), int(row["bytes"]))
        if len(trials) != metadata["repeats"]:
            raise ValueError("Trial count does not match metadata: " + str(path))
        for (label, _), phases in trials.items():
            if set(phases) != required:
                raise ValueError("Incomplete trial in " + str(path))
            def rate(phase):
                seconds, count = phases[phase]
                return count / 1048576 / seconds
            first_total = phases["read_open"][0] + phases["read_first"][0]
            groups[label].append([
                phases["read_open"][0] * 1000,
                phases["read_first"][1] / 1048576 / first_total,
                rate("read_repeat"), rate("random_read"), rate("write"),
                phases["fsync"][0] * 1000,
            ])
    print("Rates: MiB/s. Open and fsync: milliseconds. Values: medians across trials.")
    print("{:<24} {:>3} {:>10} {:>12} {:>12} {:>12} {:>12} {:>10}".format(
        "Label", "N", "Open ms", "Open+read", "Repeat read", "Random read", "Write", "fsync ms"))
    for label, values in groups.items():
        medians = [statistics.median(column) for column in zip(*values)]
        print("{:<24} {:>3} ".format(label, len(values)) + " ".join("{:>12.3f}".format(v) for v in medians))
    print("Write excludes fsync/release; inspect the raw phases before making end-to-end claims.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("csv_files", nargs="+")
    args = parser.parse_args()
    try:
        summarize(args.csv_files)
    except (OSError, ValueError, KeyError, ZeroDivisionError) as exc:
        print("FAIL: " + str(exc), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())

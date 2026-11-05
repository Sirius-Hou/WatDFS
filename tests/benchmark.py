#!/usr/bin/env python3
"""Measure local, remote-access, or cached WatDFS using the same workload."""

import argparse
import csv
import hashlib
import json
import os
import platform
import random
import statistics
import sys
import time
import uuid
from datetime import datetime, timezone
from pathlib import Path

from common import check, read_all, wait_for_content, write_all


def run(root, output, label, size_mib=16, repeats=5, random_reads=1024,
        server_root=None, cache_interval=None, server_host="local", build_info="not recorded"):
    root, output = Path(root), Path(output)
    check(root.is_dir(), "Benchmark root must already exist")
    check(size_mib > 0 and repeats > 0 and random_reads > 0, "Workload sizes must be positive")
    output.mkdir(parents=True, exist_ok=True)
    run_id = "bench-" + uuid.uuid4().hex[:12]
    rows = []
    block_tail = (bytes(range(256)) * 16)[:4088]
    data = b"".join(index.to_bytes(8, "little") + block_tail
                    for index in range(size_mib * 256))
    digest = hashlib.sha256(data).digest()
    rng = random.Random(454)
    offsets = [rng.randrange(0, len(data) // 4096) * 4096 for _ in range(random_reads)]
    metadata = {
        "run_id": run_id, "label": label, "utc": datetime.now(timezone.utc).isoformat(),
        "client_host": platform.node(), "server_host": server_host,
        "platform": platform.platform(), "python": platform.python_version(),
        "root": str(root.resolve()), "verification_root": str(server_root or root),
        "size_mib": size_mib, "repeats": repeats, "sequential_block_bytes": 65536,
        "random_block_bytes": 4096, "random_reads": random_reads, "random_seed": 454,
        "cache_interval_sec": cache_interval, "build_info": build_info,
        "status": "running",
        "notes": "No OS page-cache drop. Read phases share one open handle. Close is not a durability barrier.",
    }
    metadata_path = output / (run_id + ".json")
    metadata_path.write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
    csv_path = output / (run_id + ".csv")
    with csv_path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=["label", "trial", "phase", "seconds", "bytes", "mib_per_s"])
        writer.writeheader()

        def timed(trial, phase, count, operation):
            start = time.perf_counter()
            result = operation()
            seconds = time.perf_counter() - start
            row = dict(label=label, trial=trial, phase=phase, seconds=seconds,
                       bytes=count, mib_per_s=count / 1048576 / seconds if count else "")
            writer.writerow(row)
            stream.flush()
            rows.append(row)
            return result

        for trial in range(1, repeats + 1):
            # Unique names avoid stale data, overwrite, and release/open races.
            path = root / (run_id + "-" + str(trial) + ".bin")
            fd = timed(trial, "write_open", 0,
                       lambda: os.open(path, os.O_CREAT | os.O_EXCL | os.O_RDWR, 0o600))
            try:
                def write_chunks():
                    for offset in range(0, len(data), 65536):
                        write_all(fd, memoryview(data)[offset:offset + 65536])
                timed(trial, "write", len(data), write_chunks)
                timed(trial, "fsync", 0, lambda: os.fsync(fd))
            finally:
                timed(trial, "write_close", 0, lambda: os.close(fd))
            verify_path = Path(server_root or root) / path.name
            wait_for_content(verify_path, data, timeout=120)
            if server_root is not None:
                # A matching backing file after fsync alone does not mean the
                # later release/upload has finished. A client reopen traverses
                # the transfer lock and verifies the completed server copy.
                wait_for_content(path, data, timeout=120)
            # Verification and setup are excluded from read timing. These are
            # application-cache measurements, not cold disk measurements.
            fd = timed(trial, "read_open", 0, lambda: os.open(path, os.O_RDONLY))
            try:
                for phase in ("read_first", "read_repeat"):
                    os.lseek(fd, 0, os.SEEK_SET)
                    result = timed(trial, phase, len(data), lambda: read_all(fd))
                    check(hashlib.sha256(result).digest() == digest, "Sequential data mismatch")
                blocks = timed(trial, "random_read", random_reads * 4096,
                               lambda: [os.pread(fd, 4096, offset) for offset in offsets])
                check(all(block == data[offset:offset + 4096] for block, offset in zip(blocks, offsets)),
                      "Random data mismatch")
            finally:
                timed(trial, "read_close", 0, lambda: os.close(fd))
            print("PASS " + label + " trial " + str(trial) + "/" + str(repeats), flush=True)
    metadata["status"] = "complete"
    metadata_path.write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
    print("\n" + label + " (median of " + str(repeats) + " trials)")
    print("{:<16} {:>12} {:>14}".format("Phase", "Seconds", "MiB/s"))
    for phase in dict.fromkeys(row["phase"] for row in rows):
        group = [row for row in rows if row["phase"] == phase]
        seconds = statistics.median(row["seconds"] for row in group)
        rates = [row["mib_per_s"] for row in group if row["mib_per_s"] != ""]
        rate = "{:.2f}".format(statistics.median(rates)) if rates else "-"
        print("{:<16} {:>12.6f} {:>14}".format(phase, seconds, rate))
    print("CSV: " + str(csv_path))
    print("Test data retained: " + run_id + "-*.bin")
    return csv_path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--output", type=Path, default=Path("results"))
    parser.add_argument("--label", required=True)
    parser.add_argument("--size-mib", type=int, default=16)
    parser.add_argument("--repeats", type=int, default=5)
    parser.add_argument("--random-reads", type=int, default=1024)
    parser.add_argument("--server-root", type=Path)
    parser.add_argument("--cache-interval", type=int)
    parser.add_argument("--server-host", default="local")
    parser.add_argument("--build-info", default="not recorded")
    args = parser.parse_args()
    if min(args.size_mib, args.repeats, args.random_reads) <= 0:
        parser.error("workload sizes must be positive")
    if args.cache_interval is not None and args.cache_interval < 0:
        parser.error("--cache-interval must be nonnegative")
    try:
        run(args.root, args.output, args.label, args.size_mib, args.repeats,
            args.random_reads, args.server_root, args.cache_interval,
            args.server_host, args.build_info)
    except (OSError, AssertionError) as exc:
        print("FAIL: " + str(exc), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())

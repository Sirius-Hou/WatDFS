# Measuring WatDFS

This procedure compares local file I/O, the remote-access baseline, and the final cached implementation. It separates file-open/download cost from repeated access to an already-open file. Run the [functional checks](RUNNING.md) first.

No school-server performance result is claimed yet. A successful small tooling run is evidence that the measurement code works, not a representative performance evaluation.

## Workload and interpretation

Defaults: 16 MiB per file, 64 KiB sequential calls, 4 KiB random reads, 1,024 random reads with seed 454, and five trials per configuration. Every trial creates a unique file. Payload blocks encode their position so correctness checks detect incorrect offsets. Data validation is outside the timed phases.

| Phase | What is measured |
| --- | --- |
| `write_open` | Creating and opening a new empty file |
| `write` | Sequential application writes; may include operation-triggered writeback if the cache interval expires |
| `fsync` | Explicit synchronization; cached WatDFS uploads the whole file and requests server `fsync` |
| `write_close` | Application close latency, **not** completion of asynchronous FUSE release |
| `read_open` | Opening the populated file; includes a whole-file download in cached WatDFS |
| `read_first` | First sequential pass on that open handle |
| `read_repeat` | Second sequential pass on the **same** handle |
| `random_read` | Fixed-seed positional reads on that same handle |
| `read_close` | Application close latency |

The summary also reports `Open+read`: file size divided by the sum of `read_open` and `read_first` for each trial. This prevents fast cache reads from hiding the initial download cost.

Important controls:

- Use the same hosts, binaries, file sizes, and workload settings for every comparable run. Do not run multiple benchmarks simultaneously.
- Cached intervals 0 and 5 are separate configurations. Interval 0 still uses whole-file caching; it is **not** the P1 baseline.
- Keep `-s -f -o direct_io` for all WatDFS clients. The local path does not traverse FUSE and is a reference, not an isolated measurement of cache benefits.
- Neither the OS page cache nor backing storage caches are dropped. Setup writes and verification can warm them. These are application-level filesystem measurements, not cold-disk benchmarks.
- A cached writable close uploads again even after `fsync`. The script waits for verified contents and reopens through the client outside timing; the close number alone does not capture that transfer's full cost.
- No background cache timer is assumed. A five-second interval may expire during a long trial; report the actual interval rather than claiming every repeated read is a cache hit.
- The provided RPC/runtime archives may log internally. Keep the same logging and build configuration across runs; redirecting console output does not remove the cost of library file logging.
- With interval 0, small writes can repeatedly upload a growing file. The full default workload can take a long time; do not mistake that expected cost for a hung process. Start with the quick validation below.

## Capture the environment

On the machine running the test application:

```bash
export REPO="$HOME/WatDFS"
cd "$REPO"
mkdir -p results/school
{
  date -u
  hostname -f
  uname -a
  g++ --version
  pkg-config --modversion fuse
  python3 --version
  git rev-parse HEAD
  make -Bn all
  make -C baseline -Bn all
} > results/school/environment.txt
export BUILD_INFO="Default Makefiles: -g -Wall -std=c++1y -MMD -pthread; $(g++ --version | head -n 1); FUSE $(pkg-config --modversion fuse)"
set -o pipefail
```

Do not change compiler optimizations between variants. If you intentionally use different build flags later, record the exact commands and rerun every comparison. The initial comparison uses the preserved default build configuration.

## Quick tooling check

With the final server and first client running as in the runbook:

```bash
export RUN="/tmp/$USER/watdfs-demo-01"
python3 -u tests/benchmark.py \
  --root "$RUN/mount-a" --server-root "$RUN/server" \
  --label cached-5-quick --cache-interval 5 \
  --size-mib 1 --repeats 1 --random-reads 8 \
  --server-host "$(hostname -f)" --build-info "$BUILD_INFO" \
  --output results/quick
```

This is intentionally excluded from the full comparison directory. A configuration label and `--cache-interval` record metadata; neither changes a running client.

## 1. Local filesystem

```bash
mkdir -p "/tmp/$USER/watdfs-local-01"
python3 -u tests/benchmark.py \
  --root "/tmp/$USER/watdfs-local-01" --label local \
  --server-host "$(hostname -f)" --build-info "$BUILD_INFO" \
  --output results/school | tee results/school/local.txt
```

## 2. Remote-access baseline

Start a baseline server/client pair using the runbook, with `RUN=/tmp/$USER/watdfs-baseline-01`. In the test terminal:

```bash
export RUN="/tmp/$USER/watdfs-baseline-01"
python3 -u tests/benchmark.py \
  --root "$RUN/mount-a" --server-root "$RUN/server" \
  --label remote-access --server-host "$(hostname -f)" \
  --build-info "$BUILD_INFO" --output results/school \
  | tee results/school/remote-access.txt
```

Stop and unmount that pair after the run. The final client requires its own final server.

## 3. Final implementation, five-second interval

Start the final server/client pair with `CACHE_INTERVAL_SEC=5`, using the runbook directories. A second client is not required for performance measurement; stop other test applications first.

```bash
export RUN="/tmp/$USER/watdfs-demo-01"
python3 -u tests/benchmark.py \
  --root "$RUN/mount-a" --server-root "$RUN/server" \
  --label cached-5 --cache-interval 5 \
  --server-host "$(hostname -f)" --build-info "$BUILD_INFO" \
  --output results/school | tee results/school/cached-5.txt
```

## 4. Final implementation, zero-second interval

After the previous benchmark exits, unmount the first client:

```bash
fusermount -u "$RUN/mount-a"
```

Wait for that foreground client to exit. Keep the server running. In the client terminal, retain the correct server address/port and start a new client with a fresh cache:

```bash
export CACHE_INTERVAL_SEC=0
mkdir -p "$RUN/cache-zero" "$RUN/mount-zero" "$RUN/client-zero-log"
cd "$RUN/client-zero-log"
"$REPO/watdfs_client" -s -f -o direct_io "$RUN/cache-zero" "$RUN/mount-zero"
```

Back in the test terminal:

```bash
cd "$REPO"
python3 -u tests/benchmark.py \
  --root "$RUN/mount-zero" --server-root "$RUN/server" \
  --label cached-0 --cache-interval 0 \
  --server-host "$(hostname -f)" --build-info "$BUILD_INFO" \
  --output results/school | tee results/school/cached-0.txt
```

After the run finishes, unmount this client with `fusermount -u "$RUN/mount-zero"`; unmount any remaining second client before stopping the server.

If you need a shorter exploratory run, set the **same** `--size-mib`, `--repeats`, and `--random-reads` on all four configurations and put them in a different output directory. Do not combine different workloads under the same comparison label.

## Comparison and screenshots

```bash
python3 tests/summarize.py results/school/bench-*.csv \
  | tee results/school/comparison.txt
```

The summary accepts only complete trials. Do not mix quick checks, different workloads, or interrupted runs into this glob. The CSV files retain each phase of each trial, while the matching JSON files record host and workload metadata. A failure exits nonzero; fix the failure before treating a partial CSV as evidence.

Good screenshots are:

1. Functional output ending in `PASS all requested checks`, including the multi-client check.
2. The comparison table together with the recorded file size, cache intervals, and host arrangement.
3. Server/client terminals showing the running system, if useful for the demonstration.

Copy actual results to a dated directory under `docs/results/` and screenshots under `docs/images/` when ready, then link them from the README. The top-level `results/` directory is ignored to prevent accidental publication of every exploratory run; `docs/results/` can be committed normally. Review host/account details before sharing. Do not add placeholder screenshots or describe same-host figures as cross-machine measurements.

From your own computer, retrieve a run using the actual school hostname:

```bash
scp -r YOUR_USERID@ACTUAL_HOST:~/WatDFS/results/school ./watdfs-school-results
```

## Cross-host performance

Use host A for the server and host B for the client, as described in the runbook. Run the benchmark on B. Omit `--server-root` because A's local `/tmp` is not visible on B; verification then reopens the file through the client, which downloads the server copy. This checks content via the filesystem interface rather than directly reading A's disk.

For example, on B with the five-second client already mounted:

```bash
python3 -u "$REPO/tests/benchmark.py" \
  --root "$RUN/mount-a" --label cached-5-cross-host \
  --cache-interval 5 --server-host ACTUAL_SERVER_HOST \
  --build-info "$BUILD_INFO" --output "$REPO/results/cross-host"
```

Repeat the baseline and zero-second configurations with the same host pair, and measure the local reference on B. Record environment details for both hosts. Use a separate results directory and labels for cross-host measurements. All benchmark files remain in the dedicated experiment directories; remove them only after unmounting and reviewing the results.

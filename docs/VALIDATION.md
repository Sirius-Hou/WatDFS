# Validation record

Repository preparation date: **2026-11-04 (America/Toronto)**.

## Historical course result

The author reports that both final submissions passed all course tests. The selected cached implementation is the author's final submission ZIP, and the baseline is the final P1 submission directory. The original grading service was not rerun during repository preparation.

## Checks performed for this repository

Environment: Ubuntu 24.04 under WSL2, Linux `6.18.54.1-microsoft-standard-WSL2`, GCC 13.3.0, FUSE 2.9.9, and Python 3.12.3. Server and clients ran on the same WSL host, with backing storage and caches under `/tmp`. The original course static archives linked and ran in this environment.

| Check | Result |
| --- | --- |
| Final C++ source and headers compared with the selected ZIP | Byte-identical |
| Baseline C++ source and headers compared with the selected P1 directory | Byte-identical |
| Shared course archives compared with the provided originals | Byte-identical |
| `make clean all` | Passed |
| `make -C baseline clean all` | Passed |
| Python tooling self-checks | Passed: short/zero writes, content mismatch, local functional workload, benchmark output, and summary rejection of incomplete results |
| Cached implementation through real FUSE mounts | Passed: create, empty file, offset I/O, large transfer, truncate, timestamp, `fsync`, and close writeback |
| Two cached clients, interval 5 seconds | Passed: concurrent reader, second-writer rejection, and reader refresh after expiry |
| Two cached clients, interval 0 seconds | Passed: concurrent reader, second-writer rejection, and refresh |
| Baseline through a real FUSE mount | Passed: basic functional checks; cache-specific checks omitted |
| Live benchmark tooling, intervals 0 and 5 plus baseline | Passed: small workloads with content verification |
| Final benchmark completion metadata and summary | Passed on a live interval-0 mount; runtime log confirmed `cache_interval:0 s` |

Makefile edits relocate the supplied library paths and explicitly link threading support. No client/server algorithm changes were made to obtain these results.

## Performance evidence boundary

Small local live runs use 1 MiB, one trial, and eight random reads to validate the benchmark against the real filesystem. They are not the planned five-trial, 16 MiB school-server evaluation and are not published as a performance claim.

Still to run on the target school environment:

- Full five-trial comparison of local I/O, the remote-access baseline, and cache intervals 0 and 5.
- Cross-machine experiments with host/network information recorded separately.
- User-captured screenshots and any desired historical grading evidence.

Use [Running](RUNNING.md) and [Benchmarking](BENCHMARKING.md) to reproduce the checks and collect those results. Passing these tests does not establish crash recovery, production reliability, or compatibility with every Linux ABI.

# WatDFS — A Simplified Distributed File System

**A FUSE-based distributed file system built in C/C++, featuring RPC communication, client-side caching, consistency checks, and concurrency control.**

WatDFS provides a transparent interface for accessing files stored on a remote server through standard Linux file operations. Applications can interact with a mounted directory using familiar system calls—such as `open`, `read`, `write`, and `close`—while WatDFS manages remote communication, local file caching, and synchronization behind the scenes.

Built as part of the University of Waterloo's **CS 454/654: Distributed Systems** course, the project evolved from an RPC-based remote-access file system into an **upload/download architecture** that performs file operations on locally cached copies while coordinating consistency across multiple clients.

## Highlights

- **Transparent file access with FUSE:** Exposes remote files through a mounted Linux directory without requiring application-level changes or kernel modifications.
- **RPC-based client–server communication:** Translates file operations into remote procedure calls, with a centralized server managing persistent files.
- **Local disk caching:** Downloads files on demand and performs subsequent operations on client-side cached copies, reducing the need to transfer file contents for every operation.
- **Timeout-based cache validation:** Uses a configurable freshness interval and file modification timestamps to determine when cached data can be reused or must be refreshed.
- **Write-back synchronization:** Applies writes locally and propagates modifications to the server on synchronization events, including `fsync` and file release.
- **Multi-client concurrency control:** Allows multiple readers while enforcing single-writer access, with safeguards against conflicting opens.
- **Atomic file transfers:** Coordinates uploads and downloads with reader–writer locking so clients do not observe partially transferred file contents.

## Architecture

```text
                        Linux Applications
                     (open / read / write / stat)
                                 |
                                 v
                         Linux VFS / FUSE
                                 |
                 +---------------v----------------+
                 |          WatDFS Client          |
                 |                                |
                 |  File operations & open state  |
                 |  Timeout-based freshness check |
                 |  Local disk cache              |
                 +----------+---------------------+
                            |
                       RPC requests
                            |
                 +----------v---------------------+
                 |          WatDFS Server          |
                 |                                |
                 |  RPC handlers                  |
                 |  Write-access coordination     |
                 |  Atomic transfer locks         |
                 |  Local filesystem operations   |
                 +----------+---------------------+
                            |
                            v
                  Persistent Server Files
```

Each client exposes a FUSE mount point to applications and maintains a separate on-disk cache. The server is the shared backing store and coordinates access across clients. The design separates application-facing filesystem operations from remote storage and consistency management.

## Core Features

### 1. Filesystem operations through FUSE

WatDFS integrates with Linux's Filesystem in Userspace (FUSE) interface, allowing standard filesystem requests to pass through a user-space client implementation. The supported operations include:

| Category | Operations |
| --- | --- |
| File lifecycle | `mknod`, `open`, `release` |
| Data access | `read`, `write`, `truncate` |
| Metadata | `getattr`, `utimensat` |
| Synchronization | `fsync` |

The client maps these operations to local system calls and RPC communication as needed, preserving a familiar filesystem interface for applications.

### 2. RPC communication and remote storage

WatDFS uses a client–server architecture to coordinate file access over RPC:

1. An application invokes an operation on a file in the FUSE mount.
2. The WatDFS client handles the request, consulting its cache or communicating with the server.
3. The server performs the required storage or coordination operation against its local filesystem.
4. Results, metadata, and file contents are returned to the client as appropriate.

The initial remote-access implementation forwarded file operations to the server. The final design builds on that foundation with an upload/download model and local execution of file operations.

### 3. Client-side caching and consistency

WatDFS maintains **file-backed caches on each client** rather than holding file contents solely in memory.

- **On open:** The client makes the file available locally, downloading it when necessary.
- **On read:** A cached copy can be reused within the configured freshness interval. After the interval expires, the client compares modification timestamps with the server and refreshes the file if it has changed.
- **On write:** Changes are applied to the local file and synchronized back to the server according to the write-back freshness policy.
- **On `fsync` or release:** Pending file modifications are flushed to the server; cached copies remain available for later access.

The cache interval is configurable through `CACHE_INTERVAL_SEC`. This is a **timeout-based consistency model**, not a claim of instantaneous consistency between independently cached clients.

### 4. Concurrent access and atomic transfers

WatDFS supports multiple clients connected to the same server while coordinating conflicting file operations:

- **Multiple readers:** Different clients may open the same file for reading concurrently.
- **Single writer:** At most one client may hold a write-mode open on a given file; competing write opens are rejected.
- **Per-client open tracking:** The client prevents duplicate opens of the same file within that client instance.
- **Atomic transfers:** Reader–writer synchronization prevents a download from observing an upload in a partially completed state.

These controls protect file contents during concurrent access and cross-client transfers, without requiring a fully general distributed transaction protocol.

## Repository Layout

The root directory contains the final cached implementation from the P2 submission. The `baseline/` directory contains the final P1 remote-access implementation for reference and comparison only. Running `make` in the repository root does not build or run the baseline; it is used only if you explicitly enter `baseline/` or invoke `make -C baseline`.

## Technology Stack

- **Languages:** C / C++-compatible systems programming interfaces
- **Platform:** Linux
- **Filesystem interface:** FUSE (Filesystem in Userspace)
- **Communication:** Remote Procedure Calls (RPC)
- **Concurrency:** Reader–writer locks and per-file access tracking
- **Storage:** Local filesystem-backed server storage and client caches
- **Build:** GNU Make

## Build and Run

> Requires the Linux/FUSE environment and the course-provided `librpc.a` and `libwatdfsmain.a` support libraries. These libraries are not included in this repository because they are course-provided binaries rather than project source. Copy them into the repository root before building.

```bash
make clean all
```

**Start the server** in one terminal:

```bash
mkdir -p /tmp/$USER/watdfs-server
./watdfs_server /tmp/$USER/watdfs-server
```

The server prints `SERVER_ADDRESS` and `SERVER_PORT`. In another terminal, set them to the values printed by your server, then start the client:

```bash
export SERVER_ADDRESS=<server-hostname>
export SERVER_PORT=<server-port>
export CACHE_INTERVAL_SEC=5

mkdir -p /tmp/$USER/watdfs-cache /tmp/$USER/watdfs-mount
./watdfs_client -s -f -o direct_io \
  /tmp/$USER/watdfs-cache \
  /tmp/$USER/watdfs-mount
```

**Use the mounted filesystem** from a third terminal:

```bash
printf 'Hello from WatDFS!\n' > /tmp/$USER/watdfs-mount/hello.txt
cat /tmp/$USER/watdfs-mount/hello.txt
stat /tmp/$USER/watdfs-mount/hello.txt
```

Multiple clients can connect to the same server using separate cache and mount directories. The filesystem focuses on file operations; arbitrary directory operations are outside the supported scope.

## Validation and Performance

The completed project passed the course's automated tests across both development stages. The tested behavior includes standard file operations, cache freshness, write synchronization, multi-client access restrictions, and atomic transfers.

For a reproducible showcase, the following measurements can be collected on the target Linux environment:

| Evaluation | What it demonstrates | Result |
| --- | --- | --- |
| End-to-end file operations | Correct file creation, reads, writes, and metadata access | Passed course tests |
| Binary data integrity | Matching checksums across client and server | To benchmark |
| Cold vs. warm read | Impact of client-side file caching | To benchmark |
| Cache expiration | Refresh of stale client data after server updates | Passed course tests |
| Concurrent access | Multiple readers and rejection of conflicting writers | Passed course tests |
| Atomic transfer | No partially transferred file contents observed by readers | Passed course tests |

<!-- Optional: Add measured latency / throughput results and screenshots after running benchmarks.
![Read latency comparison](assets/read-performance.png)
-->

## Scope and Design Trade-offs

WatDFS is an educational distributed filesystem prototype rather than a production replacement for NFS or a fault-tolerant distributed storage platform. Its core focus is on **filesystem transparency, RPC boundaries, caching, consistency, and concurrency**. It supports the designated file operations, rather than a complete POSIX filesystem API, and uses a centralized backing server rather than replication or distributed consensus.

## Acknowledgments

Developed for **CS 454/654 — Distributed Systems, University of Waterloo**. The project uses course-provided FUSE integration and RPC support components; the WatDFS client/server logic was implemented as part of the course project.

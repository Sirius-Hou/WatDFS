# Running and checking WatDFS

These commands use Bash on the school Linux environment identified in the project specification: `linux.student.cs.uwaterloo.ca`. Replace `YOUR_USERID` with your school account. The hostname is the documented course entry point; current access depends on your account and the school's configuration.

## 1. Connect and build

From your own computer:

```bash
ssh YOUR_USERID@linux.student.cs.uwaterloo.ca
```

On that Linux host:

```bash
hostname -f
command -v g++ make pkg-config python3 fusermount
pkg-config --modversion fuse
test -c /dev/fuse && echo "FUSE device available"
git clone https://github.com/Sirius-Hou/WatDFS.git
cd WatDFS
make clean all
make -C baseline clean all
python3 -m unittest discover -s tests -v
```

The required development API is FUSE 2, not FUSE 3. If dependencies are missing on a managed school host, use a supported host or contact the administrator. On your own Debian/Ubuntu installation, the packages are `build-essential pkg-config libfuse-dev fuse python3`.

Record the actual hostname printed above. For the same-host procedure below, open additional SSH sessions to **that specific host**, or use multiple `tmux` windows in the existing session. Reconnecting to a load-balanced entry point may land on another machine; `/tmp` is local to each machine.

## 2. Terminal A: cached server

The examples assume the clone is at `$HOME/WatDFS`. Adjust `REPO` if needed. Choose a new `RUN` name for a new experiment; use the same name in every terminal participating in that experiment.

```bash
export REPO="$HOME/WatDFS"
export RUN="/tmp/$USER/watdfs-demo-01"
mkdir -p "$RUN/server" "$RUN/server-log"
cd "$RUN/server-log"
"$REPO/watdfs_server" "$RUN/server"
```

Keep this terminal running. Copy the actual `export SERVER_ADDRESS=...` and `export SERVER_PORT=...` lines from the server output. Do not reuse an example port or an address from a previous run.

## 3. Terminal B: first client

Connect to the same host, then run:

```bash
export REPO="$HOME/WatDFS"
export RUN="/tmp/$USER/watdfs-demo-01"
export SERVER_ADDRESS=REPLACE_WITH_SERVER_OUTPUT
export SERVER_PORT=REPLACE_WITH_SERVER_OUTPUT
export CACHE_INTERVAL_SEC=5
mkdir -p "$RUN/cache-a" "$RUN/mount-a" "$RUN/client-a-log"
cd "$RUN/client-a-log"
"$REPO/watdfs_client" -s -f -o direct_io "$RUN/cache-a" "$RUN/mount-a"
```

Keep this terminal running. `-s` selects a single-threaded client; `-f` keeps it in the foreground; `direct_io` disables the FUSE data page cache so the experiment exercises WatDFS's own cache. It does not evict the operating system's backing-file cache.

## 4. Terminal C: second client

Use the **same server address and port**, but distinct client directories:

```bash
export REPO="$HOME/WatDFS"
export RUN="/tmp/$USER/watdfs-demo-01"
export SERVER_ADDRESS=REPLACE_WITH_SERVER_OUTPUT
export SERVER_PORT=REPLACE_WITH_SERVER_OUTPUT
export CACHE_INTERVAL_SEC=5
mkdir -p "$RUN/cache-b" "$RUN/mount-b" "$RUN/client-b-log"
cd "$RUN/client-b-log"
"$REPO/watdfs_client" -s -f -o direct_io "$RUN/cache-b" "$RUN/mount-b"
```

Each process uses its own working directory so the course libraries' fixed log filenames do not collide.

## 5. Terminal D: functional checks

On the same host:

```bash
export REPO="$HOME/WatDFS"
export RUN="/tmp/$USER/watdfs-demo-01"
cd "$REPO"
mkdir -p results
set -o pipefail
python3 -u tests/smoke.py \
  --mount "$RUN/mount-a" --peer-mount "$RUN/mount-b" \
  --server-root "$RUN/server" --cache-interval 5 \
  | tee results/cached-smoke.txt
```

The script checks file creation, empty files, offset I/O, transfers larger than one RPC array, truncation, timestamps, explicit synchronization, close-triggered writeback, concurrent reading, rejection of a second writer, and refresh of an already-open reader after expiry. It waits for asynchronous release where needed and exits nonzero on failure.

The `--cache-interval` argument must match the interval used to start the reader client. It describes the running configuration; it does not change the client. Test files have unique names and are retained in the dedicated backing directory for inspection. No existing files are overwritten.

For a quick visible demonstration with a new filename:

```bash
export DEMO="demo-$(date +%s).txt"
printf 'Hello from WatDFS\n' > "$RUN/mount-a/$DEMO"
cat "$RUN/mount-b/$DEMO"
stat "$RUN/mount-a/$DEMO"
```

The automated smoke test is the authoritative check for release timing and cache coherence; a one-off `cat` is only a demonstration.

## 6. Baseline

Use a separate server and a separate directory tree, for example `RUN=/tmp/$USER/watdfs-baseline-01`. Start `$REPO/baseline/watdfs_server` and `$REPO/baseline/watdfs_client` using the same terminal pattern. Use the address and port from the **baseline server**. Do not connect the cached client to the baseline server: the baseline does not implement the cache transfer-lock protocol.

Run its basic checks without `--peer-mount`:

```bash
python3 -u "$REPO/tests/smoke.py" \
  --mount "$RUN/mount-a" --server-root "$RUN/server"
```

The multi-client cache/one-writer checks apply to the final implementation and are intentionally omitted for the baseline.

## 7. Shutdown

After test applications exit, run on each client host:

```bash
fusermount -u "$RUN/mount-a"
fusermount -u "$RUN/mount-b"  # Only if the second client was mounted.
```

Wait for the foreground clients to exit, then stop the server with Ctrl+C. Keep the logs and results until you have reviewed them. Do not remove cache or backing directories while the clients are mounted; choose a fresh `RUN` directory for the next experiment.

## Two-machine setup

Start the server on host A, then start the client on host B using the address printed by A. Create client caches, mounts, and logs under B's local `/tmp/$USER/…`; keep the server backing directory under A's local `/tmp/$USER/…`.

Run applications on B, where the FUSE mount exists. To inspect server contents, open a terminal on A. Do not assume `/tmp` is shared between hosts. The smoke script's `--server-root` requires local access to the real backing directory, so the complete automated smoke procedure above is intentionally a same-host check. For cross-host performance collection, use the separate instructions in [Benchmarking](BENCHMARKING.md).

## Troubleshooting

- **`fuse.h` / `pkg-config fuse` missing:** the FUSE 2 development package is unavailable on that host.
- **Connection failure:** compare the server's current output with the client's exported environment variables.
- **Transport endpoint is not connected:** unmount the stale mount with `fusermount -u` before starting a new client.
- **`EACCES` on a writable open:** another client may still hold the write open, including a release still being processed.
- **`ls`, `mkdir`, or `rm` inside the mount fails:** those directory/unlink operations are outside this project's interface.
- **Unexpected callback sequence:** inspect `watdfs.log`, `rpc_client.log`, and `rpc_server.log` in the corresponding process working directories. FUSE may translate one shell command into several callbacks.

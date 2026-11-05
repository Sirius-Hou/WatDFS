#!/usr/bin/env python3
"""End-to-end checks against a mounted WatDFS and its local backing directory."""

import argparse
import errno
import os
import sys
import time
import uuid
from pathlib import Path

from common import check, open_after_release, read_all, wait_for_content, write_all


def run(mount, server_root, peer_mount=None, cache_interval=5):
    prefix = "smoke-" + uuid.uuid4().hex[:12]
    payload = bytes(range(256)) * 1024
    path = mount / (prefix + "-data.bin")
    backing = server_root / path.name
    fd = os.open(path, os.O_CREAT | os.O_EXCL | os.O_RDWR, 0o600)
    try:
        check(os.fstat(fd).st_size == 0, "New file must be empty")
        write_all(fd, payload)
        os.lseek(fd, 70001, os.SEEK_SET)
        write_all(fd, b"WatDFS")
        payload = payload[:70001] + b"WatDFS" + payload[70007:]
        os.lseek(fd, 0, os.SEEK_SET)
        check(read_all(fd) == payload, "Offset write / large read mismatch")
        os.fsync(fd)
        wait_for_content(backing, payload)
        print("PASS create, empty file, offset I/O, >65,535-byte transfer, fsync")
        os.truncate(path, 70010)
        payload = payload[:70010]
        stamp = 1700000000
        os.utime(path, (stamp, stamp))
        check(int(os.stat(path).st_mtime) == stamp, "Timestamp mismatch")
        os.fsync(fd)
        wait_for_content(backing, payload)
        check(int(backing.stat().st_mtime) == stamp, "Server timestamp mismatch")
        print("PASS truncate and timestamp propagation")
    finally:
        os.close(fd)

    close_path = mount / (prefix + "-close.txt")
    fd = os.open(close_path, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
    try:
        write_all(fd, b"Written without explicit fsync\n")
    finally:
        os.close(fd)
    wait_for_content(server_root / close_path.name, b"Written without explicit fsync\n")
    print("PASS close-triggered writeback (waited for asynchronous release)")

    empty_path = mount / (prefix + "-empty")
    fd = os.open(empty_path, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
    os.close(fd)
    wait_for_content(server_root / empty_path.name, b"")
    print("PASS zero-byte file persistence")

    if peer_mount is not None:
        # Both clients connect to the cached server, with separate caches/mounts.
        fd = open_after_release(path, os.O_RDWR)
        reader = None
        try:
            try:
                unexpected = os.open(peer_mount / path.name, os.O_RDWR)
            except OSError as exc:
                check(exc.errno == errno.EACCES, "Second writer must return EACCES")
            else:
                os.close(unexpected)
                raise AssertionError("Second writer was accepted")
            reader = os.open(peer_mount / path.name, os.O_RDONLY)
            check(read_all(reader) == payload, "Concurrent reader saw unexpected data")
            updated = b"REFRESH" + payload[7:]
            os.lseek(fd, 0, os.SEEK_SET)
            write_all(fd, updated)
            # Force a distinct timestamp even on filesystems with coarse clocks.
            os.utime(path, (1700000010, 1700000010))
            os.fsync(fd)
            wait_for_content(backing, updated)
            time.sleep(cache_interval + 1)
            os.lseek(reader, 0, os.SEEK_SET)
            check(read_all(reader) == updated, "Reader did not refresh after cache expiry")
            print("PASS concurrent reader, rejected second writer, timeout-based refresh")
        finally:
            if reader is not None:
                os.close(reader)
            os.close(fd)
    else:
        print("SKIP multi-client checks (supply --peer-mount for the cached implementation)")
    print("PASS all requested checks; retained test files with prefix " + prefix)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mount", type=Path, required=True)
    parser.add_argument("--server-root", type=Path, required=True)
    parser.add_argument("--peer-mount", type=Path)
    parser.add_argument("--cache-interval", type=int, default=5)
    args = parser.parse_args()
    if args.cache_interval < 0:
        parser.error("--cache-interval must be nonnegative")
    try:
        run(args.mount, args.server_root, args.peer_mount, args.cache_interval)
    except (OSError, AssertionError) as exc:
        print("FAIL: " + str(exc), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())

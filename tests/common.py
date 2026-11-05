"""Small helpers shared by the live filesystem checks and benchmark."""

import errno
import hashlib
import os
import time
from pathlib import Path


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def write_all(fd, data):
    view = memoryview(data)
    while view:
        count = os.write(fd, view)
        if count == 0:
            raise OSError("Write made no progress")
        view = view[count:]


def read_all(fd, block_size=65536):
    chunks = []
    while True:
        block = os.read(fd, block_size)
        if not block:
            return b"".join(chunks)
        chunks.append(block)


def wait_for_content(path, expected, timeout=15):
    """FUSE release can finish after close returns; wait for the actual data."""
    deadline = time.monotonic() + timeout
    expected_hash = hashlib.sha256(expected).digest()
    while True:
        try:
            data = Path(path).read_bytes()
            if len(data) == len(expected) and hashlib.sha256(data).digest() == expected_hash:
                return
        except OSError as exc:
            if exc.errno not in (errno.ENOENT, errno.EACCES, errno.EMFILE):
                raise
        if time.monotonic() >= deadline:
            raise AssertionError("Timed out waiting for verified content: " + str(path))
        time.sleep(0.1)


def open_after_release(path, flags, timeout=15):
    """Retry only the transient open conflict caused by asynchronous release."""
    deadline = time.monotonic() + timeout
    while True:
        try:
            return os.open(path, flags)
        except OSError as exc:
            if exc.errno not in (errno.EACCES, errno.EMFILE) or time.monotonic() >= deadline:
                raise
            time.sleep(0.1)

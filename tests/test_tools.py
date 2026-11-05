"""Local self-checks for tooling; these do not replace a live FUSE test."""

import contextlib
import csv
import io
import json
import os
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import benchmark
from summarize import summarize
from common import wait_for_content, write_all
from smoke import run as smoke


class ToolTests(unittest.TestCase):
    def test_short_and_zero_writes(self):
        with patch("common.os.write", side_effect=[2, 3]) as write:
            write_all(123, b"hello")
            self.assertEqual(write.call_count, 2)
        with patch("common.os.write", return_value=0):
            with self.assertRaises(OSError):
                write_all(123, b"hello")

    def test_content_mismatch_fails(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "file"
            path.write_bytes(b"actual")
            with self.assertRaises(AssertionError):
                wait_for_content(path, b"expected", timeout=0)

    def test_local_workloads(self):
        with tempfile.TemporaryDirectory() as directory, contextlib.redirect_stdout(io.StringIO()):
            root = Path(directory)
            smoke(root, root)
            csv_path = benchmark.run(root, root / "results", "self-check", 1, 2, 8)
            with csv_path.open(newline="") as stream:
                rows = list(csv.DictReader(stream))
            self.assertEqual(len(rows), 18)
            self.assertEqual({row["trial"] for row in rows}, {"1", "2"})
            self.assertTrue(all(float(row["seconds"]) >= 0 for row in rows))
            self.assertEqual(len(list(root.glob("bench-*.bin"))), 2)
            summarize([csv_path])
            partial = root / "partial.csv"
            partial.write_text("label,trial,phase,seconds,bytes,mib_per_s\nx,1,read_open,1,0,\n")
            with self.assertRaises(ValueError):
                summarize([partial])
            metadata_path = csv_path.with_suffix(".json")
            metadata = json.loads(metadata_path.read_text())
            metadata["status"] = "running"
            metadata_path.write_text(json.dumps(metadata))
            with self.assertRaises(ValueError):
                summarize([csv_path])


if __name__ == "__main__":
    unittest.main()

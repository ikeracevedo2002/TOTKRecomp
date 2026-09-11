#!/usr/bin/env python3
"""Focused tests for the pure CI change classifier."""

from __future__ import annotations

import json
import pathlib
import subprocess
import sys
import unittest


SCRIPT_DIR = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(SCRIPT_DIR))
from classify_changes import classify_paths  # noqa: E402


class ClassifierTests(unittest.TestCase):
    def assert_light(self, paths: list[object]) -> None:
        result = classify_paths(paths)
        self.assertEqual(result["classification"], "light")
        self.assertTrue(result["classification_ok"])

    def assert_heavy(self, paths: list[object]) -> None:
        self.assertEqual(classify_paths(paths)["classification"], "heavy")

    def test_allowlisted_paths(self) -> None:
        self.assert_light(["docs/MILESTONE_35.md"])
        self.assert_light(["README.md"])
        self.assert_light(["AGENTS.md"])
        self.assert_light(["docs/subdir/foo.md"])

    def test_heavy_paths(self) -> None:
        for path in (
            "src/foo.cpp",
            "include/foo.hpp",
            "tests/foo.cpp",
            "tools/foo.cpp",
            "scripts/foo.py",
            ".github/workflows/ci.yml",
            "CMakeLists.txt",
            "config/foo.json",
            "targets/foo.json",
            "unknown.root",
        ):
            with self.subTest(path=path):
                self.assert_heavy([path])

    def test_mixed_and_spaces(self) -> None:
        self.assert_heavy(["docs/foo.md", "src/foo.cpp"])
        self.assert_light(["docs/a document with spaces.md"])
        self.assert_heavy(["docs/a document with spaces.md", "tools/a tool.cpp"])

    def test_duplicates_and_empty(self) -> None:
        self.assert_light(["README.md", "README.md", "docs/x.md"])
        result = classify_paths([])
        self.assertEqual(result["reason"], "empty-input")
        self.assert_heavy([])

    def test_malformed_and_traversal_paths_fail_closed(self) -> None:
        for path in (None, "", "../README.md", "docs/../src/foo.cpp", "/tmp/README.md", "docs\\foo.md"):
            with self.subTest(path=path):
                result = classify_paths([path])
                self.assertEqual(result["classification"], "heavy")
                self.assertFalse(result["classification_ok"])

    def test_cli_malformed_json_is_heavy(self) -> None:
        completed = subprocess.run(
            [sys.executable, str(SCRIPT_DIR / "classify_changes.py")],
            input=b"not-json",
            capture_output=True,
            check=True,
        )
        result = json.loads(completed.stdout)
        self.assertEqual(result["classification"], "heavy")
        self.assertEqual(result["reason"], "malformed-input")

    def test_cli_null_paths_preserves_spaces(self) -> None:
        completed = subprocess.run(
            [sys.executable, str(SCRIPT_DIR / "classify_changes.py"), "--input-format", "null"],
            input=b"docs/a file.md\x00README.md\x00",
            capture_output=True,
            check=True,
        )
        result = json.loads(completed.stdout)
        self.assertEqual(result["classification"], "light")
        self.assertEqual(result["paths"], ["docs/a file.md", "README.md"])


if __name__ == "__main__":
    unittest.main()

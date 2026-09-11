#!/usr/bin/env python3
"""Pure, fail-closed path classification for the CI workflow."""

from __future__ import annotations

import argparse
import json
import sys
from typing import Iterable


def _valid_relative_path(path: object) -> bool:
    if not isinstance(path, str) or not path or "\x00" in path:
        return False
    if path.startswith(("/", "./", "../")) or path.endswith("/"):
        return False
    if "\\" in path or "//" in path or "/../" in path or path == "..":
        return False
    return True


def _is_allowlisted(path: str) -> bool:
    return path in {"README.md", "AGENTS.md"} or (
        path.startswith("docs/") and len(path) > len("docs/")
    )


def classify_paths(paths: Iterable[object]) -> dict[str, object]:
    """Classify a complete changed-path set.

    The result is deliberately conservative: an empty, malformed, unknown, or
    mixed path set is heavy. Duplicate paths do not alter the classification.
    """

    materialized = list(paths)
    if not materialized:
        return {
            "classification": "heavy",
            "classification_ok": False,
            "reason": "empty-input",
            "paths": [],
        }

    if any(not _valid_relative_path(path) for path in materialized):
        return {
            "classification": "heavy",
            "classification_ok": False,
            "reason": "malformed-path",
            "paths": materialized,
        }

    disallowed = [path for path in materialized if not _is_allowlisted(path)]
    if disallowed:
        return {
            "classification": "heavy",
            "classification_ok": True,
            "reason": "non-documentation-path",
            "paths": materialized,
            "heavy_paths": disallowed,
        }

    return {
        "classification": "light",
        "classification_ok": True,
        "reason": "strict-documentation-allowlist",
        "paths": materialized,
    }


def _read_paths(args: argparse.Namespace) -> list[object]:
    if args.path:
        return list(args.path)

    raw = sys.stdin.buffer.read()
    if args.input_format == "json":
        value = json.loads(raw.decode("utf-8"))
        if not isinstance(value, list):
            raise ValueError("JSON input must be an array")
        return value
    if args.input_format == "null":
        if not raw.endswith(b"\x00"):
            raise ValueError("NUL input must end with a NUL separator")
        return raw.decode("utf-8").split("\x00")[:-1]
    return raw.decode("utf-8").splitlines()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--path", action="append", help="changed path; may be repeated")
    parser.add_argument(
        "--input-format",
        choices=("json", "lines", "null"),
        default="json",
        help="stdin encoding (default: JSON array)",
    )
    args = parser.parse_args()
    try:
        paths = _read_paths(args)
        result = classify_paths(paths)
    except (UnicodeDecodeError, json.JSONDecodeError, TypeError, ValueError) as error:
        result = {
            "classification": "heavy",
            "classification_ok": False,
            "reason": "malformed-input",
            "error": str(error),
            "paths": [],
        }
    print(json.dumps(result, sort_keys=True, separators=(",", ":")))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

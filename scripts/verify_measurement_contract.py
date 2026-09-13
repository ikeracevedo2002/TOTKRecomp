#!/usr/bin/env python3
"""Verify the local measurement input against a frozen contract.

This exists because milestones were previously lost when the private local
recovery set drifted: an implementation could be correct and still be
unmeasurable, and the drift only surfaced after a long real run. The check is
deliberately offline and cheap.

Privacy contract
----------------
The tracked repository holds only this verifier and its schema. The contract
document itself carries module sizes, SHA-256 digests and the previously
measured real stop, which are private real-execution observations. It therefore
lives outside the tracked tree (by convention ``local/m*-frozen-contract.json``)
and this script never prints file contents; it prints digests it computes and
the expectations read from the contract, both of which are already considered
non-public and only ever displayed, never committed.

Exit codes
----------
0  contract matched; the measurement input is reproducible
2  contract or configuration could not be read
3  drift detected; the milestone is blocked, not reinterpretable
"""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path
from typing import Any

EXIT_OK = 0
EXIT_UNREADABLE = 2
EXIT_DRIFT = 3

DEFAULT_CONTRACT = Path("local/m38-frozen-contract.json")
DEFAULT_CONFIG = Path("config/local.json")
CHUNK = 1024 * 1024


def fail(message: str) -> None:
    print(f"CONTRACT_UNREADABLE: {message}", file=sys.stderr)


def drift(message: str) -> None:
    print(f"CONTRACT_DRIFT: {message}", file=sys.stderr)


def sha256_of(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(CHUNK), b""):
            digest.update(block)
    return digest.hexdigest()


def load_json(path: Path) -> Any:
    with path.open("r", encoding="utf-8") as handle:
        return json.load(handle)


def modules_from_config(config: Any) -> dict[str, str]:
    """Return declared module name -> path from the local configuration."""
    modules = config.get("modules")
    if not isinstance(modules, dict) or not modules:
        raise ValueError("configuration has no non-empty 'modules' object")
    resolved: dict[str, str] = {}
    for name, value in modules.items():
        if not isinstance(value, str) or not value:
            raise ValueError(f"module '{name}' is not a path string")
        resolved[str(name)] = value
    return resolved


def basename_of(path: str) -> str:
    return Path(path).name


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--contract", type=Path, default=DEFAULT_CONTRACT,
                        help="frozen contract JSON (outside the tracked tree)")
    parser.add_argument("--config", type=Path, default=DEFAULT_CONFIG,
                        help="local configuration declaring module paths")
    parser.add_argument("--verbose", action="store_true",
                        help="print every module result, not only failures")
    args = parser.parse_args(argv)

    if not args.contract.is_file():
        fail(f"contract not found: {args.contract}")
        return EXIT_UNREADABLE
    if not args.config.is_file():
        fail(f"configuration not found: {args.config}")
        return EXIT_UNREADABLE

    try:
        contract = load_json(args.contract)
        config = load_json(args.config)
    except (OSError, ValueError) as error:
        fail(f"could not parse input: {error}")
        return EXIT_UNREADABLE

    expected = contract.get("modules")
    if not isinstance(expected, list) or not expected:
        fail("contract has no non-empty 'modules' array")
        return EXIT_UNREADABLE

    try:
        configured = modules_from_config(config)
    except ValueError as error:
        fail(str(error))
        return EXIT_UNREADABLE

    problems: list[str] = []

    # The contract is keyed by basename; the configuration is keyed by module
    # name. Compare on the basename so a renamed module directory does not
    # silently pass while a replaced binary does.
    by_basename: dict[str, dict[str, Any]] = {}
    for entry in expected:
        if not isinstance(entry, dict):
            fail("contract module entry is not an object")
            return EXIT_UNREADABLE
        name = entry.get("basename")
        if not isinstance(name, str):
            fail("contract module entry has no 'basename'")
            return EXIT_UNREADABLE
        by_basename[name] = entry

    for module, path_text in sorted(configured.items()):
        path = Path(path_text)
        name = basename_of(path_text)
        expectation = by_basename.get(name)
        if expectation is None:
            problems.append(f"module '{module}' basename '{name}' is absent from the contract")
            continue
        if not path.is_file():
            problems.append(f"module '{module}' file is missing: {name}")
            continue

        actual_size = path.stat().st_size
        want_size = expectation.get("size")
        if isinstance(want_size, int) and actual_size != want_size:
            problems.append(
                f"module '{module}' size is {actual_size}, contract expects {want_size}"
            )
            continue

        actual_digest = sha256_of(path)
        want_digest = expectation.get("sha256")
        if isinstance(want_digest, str) and actual_digest != want_digest:
            problems.append(
                f"module '{module}' sha256 is {actual_digest}, contract expects {want_digest}"
            )
            continue

        if args.verbose:
            print(f"OK {module} size={actual_size} sha256={actual_digest}")

    for name in sorted(by_basename):
        if not any(basename_of(p) == name for p in configured.values()):
            problems.append(f"contract module '{name}' is not declared in the configuration")

    if problems:
        for item in problems:
            drift(item)
        print(
            f"BLOCKED: measurement input does not reproduce the frozen contract "
            f"({len(problems)} problem(s)). The milestone is blocked, not reinterpretable.",
            file=sys.stderr,
        )
        return EXIT_DRIFT

    stop = contract.get("first_real_stop")
    if isinstance(stop, dict):
        print(
            "CONTRACT_MATCH: input reproduces the frozen module set. "
            f"documented real stop={stop.get('stop_reason', 'unknown')} "
            f"guest_instructions={stop.get('guest_instruction_count', 'unknown')}"
        )
    else:
        print("CONTRACT_MATCH: input reproduces the frozen module set.")
    return EXIT_OK


if __name__ == "__main__":
    sys.exit(main())

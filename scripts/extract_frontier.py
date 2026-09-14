#!/usr/bin/env python3
"""Extract compact, path-free frontier evidence from a run-entry JSON report."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
from typing import Any


def _required(mapping: dict[str, Any], key: str) -> Any:
    if key not in mapping:
        raise ValueError(f"report is missing required field: {key}")
    return mapping[key]


def extract(report_bytes: bytes) -> dict[str, Any]:
    document = json.loads(report_bytes)
    execution = _required(document, "execution")
    if not isinstance(execution, dict):
        raise ValueError("report execution field is not an object")
    refinement = execution.get("indirect_target_refinement") or {}
    if not isinstance(refinement, dict):
        raise ValueError("indirect_target_refinement field is not an object")
    return {
        "schema_version": 1,
        "report_sha256": hashlib.sha256(report_bytes).hexdigest(),
        "frontier": {
            "guest_instruction_count": _required(execution, "guest_instruction_count"),
            "stop_reason": _required(execution, "stop_reason"),
            "stop_module": _required(execution, "stop_module"),
            "stop_pc": _required(execution, "stop_pc"),
            "current_function_module": _required(execution, "current_function_module"),
            "current_function": _required(execution, "current_function"),
            "target": execution.get("target"),
            "source_pc": execution.get("source_pc"),
            "diagnostic": _required(execution, "diagnostic"),
        },
        "refinement": {
            key: refinement.get(key, 0)
            for key in (
                "total_execution_attempts",
                "productive_rounds",
                "stagnant_rounds",
                "candidate_assessments",
                "successful_promotions",
                "map_rebuilds",
                "map_generation",
                "pending_candidate_count",
                "exhausted_dimension",
            )
        },
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report", type=Path)
    args = parser.parse_args()
    try:
        evidence = extract(args.report.read_bytes())
    except (OSError, ValueError, json.JSONDecodeError) as error:
        parser.error(str(error))
    print(json.dumps(evidence, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

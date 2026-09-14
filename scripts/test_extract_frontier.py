#!/usr/bin/env python3

import hashlib
import json
import unittest

from extract_frontier import extract


class ExtractFrontierTests(unittest.TestCase):
    def test_extracts_only_compact_frontier_and_refinement_evidence(self) -> None:
        report = json.dumps(
            {
                "private_noise": {"host_path": "/private/input"},
                "execution": {
                    "guest_instruction_count": 123,
                    "stop_reason": "memory_fault",
                    "stop_module": "main",
                    "stop_pc": "0x1234",
                    "current_function_module": "main",
                    "current_function": "0x1200",
                    "target": None,
                    "source_pc": None,
                    "diagnostic": "unaligned",
                    "events": [{"private": "large"}],
                    "indirect_target_refinement": {
                        "total_execution_attempts": 8,
                        "productive_rounds": 7,
                        "stagnant_rounds": 1,
                        "candidate_assessments": 12,
                        "successful_promotions": 7,
                        "map_rebuilds": 7,
                        "map_generation": 7,
                        "pending_candidate_count": 0,
                        "exhausted_dimension": "none",
                    },
                },
            },
            separators=(",", ":"),
        ).encode()
        evidence = extract(report)
        self.assertEqual(evidence["report_sha256"], hashlib.sha256(report).hexdigest())
        self.assertEqual(evidence["frontier"]["guest_instruction_count"], 123)
        self.assertEqual(evidence["refinement"]["successful_promotions"], 7)
        self.assertNotIn("private_noise", evidence)
        self.assertNotIn("events", evidence)

    def test_rejects_missing_frontier_identity(self) -> None:
        with self.assertRaisesRegex(ValueError, "guest_instruction_count"):
            extract(b'{"execution": {}}')


if __name__ == "__main__":
    unittest.main()

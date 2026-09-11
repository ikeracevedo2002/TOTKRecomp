#!/usr/bin/env python3
"""Focused fail-closed tests for event and carry-forward policy."""

from __future__ import annotations

import pathlib
import sys
import unittest
from unittest.mock import patch


SCRIPT_DIR = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(SCRIPT_DIR))
import ci_decision  # noqa: E402


class DecisionTests(unittest.TestCase):
    def test_non_synchronize_pull_request_is_heavy(self) -> None:
        result = ci_decision.decide({"action": "opened", "pull_request": {"number": 7}}, "pull_request")
        self.assertTrue(result["heavy_required"])
        self.assertFalse(result["carry_forward_proven"])

    def test_missing_delta_is_heavy(self) -> None:
        result = ci_decision.decide(
            {"action": "synchronize", "pull_request": {"number": 7}}, "pull_request"
        )
        self.assertTrue(result["heavy_required"])
        self.assertEqual(result["reason"], "missing-before-after-delta")

    @patch.object(ci_decision, "_successful_gate_for_predecessor", return_value=(True, "test-proof"))
    @patch.object(ci_decision, "_changed_paths", return_value=(["docs/foo.md", "README.md"], None))
    def test_docs_delta_needs_predecessor_proof(self, changed_paths, predecessor) -> None:
        event = {
            "action": "synchronize",
            "number": 7,
            "before": "a" * 40,
            "after": "b" * 40,
            "pull_request": {"number": 7},
        }
        result = ci_decision.decide(event, "pull_request")
        self.assertFalse(result["heavy_required"])
        self.assertTrue(result["classification_ok"])
        self.assertTrue(result["carry_forward_proven"])
        changed_paths.assert_called_once_with("a" * 40, "b" * 40)
        predecessor.assert_called_once_with("a" * 40, 7)

    @patch.object(ci_decision, "_successful_gate_for_predecessor")
    @patch.object(ci_decision, "_changed_paths", return_value=(["docs/foo.md", "src/foo.cpp"], None))
    def test_mixed_delta_never_queries_carry_forward(self, changed_paths, predecessor) -> None:
        event = {
            "action": "synchronize",
            "number": 7,
            "before": "a" * 40,
            "after": "b" * 40,
            "pull_request": {"number": 7},
        }
        result = ci_decision.decide(event, "pull_request")
        self.assertTrue(result["heavy_required"])
        self.assertEqual(result["reason"], "non-documentation-path")
        predecessor.assert_not_called()

    @patch.object(ci_decision, "_changed_paths", return_value=(None, "non-ancestor-history"))
    def test_non_ancestor_history_is_heavy(self, changed_paths) -> None:
        event = {
            "action": "synchronize",
            "number": 7,
            "before": "a" * 40,
            "after": "b" * 40,
            "pull_request": {"number": 7},
        }
        result = ci_decision.decide(event, "pull_request")
        self.assertTrue(result["heavy_required"])
        self.assertEqual(result["reason"], "non-ancestor-history")

    def test_manual_and_main_push_are_heavy(self) -> None:
        with patch.dict(ci_decision.os.environ, {"GITHUB_REF": "refs/heads/main"}, clear=False):
            push = ci_decision.decide({"ref": "refs/heads/main"}, "push")
        manual = ci_decision.decide({}, "workflow_dispatch")
        self.assertTrue(push["heavy_required"])
        self.assertEqual(push["reason"], "main-push")
        self.assertTrue(manual["heavy_required"])
        self.assertEqual(manual["reason"], "manual-validation")


if __name__ == "__main__":
    unittest.main()

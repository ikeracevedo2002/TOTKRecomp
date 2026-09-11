#!/usr/bin/env python3
"""Resolve GitHub event policy and carry-forward proof for CI.

Path classification remains in classify_changes.py. This module owns only
event, Git, and GitHub Actions metadata/API decisions.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
from pathlib import Path
from typing import Any
from urllib.error import HTTPError, URLError
from urllib.parse import urlencode
from urllib.request import Request, urlopen

SCRIPT_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(SCRIPT_DIR))
from classify_changes import classify_paths  # noqa: E402

SHA_RE = re.compile(r"^[0-9a-fA-F]{40}$")


def _git(*args: str) -> subprocess.CompletedProcess[bytes]:
    return subprocess.run(["git", *args], check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE)


def _available_commit(sha: str) -> bool:
    return bool(SHA_RE.fullmatch(sha)) and _git("cat-file", "-e", f"{sha}^{{commit}}").returncode == 0


def _changed_paths(before: str, after: str) -> tuple[list[str] | None, str | None]:
    if not _available_commit(before) or not _available_commit(after):
        return None, "commit-unavailable"
    if _git("merge-base", "--is-ancestor", before, after).returncode != 0:
        return None, "non-ancestor-history"
    diff = _git("diff", "--name-only", "--no-renames", "-z", before, after)
    if diff.returncode != 0:
        return None, "diff-failed"
    try:
        raw_paths = diff.stdout.decode("utf-8")
    except UnicodeDecodeError:
        return None, "non-utf8-path"
    paths = raw_paths.split("\x00")
    if paths and paths[-1] == "":
        paths.pop()
    return paths, None


def _api_get(path: str, token: str, api_url: str) -> Any:
    url = f"{api_url.rstrip('/')}{path}"
    request = Request(
        url,
        headers={
            "Accept": "application/vnd.github+json",
            "Authorization": f"Bearer {token}",
            "X-GitHub-Api-Version": "2022-11-28",
        },
    )
    with urlopen(request, timeout=15) as response:
        return json.load(response)


def _same_pr(run: dict[str, Any], pr_number: int) -> bool:
    pull_requests = run.get("pull_requests")
    if not isinstance(pull_requests, list):
        return False
    return any(isinstance(item, dict) and item.get("number") == pr_number for item in pull_requests)


def _successful_gate_for_predecessor(before: str, pr_number: int) -> tuple[bool, str]:
    token = os.environ.get("GITHUB_TOKEN", "")
    repository = os.environ.get("GITHUB_REPOSITORY", "")
    api_url = os.environ.get("GITHUB_API_URL", "https://api.github.com")
    if not token or not repository:
        return False, "github-api-credentials-unavailable"

    workflow_path = "/actions/workflows/ci.yml/runs?" + urlencode(
        {"head_sha": before, "event": "pull_request", "per_page": "100"}
    )
    try:
        payload = _api_get(f"/repos/{repository}{workflow_path}", token, api_url)
    except (HTTPError, URLError, TimeoutError, OSError, json.JSONDecodeError):
        return False, "github-api-lookup-failed"

    runs = payload.get("workflow_runs") if isinstance(payload, dict) else None
    if not isinstance(runs, list):
        return False, "github-api-ambiguous-response"

    matching_runs = [
        run
        for run in runs
        if isinstance(run, dict)
        and run.get("head_sha") == before
        and run.get("event") == "pull_request"
        and run.get("status") == "completed"
        and run.get("conclusion") == "success"
        and _same_pr(run, pr_number)
        and isinstance(run.get("id"), int)
    ]
    if not matching_runs:
        return False, "no-successful-predecessor-gate"

    for run in matching_runs:
        try:
            jobs_payload = _api_get(
                f"/repos/{repository}/actions/runs/{run['id']}/jobs?per_page=100",
                token,
                api_url,
            )
        except (HTTPError, URLError, TimeoutError, OSError, json.JSONDecodeError):
            continue
        jobs = jobs_payload.get("jobs") if isinstance(jobs_payload, dict) else None
        if not isinstance(jobs, list):
            continue
        for job in jobs:
            if (
                isinstance(job, dict)
                and job.get("name") == "CI Gate"
                and job.get("status") == "completed"
                and job.get("conclusion") == "success"
            ):
                return True, "successful-predecessor-ci-gate"
    return False, "predecessor-gate-not-successful"


def _write_output(path: str, values: dict[str, object]) -> None:
    with open(path, "a", encoding="utf-8") as output:
        for key, value in values.items():
            text = str(value).lower() if isinstance(value, bool) else str(value)
            output.write(f"{key}={text}\n")


def _summary(path: str, result: dict[str, object]) -> None:
    with open(path, "a", encoding="utf-8") as output:
        output.write("## CI lane decision\n\n")
        output.write(f"- Classification: `{result['classification']}`\n")
        output.write(f"- Heavy required: `{result['heavy_required']}`\n")
        output.write(f"- Reason: `{result['reason']}`\n")
        if result.get("before") and result.get("after"):
            output.write(f"- Delta: `{result['before']}` -> `{result['after']}`\n")
        if result.get("changed_paths") is not None:
            output.write(f"- Changed paths: `{result['changed_paths']}`\n")


def decide(event: dict[str, Any], event_name: str) -> dict[str, object]:
    result: dict[str, object] = {
        "classification": "heavy",
        "classification_ok": False,
        "carry_forward_proven": False,
        "heavy_required": True,
        "before": "",
        "after": "",
        "changed_paths": None,
        "reason": "fail-closed-default",
    }

    if event_name == "workflow_dispatch":
        result["reason"] = "manual-validation"
        return result
    if event_name == "push":
        result["reason"] = "main-push" if os.environ.get("GITHUB_REF") == "refs/heads/main" else "unsupported-push"
        return result
    if event_name != "pull_request":
        result["reason"] = "unsupported-event"
        return result

    action = event.get("action")
    if action != "synchronize":
        result["reason"] = f"pull-request-{action or 'unknown'}-requires-heavy"
        return result

    pull_request = event.get("pull_request")
    if not isinstance(pull_request, dict) or not isinstance(pull_request.get("number"), int):
        result["reason"] = "missing-pull-request-context"
        return result

    before = event.get("before")
    after = event.get("after")
    if not isinstance(before, str) or not isinstance(after, str):
        result["reason"] = "missing-before-after-delta"
        return result
    result["before"] = before
    result["after"] = after

    paths, error = _changed_paths(before, after)
    if error is not None or paths is None:
        result["reason"] = error or "changed-path-retrieval-failed"
        return result
    result["changed_paths"] = len(paths)
    classification = classify_paths(paths)
    result["classification"] = classification["classification"]
    result["classification_ok"] = classification["classification_ok"]
    if classification["classification"] != "light" or not classification["classification_ok"]:
        result["reason"] = str(classification["reason"])
        return result

    proven, reason = _successful_gate_for_predecessor(before, pull_request["number"])
    result["carry_forward_proven"] = proven
    result["reason"] = reason
    if proven:
        result["heavy_required"] = False
    return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", required=True)
    parser.add_argument("--summary", required=True)
    args = parser.parse_args()

    event_path = os.environ.get("GITHUB_EVENT_PATH", "")
    try:
        with open(event_path, encoding="utf-8") as event_file:
            event = json.load(event_file)
        if not isinstance(event, dict):
            raise ValueError("event payload must be an object")
    except (OSError, UnicodeDecodeError, json.JSONDecodeError, ValueError) as error:
        result = {
            "classification": "heavy",
            "classification_ok": False,
            "carry_forward_proven": False,
            "heavy_required": True,
            "before": "",
            "after": "",
            "changed_paths": None,
            "reason": f"malformed-event:{error}",
        }
    else:
        result = decide(event, os.environ.get("GITHUB_EVENT_NAME", ""))

    _write_output(args.output, result)
    _summary(args.summary, result)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

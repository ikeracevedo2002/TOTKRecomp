#!/usr/bin/env bash
set -euo pipefail

usage() {
    echo "usage: $0 --session NAME --worktree PATH --prompt PATH --report PATH --log PATH [--sha SHA]" >&2
    exit 2
}

session=
worktree=
prompt=
report=
log=
expected_sha=
while (($#)); do
    case "$1" in
        --session|--worktree|--prompt|--report|--log|--sha)
            (($# >= 2)) || usage
            case "$1" in
                --session) session=$2 ;;
                --worktree) worktree=$2 ;;
                --prompt) prompt=$2 ;;
                --report) report=$2 ;;
                --log) log=$2 ;;
                --sha) expected_sha=$2 ;;
            esac
            shift 2
            ;;
        *)
            usage
            ;;
    esac
done

[[ -n "$session" && -n "$worktree" && -n "$prompt" && -n "$report" && -n "$log" ]] || usage
[[ "$session" != *[!A-Za-z0-9_.-]* ]] || { echo "unsafe tmux session name: $session" >&2; exit 1; }
command -v tmux >/dev/null 2>&1 || { echo "tmux is required; refusing to install it" >&2; exit 1; }
pi_bin=$(command -v pi) || { echo "pi is required; refusing to install or reconfigure it" >&2; exit 1; }
[[ -f "$prompt" ]] || { echo "prompt file does not exist: $prompt" >&2; exit 1; }
[[ -d "$worktree" ]] || { echo "review worktree does not exist: $worktree" >&2; exit 1; }
git -C "$worktree" rev-parse --show-toplevel >/dev/null || {
    echo "review path is not a Git worktree: $worktree" >&2
    exit 1
}
actual_sha=$(git -C "$worktree" rev-parse HEAD)
if [[ -n "$expected_sha" && "$actual_sha" != "$expected_sha" ]]; then
    echo "review SHA mismatch: $actual_sha != $expected_sha" >&2
    exit 1
fi
[[ ! -e "$report" ]] || { echo "refusing to overwrite report: $report" >&2; exit 1; }
[[ ! -e "$log" ]] || { echo "refusing to overwrite log: $log" >&2; exit 1; }
report_dir=$(dirname "$report")
log_dir=$(dirname "$log")
mkdir -p "$report_dir" "$log_dir"
worker=$(cd "$(dirname "$0")" && pwd -P)/pi_worker.sh

if tmux has-session -t "$session" 2>/dev/null; then
    echo "refusing to conflict with existing tmux session: $session" >&2
    exit 1
fi

status_file=$(mktemp "${TMPDIR:-/tmp}/totkrecomp-pi-status.XXXXXX")
rm -f "$status_file"
cleanup_status() {
    rm -f "$status_file"
}
trap cleanup_status EXIT

tmux new-session -d -s "$session" -c "$worktree" -- \
    "$worker" "$pi_bin" "$prompt" "$report" "$log" "$status_file"

while tmux has-session -t "$session" 2>/dev/null; do
    sleep 1
done

if [[ ! -f "$status_file" ]]; then
    echo "Pi session ended without an exit-status record: $session" >&2
    exit 1
fi
exit_code=$(<"$status_file")
if [[ "$exit_code" != 0 ]]; then
    echo "Pi verifier failed with exit status $exit_code; see $log" >&2
    exit "$exit_code"
fi
[[ -f "$report" ]] || { echo "Pi exited successfully without a report: $report" >&2; exit 1; }
echo "Pi report: $report"
echo "Pi log: $log"
echo "Reviewed SHA: $actual_sha"

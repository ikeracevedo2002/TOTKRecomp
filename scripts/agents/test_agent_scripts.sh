#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "$0")/../.." && pwd -P)
tmp_dir=$(mktemp -d "${TMPDIR:-/tmp}/totkrecomp-agent-test.XXXXXX")
review_worktree="$tmp_dir/review worktree"
report="$tmp_dir/report with spaces.md"
log="$tmp_dir/log with spaces.txt"
prompt="$tmp_dir/prompt with spaces.md"
fake_bin="$tmp_dir/bin"
session="totkrecomp-agent-test-$$"
cleanup() {
    tmux kill-session -t "$session" 2>/dev/null || true
    if [[ -d "$review_worktree" ]]; then
        git -C "$root" worktree remove --force "$review_worktree" 2>/dev/null || true
    fi
    rm -rf "$tmp_dir"
}
trap cleanup EXIT

bash -n "$root/scripts/agents/prepare_review_worktree.sh"
bash -n "$root/scripts/agents/pi_worker.sh"
bash -n "$root/scripts/agents/run_pi_verifier.sh"

"$root/scripts/agents/prepare_review_worktree.sh" \
    --repo "$root" \
    --worktree "$review_worktree" \
    --sha "$(git -C "$root" rev-parse HEAD)" >/dev/null
[[ "$(git -C "$review_worktree" rev-parse HEAD)" == "$(git -C "$root" rev-parse HEAD)" ]]

mkdir -p "$fake_bin"
cat >"$fake_bin/pi" <<'FAKE_PI'
#!/usr/bin/env bash
set -euo pipefail
[[ "${1:-}" == "--no-approve" ]]
[[ "${2:-}" == "--no-session" ]]
[[ "${3:-}" == "--tools" ]]
[[ "${4:-}" == "read,grep,find,ls,bash" ]]
[[ "${5:-}" == "-p" ]]
[[ "${6:-}" == @* ]]
echo "fake pi report"
FAKE_PI
chmod +x "$fake_bin/pi"
printf '%s\n' 'fake prompt' >"$prompt"

PATH="$fake_bin:$PATH" "$root/scripts/agents/run_pi_verifier.sh" \
    --session "$session" \
    --worktree "$review_worktree" \
    --prompt "$prompt" \
    --report "$report" \
    --log "$log" \
    --sha "$(git -C "$root" rev-parse HEAD)"

grep -Fxq 'fake pi report' "$report"
[[ -f "$log" ]]
! tmux has-session -t "$session" 2>/dev/null

#!/usr/bin/env bash
set -euo pipefail

usage() {
    echo "usage: $0 --repo PATH --worktree PATH --sha SHA" >&2
    exit 2
}

repo=
worktree=
sha=
while (($#)); do
    case "$1" in
        --repo)
            (($# >= 2)) || usage
            repo=$2
            shift 2
            ;;
        --worktree)
            (($# >= 2)) || usage
            worktree=$2
            shift 2
            ;;
        --sha)
            (($# >= 2)) || usage
            sha=$2
            shift 2
            ;;
        *)
            usage
            ;;
    esac
done

[[ -n "$repo" && -n "$worktree" && -n "$sha" ]] || usage
[[ -d "$repo" ]] || { echo "repository does not exist: $repo" >&2; exit 1; }

resolved_sha=$(git -C "$repo" rev-parse --verify "$sha^{commit}") || {
    echo "checkpoint is not a commit available in repository: $sha" >&2
    exit 1
}

if [[ -e "$worktree" ]]; then
    [[ -d "$worktree" ]] || { echo "review worktree path is not a directory: $worktree" >&2; exit 1; }
    current_sha=$(git -C "$worktree" rev-parse HEAD 2>/dev/null) || {
        echo "existing review path is not a Git worktree: $worktree" >&2
        exit 1
    }
    if [[ "$current_sha" != "$resolved_sha" ]]; then
        echo "existing review worktree points to $current_sha, expected $resolved_sha; use a new path" >&2
        exit 1
    fi
else
    git -C "$repo" worktree add --detach "$worktree" "$resolved_sha"
fi

actual_root=$(git -C "$worktree" rev-parse --show-toplevel)
actual_sha=$(git -C "$worktree" rev-parse HEAD)
[[ "$actual_sha" == "$resolved_sha" ]] || {
    echo "review worktree SHA mismatch: $actual_sha != $resolved_sha" >&2
    exit 1
}
[[ "$actual_root" == "$(cd "$worktree" && pwd -P)" ]] || {
    echo "review worktree root mismatch: $actual_root" >&2
    exit 1
}
git -C "$worktree" diff --quiet || {
    echo "review worktree is not clean: $worktree" >&2
    exit 1
}

echo "Review worktree: $actual_root"
echo "Review SHA: $actual_sha"

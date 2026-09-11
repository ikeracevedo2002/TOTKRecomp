#!/usr/bin/env bash
set -euo pipefail

[[ $# -eq 5 ]] || {
    echo "usage: pi_worker.sh PI PROMPT REPORT LOG STATUS" >&2
    exit 2
}

pi_bin=$1
prompt=$2
report=$3
log=$4
status=$5

set +e
"$pi_bin" --no-approve --no-session --tools read,grep,find,ls,bash -p "@$prompt" >"$report" 2>"$log"
exit_code=$?
set -e
printf '%s\n' "$exit_code" >"$status"
exit "$exit_code"

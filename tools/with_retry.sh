#!/usr/bin/env bash
# Run a command, retrying with a doubling delay when it fails. For network
# fetches only: a failing test or build must fail on its first run.
#   tools/with_retry.sh git clone URL DIR
# RETRY_ATTEMPTS (default 5) and RETRY_DELAY seconds (default 5) tune it.
set -u

attempts=${RETRY_ATTEMPTS:-5}
delay=${RETRY_DELAY:-5}
attempt=1
while true; do
  "$@" && exit 0
  status=$?
  if [ "$attempt" -ge "$attempts" ]; then
    echo "with_retry: giving up after $attempt attempts: $*" >&2
    exit "$status"
  fi
  echo "with_retry: attempt $attempt failed (exit $status), retrying in" \
       "${delay}s: $*" >&2
  sleep "$delay"
  delay=$((delay * 2))
  attempt=$((attempt + 1))
done

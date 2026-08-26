#!/usr/bin/env bash
# What has the Stan language grown that the pinned toolchain cannot see?
#
# stanc3 checks its complete signature dump into its own repository as a
# cram expectation (test/integration/signatures/stan_math_signatures.t),
# updated in the same commit as any signature change. Diffing that file
# between our pinned stanc3 revision and upstream master answers "what
# are we missing" by reading text: no nightly binary is downloaded and
# nothing unreviewed is executed.
#
# Reporting is this script's whole job. New signatures cannot be
# *tested* by any frozen toolchain -- exercising them means adopting a
# compiler and a stan-math that know them, which is a reviewed pin
# advance (STANC3_SRC_SHA in tools/dev_setup.sh), not a CI action.
#
# Usage: harnesses/conformance/signature_watch.sh [upstream-ref]
# Exit 0 always, unless a fetch itself fails.
set -euo pipefail

repo_root=$(cd "$(dirname "$0")/../.." && pwd)
upstream_ref=${1:-master}

pinned_sha=$(sed -n 's/^STANC3_SRC_SHA=\([0-9a-f]*\).*/\1/p' \
             "$repo_root/tools/dev_setup.sh")
pinned_repo=$(sed -n 's/^STANC3_SRC_REPO=\([^ ]*\).*/\1/p' \
              "$repo_root/tools/dev_setup.sh")
if [[ -z "$pinned_repo" || -z "$pinned_sha" ]]; then
  echo "Could not read STANC3_SRC_REPO/STANC3_SRC_SHA from tools/dev_setup.sh" >&2
  exit 1
fi

case "$pinned_repo" in
  https://github.com/*.git)
    pinned_slug=${pinned_repo#https://github.com/}
    pinned_slug=${pinned_slug%.git}
    ;;
  *)
    echo "STANC3_SRC_REPO must be an https://github.com/*.git URL" >&2
    exit 1
    ;;
esac
pinned_raw="https://raw.githubusercontent.com/$pinned_slug"
upstream_raw=https://raw.githubusercontent.com/stan-dev/stanc3
file=test/integration/signatures/stan_math_signatures.t
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

# The cram file is the command plus its expected output; the signatures
# are the two-space-indented lines. Everything else is header.
fetch_signatures() { # raw-root ref outfile
  curl -fsSL --retry 5 --retry-all-errors "$1/$2/$file" |
    sed -n 's/^  \([^$].*\)$/\1/p' | sort > "$3"
}
fetch_signatures "$pinned_raw" "$pinned_sha" "$work/pinned"
fetch_signatures "$upstream_raw" "$upstream_ref" "$work/upstream"

added=$(comm -13 "$work/pinned" "$work/upstream")
removed=$(comm -23 "$work/pinned" "$work/upstream")
n_added=$(printf '%s' "$added" | grep -c . || true)
n_removed=$(printf '%s' "$removed" | grep -c . || true)

{
  echo "## Stan signature watch"
  echo
  echo "Pinned stanc3 \`${pinned_sha:0:7}\` vs upstream \`$upstream_ref\`:" \
       "$(wc -l < "$work/pinned" | tr -d ' ') pinned signatures," \
       "**$n_added added**, **$n_removed removed** upstream."
  for section in Added Removed; do
    body=$([ "$section" = Added ] && printf '%s' "$added" ||
           printf '%s' "$removed")
    [ -n "$body" ] || continue
    echo
    echo "### $section upstream"
    echo '```'
    printf '%s\n' "$body"
    echo '```'
  done
  if [ "$n_added" -eq 0 ] && [ "$n_removed" -eq 0 ]; then
    echo
    echo "The pin covers everything upstream offers."
  else
    echo
    echo "To adopt: advance STANC3_SRC_SHA in tools/dev_setup.sh and" \
         "rerun the conformance suite against the new frontend."
  fi
} | tee -a "${GITHUB_STEP_SUMMARY:-/dev/null}"

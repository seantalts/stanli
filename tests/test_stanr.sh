#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 || -z $1 ]]; then
  echo "usage: $0 STANR_GIT_REF" >&2
  exit 2
fi

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
stanr_ref=$1
stanr_repository=${STANR_REPOSITORY:-https://github.com/andrjohns/stanr.git}
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/stanli-stanr.XXXXXX")
trap 'find "$test_dir" -mindepth 1 -delete; rmdir "$test_dir"' EXIT

stanr_dir=$test_dir/stanr
source_stage=$test_dir/source/stanli-current-checkout
r_library=$test_dir/library

# Fetch only the reviewed downstream revision. stanr's repository is large
# because it vendors Stan Math, so a one-commit fetch saves substantial time
# and bandwidth without weakening the source pin.
git init -q "$stanr_dir"
git -C "$stanr_dir" remote add origin "$stanr_repository"
git -C "$stanr_dir" fetch --depth=1 origin "$stanr_ref"
git -C "$stanr_dir" checkout --detach FETCH_HEAD
echo "testing stanr $(git -C "$stanr_dir" rev-parse HEAD)"

# stanr's upgrade script expects a GitHub-style archive with one top-level
# directory. Stage tracked and untracked, non-ignored runtime sources so this
# command also tests a developer's current working tree before it is committed.
mkdir -p "$source_stage"
while IFS= read -r source_path; do
  [[ -f $repo_root/$source_path ]] || continue
  mkdir -p "$source_stage/$(dirname "$source_path")"
  cp "$repo_root/$source_path" "$source_stage/$source_path"
done < <(git -C "$repo_root" ls-files --cached --others --exclude-standard \
  LICENSE runtime/include runtime/src runtime/kernels)

tar -C "$test_dir/source" -czf \
  "$stanr_dir/tools/stanli-current-checkout.tar.gz" \
  stanli-current-checkout
sed -i.bak 's/^STANLI_REF=.*/STANLI_REF="current-checkout"/' \
  "$stanr_dir/tools/upgrade_stanli.sh"
rm "$stanr_dir/tools/upgrade_stanli.sh.bak"
grep -Fqx 'STANLI_REF="current-checkout"' \
  "$stanr_dir/tools/upgrade_stanli.sh"
(
  cd "$stanr_dir/tools"
  ./upgrade_stanli.sh
)

mkdir -p "$r_library"
# R is a native program: under Rtools bash on Windows it needs Windows-style
# paths (bash converts arguments, not environment variables) and a `;`
# library-path separator.
native_path() { if command -v cygpath >/dev/null 2>&1; then cygpath -m "$1"; else printf '%s' "$1"; fi; }
lib_sep=:
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) lib_sep=';' ;; esac
r_library_native=$(native_path "$r_library")
R CMD INSTALL --library="$r_library_native" "$stanr_dir"
R_LIBS_USER="$r_library_native${R_LIBS_USER:+$lib_sep$R_LIBS_USER}" \
  Rscript --vanilla "$repo_root/tests/test_stanr_backend.R" "$stanr_dir"

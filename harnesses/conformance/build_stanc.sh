#!/usr/bin/env bash
# Build the conformance stanc from source at the pinned stanc3 revision.
#
# The old arrangement downloaded a binary from stanc3's `nightly` release
# tag and pinned it by version string and sha256. That tag is reassigned
# on every upstream publish and old assets are deleted with it, so the
# pin was only satisfiable while a CI cache happened to hold a copy; the
# first cold cache after an upstream publish stranded the job with a
# binary that no longer exists anywhere. A git SHA is fetchable forever.
#
# The source is STANC3_SRC_REPO at STANC3_SRC_SHA in tools/dev_setup.sh --
# the same revision the wheels embed -- so the conformance run exercises
# the frontend the shipped runtime actually uses, not a nearby nightly.
#
# Output: deps/stanc3/stanc-pinned, with deps/stanc3/stanc-pinned.src
# recording the revision it was built from. Rerunning with a matching
# provenance file is a no-op, so callers can invoke this unconditionally.
#
# Usage: harnesses/conformance/build_stanc.sh [opam-switch]
set -euo pipefail

repo_root=$(cd "$(dirname "$0")/../.." && pwd)

# Local source builds read the repository, commit, switch, and OCaml version
# from dev_setup.sh. CI mirrors the source pair and asserts that it matches.
read_setup() {
  sed -n "s/^$1=\\([^ ]*\\).*/\\1/p" "$repo_root/tools/dev_setup.sh"
}
switch=${1:-$(read_setup OPAM_SWITCH)}
ocaml_version=$(read_setup OCAML_VERSION)
src_sha=$(read_setup STANC3_SRC_SHA)
src_repo=$(read_setup STANC3_SRC_REPO)
if [[ -z "$src_repo" || -z "$src_sha" ]]; then
  echo "Could not read STANC3_SRC_REPO/STANC3_SRC_SHA from tools/dev_setup.sh" >&2
  exit 1
fi

out="$repo_root/deps/stanc3/stanc-pinned"
if [[ -x "$out" && -f "$out.src" && "$(cat "$out.src")" == "$src_sha" ]]; then
  echo "conformance stanc already built from $src_sha"
  exit 0
fi

src_dir="$repo_root/deps/stanc3-src"
if [[ ! -d "$src_dir/.git" ]]; then
  git clone "$src_repo" "$src_dir"
else
  git -C "$src_dir" remote set-url origin "$src_repo"
fi
git -C "$src_dir" fetch -q origin "$src_sha"
# reset first: dune subst (below) edits tracked files, and a leftover
# subst from a previous build would make this checkout refuse.
git -C "$src_dir" reset -q --hard
git -C "$src_dir" checkout -q --detach "$src_sha"

if [[ ! -d "$HOME/.opam" ]]; then opam init -y --bare --no-setup; fi
if ! opam switch list --short 2>/dev/null | grep -qx "$switch"; then
  # stanc3 pins its OCaml version exactly; other versions fail to solve.
  opam switch create "$switch" "ocaml-base-compiler.$ocaml_version" -y
fi
eval "$(opam env --switch="$switch" --set-switch)"
(cd "$src_dir" && opam install . --deps-only -y)
# Fill the %%NAME%%/%%VERSION%% placeholders from git, as the release
# pipeline would; without this every source build answers --version
# identically and conformance reports cannot tell two pins apart.
(cd "$src_dir" && dune subst)
(cd "$src_dir" && dune build --profile release src/stanc/stanc.exe)

mkdir -p "$repo_root/deps/stanc3"
install -m 755 "$src_dir/_build/default/src/stanc/stanc.exe" "$out"
printf '%s\n' "$src_sha" > "$out.src"
echo "conformance stanc: $out ($("$out" --version)) from $src_sha"

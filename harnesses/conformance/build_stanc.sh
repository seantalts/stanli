#!/usr/bin/env bash
# Build stanc from source at a pinned stanc3 revision.
#
# The old arrangement downloaded a binary from stanc3's `nightly` release
# tag and pinned it by version string and sha256. That tag is reassigned
# on every upstream publish and old assets are deleted with it, so the
# pin was only satisfiable while a CI cache happened to hold a copy; the
# first cold cache after an upstream publish stranded the job with a
# binary that no longer exists anywhere. A git SHA is fetchable forever.
#
# By default this builds CmdStan's compiler, STANC3_UPSTREAM_REPO at
# STANC3_UPSTREAM_SHA in deps/pins.env, the release the fork is based on.
# Every CmdStan reference and the conformance oracle compile with it.
# Output: deps/stanc3/stanc-pinned, from deps/stanc3-upstream-src.
#
# --stanli builds stanli's compiler instead, STANC3_SRC_REPO at
# STANC3_SRC_SHA: the fork the wheels embed, built as stock stanc for the
# MIR fixtures, the signature generators and the packaged rollback.
# Output: deps/stanc3/stanc, from deps/stanc3-src.
#
# Each output has a .src file beside it recording the revision it was built
# from. Rerunning with a matching provenance file is a no-op, so callers can
# invoke this unconditionally.
#
# Usage: harnesses/conformance/build_stanc.sh [--stanli] [opam-switch]
set -euo pipefail

repo_root=$(cd "$(dirname "$0")/../.." && pwd)
source "$repo_root/tools/build_jobs.sh"
BUILD_JOBS=$(stanli_detect_build_jobs)
export OPAMJOBS=$BUILD_JOBS

source "$repo_root/deps/pins.env"
if [[ "${1:-}" == --stanli ]]; then
  shift
  src_sha=$STANC3_SRC_SHA
  src_repo=$STANC3_SRC_REPO
  src_dir="$repo_root/deps/stanc3-src"
  out="$repo_root/deps/stanc3/stanc"
  case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) out=$out.exe ;;
  esac
  record="$repo_root/deps/stanc3/stanc.src"
else
  src_sha=$STANC3_UPSTREAM_SHA
  src_repo=$STANC3_UPSTREAM_REPO
  src_dir="$repo_root/deps/stanc3-upstream-src"
  out="$repo_root/deps/stanc3/stanc-pinned"
  record="$out.src"
fi
switch=${1:-$OPAM_SWITCH}
ocaml_version=$OCAML_VERSION
# dune subst stamps `git describe`, which changes when a release tag for this
# commit is published after an earlier build.
version_matches() {
  [[ -d "$src_dir/.git" ]] || return 0
  git -C "$src_dir" fetch -q --force --tags origin "$src_sha" 2>/dev/null || true
  local described version
  described=$(git -C "$src_dir" describe --always --abbrev=7 "$src_sha") ||
    return 0
  version=$("$out" --version)
  [[ "$version" == "stanc3 $described" || "$version" == "stanc3 $described "* ]]
}
if [[ -x "$out" && -f "$record" && "$(cat "$record")" == "$src_sha" ]] &&
   version_matches; then
  echo "stanc already built from $src_sha"
  exit 0
fi

if [[ ! -d "$src_dir/.git" ]]; then
  git clone "$src_repo" "$src_dir"
else
  git -C "$src_dir" remote set-url origin "$src_repo"
fi
git -C "$src_dir" fetch -q --force --tags origin "$src_sha"
# reset first: dune subst (below) edits tracked files, and a leftover
# subst from a previous build would make this checkout refuse.
git -C "$src_dir" reset -q --hard
git -C "$src_dir" checkout -q --detach "$src_sha"

opam_confirm=yes
opam_init_args=(--bare --no-setup)
opam_root=${OPAMROOT:-$HOME/.opam}
case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*)
    opam_root=$(cygpath -u "$opam_root")
    # Reuse the Unix tools and Git on PATH instead of creating internal Cygwin.
    opam_init_args+=(--cygwin-local-install)
    # Running pacman non-interactively requires unsafe-yes.
    opam_confirm=unsafe-yes
    ;;
esac
if [[ ! -f "$opam_root/config" ]]; then
  opam init --confirm-level="$opam_confirm" "${opam_init_args[@]}"
fi
if ! opam switch list --color=never --short 2>/dev/null | tr -d '\r' | grep -Fqx "$switch"; then
  # stanc3 pins its OCaml version exactly; other versions fail to solve.
  opam switch create "$switch" "ocaml-base-compiler.$ocaml_version" \
    --confirm-level="$opam_confirm"
fi
eval "$(opam env --switch="$switch" --set-switch)"
opam pin add -n dune "$DUNE_VERSION" --confirm-level="$opam_confirm"
(cd "$src_dir" && opam install . --deps-only --confirm-level="$opam_confirm")
# Fill the %%NAME%%/%%VERSION%% placeholders from git, as the release
# pipeline would; without this every source build answers --version
# identically and conformance reports cannot tell two pins apart.
(cd "$src_dir" && dune subst)
(cd "$src_dir" && dune build -j "$BUILD_JOBS" --profile release \
  src/stanc/stanc.exe)

mkdir -p "$repo_root/deps/stanc3"
install -m 755 "$src_dir/_build/default/src/stanc/stanc.exe" "$out"
printf '%s\n' "$src_sha" > "$record"
echo "stanc: $out ($("$out" --version)) from $src_repo $src_sha"

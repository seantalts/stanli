#!/usr/bin/env bash
# One-shot dev environment setup. Safe to re-run; every step is
# idempotent and skipped once its output exists.
#
#   tools/dev_setup.sh               core: vendored deps + cmake builds + tests
#   tools/dev_setup.sh --embed       + OCaml toolchain and in-process stanc3
#   tools/dev_setup.sh --corpus      + posteriordb and the CmdStan verify rig
#   tools/dev_setup.sh --conformance + the Stan conformance reference stack
#   tools/dev_setup.sh --all         everything
#   tools/dev_setup.sh --no-build    stop before cmake (CI builds separately)
#
# Core needs: git, curl, cmake, a C++17 clang, python3.
# --embed adds: opam (OCaml 5.5.0 switch built automatically).
# --corpus adds: ~2 GB of checkouts under deps/ and a CmdStan build.
# --conformance adds: opam, the pinned CmdStan/BridgeStan pair, and a venv
#   holding the version-pinned reference client. It reuses the same
#   deps/cmdstan checkout as --corpus and the same stanc3 source tree and
#   opam switch as --embed, so with either of those already done most of
#   it is a no-op.
set -euo pipefail
cd "$(dirname "$0")/.."
REPO=$PWD

STANC3_SRC_SHA=ac69570adecc41925b4ad72b6b2681c98c09c57d  # matches deps/stanc3/stanc
PDB_SHA=28f8d3d6e975315f42aa274a8399f21e07a43b30
CMDSTAN_SHA=11cb052d3e1fc8c799e0fec559e2ee5452b38d27
OPAM_SWITCH=stanc3-55
OCAML_VERSION=5.5.0

WANT_EMBED=0
WANT_CORPUS=0
WANT_CONFORMANCE=0
WANT_BUILD=1
for arg in "$@"; do
  case "$arg" in
    --embed) WANT_EMBED=1 ;;
    --corpus) WANT_CORPUS=1 ;;
    --conformance) WANT_CONFORMANCE=1 ;;
    --all) WANT_EMBED=1; WANT_CORPUS=1; WANT_CONFORMANCE=1 ;;
    --no-build) WANT_BUILD=0 ;;
    -h|--help) sed -n '2,19p' "$0"; exit 0 ;;
    *) echo "unknown flag: $arg (try --help)"; exit 2 ;;
  esac
done

# The conformance reference client gets its own interpreter rather than the
# host's. Two reasons, and the second is the one that bites: requirements.txt
# pins bridgestan and numpy exactly, for the same reason both sides of the
# differential share one stanc; and the harness reads TOML through tomllib,
# which arrived in 3.11, so on an older system python3 the driver does not
# start at all. Relative to the repo root, which is where every command in
# this script and in the conformance README runs from.
CONFORMANCE_VENV=${CONFORMANCE_VENV:-.venv-conformance}

step() { printf '\n== %s\n' "$*"; }
have() { command -v "$1" >/dev/null 2>&1; }

# --- host prerequisites ----------------------------------------------------
step "checking prerequisites"
missing=()
for tool in git curl cmake python3; do have "$tool" || missing+=("$tool"); done
if ! have clang++ && ! have g++; then missing+=("clang++ (Xcode CLT or clang)"); fi
if [ "$WANT_EMBED" = 1 ] || [ "$WANT_CONFORMANCE" = 1 ]; then
  have opam || missing+=(opam)
fi
if [ ${#missing[@]} -gt 0 ]; then
  if have brew; then
    echo "installing via homebrew: ${missing[*]}"
    for tool in "${missing[@]}"; do
      case "$tool" in
        cmake|opam|git|curl) brew install "$tool" ;;
        *) echo "install manually: $tool"; exit 1 ;;
      esac
    done
  else
    echo "missing: ${missing[*]}"
    echo "install them (apt: sudo apt install ${missing[*]}) and re-run."
    exit 1
  fi
fi
echo "ok: git curl cmake python3 and a C++ compiler present"

# --- vendored headers + stanc binary ---------------------------------------
step "fetching pinned deps (stan-math, stan, stanc3 binary)"
./deps/fetch.sh

# --- embedded stanc3 (optional) --------------------------------------------
if [ "$WANT_EMBED" = 1 ]; then
  step "OCaml toolchain for the embedded stanc3"
  if [ ! -d "$HOME/.opam" ]; then opam init -y --bare; fi
  eval "$(opam env 2>/dev/null || true)"
  if ! opam switch list --short 2>/dev/null | grep -qx "$OPAM_SWITCH"; then
    # stanc3 pins its OCaml version exactly; other versions fail to solve.
    opam switch create "$OPAM_SWITCH" "ocaml-base-compiler.$OCAML_VERSION" -y
  fi

  step "stanc3 source at $STANC3_SRC_SHA"
  if [ ! -d deps/stanc3-src/.git ]; then
    git clone https://github.com/stan-dev/stanc3.git deps/stanc3-src
  fi
  git -C deps/stanc3-src fetch -q origin "$STANC3_SRC_SHA"
  git -C deps/stanc3-src checkout -q "$STANC3_SRC_SHA"

  step "stanc3 OCaml dependencies (switch $OPAM_SWITCH)"
  (cd deps/stanc3-src &&
   opam install . --switch="$OPAM_SWITCH" --deps-only -y)

  step "building the embeddable stanc object"
  if [ ! -f deps/stanc3/stanc_embed.o ]; then
    tools/stanc_embed/build.sh deps/stanc3-src "$OPAM_SWITCH"
  else
    echo "deps/stanc3/stanc_embed.o already present; delete it to rebuild"
  fi
fi

# --- cmake builds ----------------------------------------------------------
if [ "$WANT_BUILD" = 1 ]; then
  step "configuring and building (build/ dev, build-rel/ benchmarks)"
  EMBED_FLAGS=()
  if [ -f deps/stanc3/stanc_embed.o ] && have opam; then
    EMBED_FLAGS=(
      "-DSTANLI_STANC_EMBED_OBJ=$REPO/deps/stanc3/stanc_embed.o"
      "-DSTANLI_OCAML_STDLIB=$(opam var --switch="$OPAM_SWITCH" lib 2>/dev/null)/ocaml"
    )
  fi
  cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    ${EMBED_FLAGS[@]+"${EMBED_FLAGS[@]}"}
  cmake --build build -j8
  cmake -B build-rel -DCMAKE_BUILD_TYPE=Release
  cmake --build build-rel -j8 --target bench_grad stanli_run

  step "running tests"
  ctest --test-dir build --output-on-failure
else
  step "--no-build: skipping cmake and tests"
fi

# --- corpus + differential verification rig (optional) ---------------------
if [ "$WANT_CORPUS" = 1 ]; then
  step "posteriordb at $PDB_SHA (deps/posteriordb)"
  if [ ! -d deps/posteriordb/.git ]; then
    git clone https://github.com/stan-dev/posteriordb.git deps/posteriordb
  fi
  git -C deps/posteriordb fetch -q origin "$PDB_SHA"
  git -C deps/posteriordb checkout -q "$PDB_SHA"

  step "CmdStan at $CMDSTAN_SHA (deps/cmdstan; used by tools/verify_sample.py)"
  if [ ! -d deps/cmdstan/.git ]; then
    git clone https://github.com/stan-dev/cmdstan.git deps/cmdstan
  fi
  git -C deps/cmdstan fetch -q origin "$CMDSTAN_SHA"
  git -C deps/cmdstan checkout -q "$CMDSTAN_SHA"
  git -C deps/cmdstan submodule update --init --recursive --quiet

  if [ ! -f deps/cmdstan/stan/lib/stan_math/lib/tbb/libtbb.dylib ] &&
     [ ! -f deps/cmdstan/stan/lib/stan_math/lib/tbb/libtbb.so.2 ]; then
    step "building CmdStan (one-time; provides TBB + the bench comparator)"
    make -C deps/cmdstan -j8 build
  else
    echo "CmdStan already built"
  fi

  step "corpus scoreboard"
  python3 tools/corpus.py deps/posteriordb || true
fi

# --- Stan conformance reference stack (optional) ---------------------------
# The nightly sweep's oracle, locally: the pinned stanc built from source,
# the exact CmdStan/Stan/Stan Math triple BridgeStan compiles against, and
# the pinned reference client. fetch_cmdstan.sh already does the first three
# and already refuses to proceed on a pin mismatch, so this adds only the
# client and the staged runtime -- and every part of it is a no-op on the
# second run. The cost people remember is the one-time stanc build; it is
# once per machine per pin, not once per session, and --embed pays it too.
if [ "$WANT_CONFORMANCE" = 1 ]; then
  step "conformance reference toolchain (stanc from source, CmdStan, BridgeStan)"
  ./harnesses/conformance/fetch_cmdstan.sh

  step "conformance reference client ($CONFORMANCE_VENV)"
  [ -d "$CONFORMANCE_VENV" ] || python3 -m venv "$CONFORMANCE_VENV"
  "$CONFORMANCE_VENV/bin/pip" install -q --disable-pip-version-check \
    -r harnesses/conformance/requirements.txt
  echo "reference client ready under $("$CONFORMANCE_VENV/bin/python" -V)"

  # The harness never loads anything out of the build tree; it drives stanli
  # through the public Python package, which finds its library and its stanc
  # in python/stanli/_bin. Staging those is what makes the printed command
  # work straight after setup, and it is the same pair the nightly stages.
  # Restage after every rebuild: an unstaged rebuild silently measures the
  # library from before it.
  if [ "$WANT_BUILD" = 1 ]; then
    step "staging the runtime the harness drives (python/stanli/_bin)"
    cmake --build build-rel -j8 --target stanli_shared
    LIB=""
    for candidate in build-rel/libstanli.dylib build-rel/libstanli.so \
                     build-rel/stanli.dll; do
      if [ -f "$candidate" ]; then LIB=$candidate; break; fi
    done
    [ -n "$LIB" ] || { echo "no shared library in build-rel/"; exit 1; }
    mkdir -p python/stanli/_bin
    cp "$LIB" python/stanli/_bin/
    cp deps/stanc3/stanc-pinned python/stanli/_bin/stanc
    chmod +x python/stanli/_bin/stanc
  else
    step "--no-build: stage python/stanli/_bin before running the harness"
    echo "  cmake --build build-rel --target stanli_shared"
    echo "  cp build-rel/libstanli.* python/stanli/_bin/"
    echo "  cp deps/stanc3/stanc-pinned python/stanli/_bin/stanc"
  fi
fi

step "done"
echo "dev build:   build/            (tests: ctest --test-dir build)"
echo "bench build: build-rel/        (tools/bench_grad.cpp)"
echo "corpus:      python3 tools/corpus.py deps/posteriordb"
echo "verify:      python3 tools/verify_sample.py deps/cmdstan deps/posteriordb MODEL..."
echo "wheel:       tools/build_wheel.sh"
if [ "$WANT_CONFORMANCE" = 1 ]; then
  # Run the driver under the venv interpreter, not the host's: the harness
  # needs tomllib, and --stanli-python then defaults to the same interpreter,
  # which is where the pinned BridgeStan client lives.
  cat <<EOF

conformance: one case, from the repo root, to prove the stack end to end:

  $CONFORMANCE_VENV/bin/python harnesses/stan_conformance.py \\
    --stanc deps/stanc3/stanc-pinned --cmdstan deps/cmdstan \\
    --build build-rel --stanli-pythonpath python \\
    --case 'abs(real)=>real'

Swap --case for --filter SUBSTR to take a slice, or --shard N/M for a
stable partition. A selected run reports "gate issues: partial_run"; that
records the scope, not a failure. See harnesses/conformance/README.md.
EOF
fi

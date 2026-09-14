#!/usr/bin/env bash
# Fetch pinned dependencies into deps/. Safe to re-run.
set -euo pipefail
cd "$(dirname "$0")"

MATH_SHA=8f326d14599d3030c626c46532d8e8534c1cdbec
STAN_SHA=c96d04115d35cb04f42e45c5a69a82f9704798f1
FMT_SHA=40626af88bd7df9a5fb80be7b25ac85b122d6c21 # 11.2.0, CLI CSV formatting

fetch() { # name url sha sparse-paths...
  local name=$1 url=$2 sha=$3
  shift 3
  if [ ! -d "$name/.git" ]; then
    git clone --filter=blob:none --no-checkout "$url" "$name"
    git -C "$name" sparse-checkout init --cone
  fi
  git -C "$name" sparse-checkout set "$@"
  git -C "$name" fetch -q origin "$sha"
  git -C "$name" checkout -q -f "$sha"
}

fetch math https://github.com/stan-dev/math.git "$MATH_SHA" stan lib
fetch stan https://github.com/stan-dev/stan.git "$STAN_SHA" src/stan lib/rapidjson_1.1.0
fetch fmt https://github.com/fmtlib/fmt.git "$FMT_SHA" include

# The adjoint ODE's backward quadrature callback accumulates into the CVODES
# output vector without assigning it first, so it inherits whatever the
# vector held: uninitialized memory on the first call, the previous step's
# correction after that.
git -C math apply <<'EOF'
--- a/stan/math/rev/functor/cvodes_integrator_adjoint.hpp
+++ b/stan/math/rev/functor/cvodes_integrator_adjoint.hpp
@@ -641,6 +641,7 @@ class cvodes_integrator_adjoint_vari : public vari_base {
                      f_y_t_vars.size(), "states", N_);
     f_y_t_vars.adj() = -Eigen::Map<Eigen::VectorXd>(NV_DATA_S(yB), N_);
     grad();
+    Eigen::Map<Eigen::VectorXd>(NV_DATA_S(qBdot), num_args_vars_).setZero();
     math::apply(
         [&qBdot](auto&&... args) {
           accumulate_adjoints(NV_DATA_S(qBdot), args...);
EOF

# Do not fetch stanc3's moving `nightly` release here. It is replaced in
# place, so its URL cannot identify the compiler bytes a release used.
# tools/dev_setup.sh builds the native compiler from STANC3_SRC_SHA when a
# requested mode needs it; the wheel workflow does the corresponding source
# build for Windows.
STANC3_SRC_SHA=$(sed -n 's/^STANC3_SRC_SHA=\([0-9a-f]*\).*/\1/p' \
                   ../tools/dev_setup.sh)
if [ -z "$STANC3_SRC_SHA" ]; then
  echo "could not read STANC3_SRC_SHA from tools/dev_setup.sh" >&2
  exit 1
fi
if [ -e stanc3/stanc ] &&
   { [ ! -x stanc3/stanc ] ||
     [ "$(cat stanc3/stanc.src 2>/dev/null || true)" != "$STANC3_SRC_SHA" ];
   }; then
  echo "discarding stanc without $STANC3_SRC_SHA provenance"
  rm -f stanc3/stanc stanc3/stanc.src
fi

echo "deps ready: math@$MATH_SHA stan@$STAN_SHA fmt@$FMT_SHA"

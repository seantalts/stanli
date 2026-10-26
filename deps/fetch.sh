#!/usr/bin/env bash
# Fetch pinned dependencies into deps/. Safe to re-run.
set -euo pipefail
cd "$(dirname "$0")"

source ./pins.env

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

# libc++ 23's private namespace macros now attach function ABI attributes.
# Boost's type-only forward declarations misuse those macros and warn in every
# consumer. Use the public standard headers for these declarations instead.
git -C math apply <<'EOF'
--- a/lib/boost_1.87.0/boost/container/detail/std_fwd.hpp
+++ b/lib/boost_1.87.0/boost/container/detail/std_fwd.hpp
@@ -23,6 +23,13 @@
 //                        Standard predeclarations
 //////////////////////////////////////////////////////////////////////////////

+#if defined(_LIBCPP_VERSION)
+#include <functional>
+#include <iterator>
+#include <memory>
+#include <string>
+#include <utility>
+#else
 #include <boost/move/detail/std_ns_begin.hpp>
 BOOST_MOVE_STD_NS_BEG

@@ -59,5 +66,6 @@ struct pointer_traits;

 BOOST_MOVE_STD_NS_END
 #include <boost/move/detail/std_ns_end.hpp>
+#endif

 #endif //#ifndef BOOST_CONTAINER_DETAIL_STD_FWD_HPP
--- a/lib/boost_1.87.0/boost/intrusive/detail/std_fwd.hpp
+++ b/lib/boost_1.87.0/boost/intrusive/detail/std_fwd.hpp
@@ -23,6 +23,10 @@
 //                        Standard predeclarations
 //////////////////////////////////////////////////////////////////////////////

+#if defined(_LIBCPP_VERSION)
+#include <functional>
+#include <iterator>
+#else
 #include <boost/move/detail/std_ns_begin.hpp>
 BOOST_MOVE_STD_NS_BEG

@@ -42,5 +46,6 @@ struct random_access_iterator_tag;

 BOOST_MOVE_STD_NS_END
 #include <boost/move/detail/std_ns_end.hpp>
+#endif

 #endif //#ifndef BOOST_INTRUSIVE_DETAIL_STD_FWD_HPP
--- a/lib/boost_1.87.0/boost/move/detail/iterator_traits.hpp
+++ b/lib/boost_1.87.0/boost/move/detail/iterator_traits.hpp
@@ -58,6 +58,12 @@
 #include <cstddef>
 #include <boost/move/detail/type_traits.hpp>

+#if defined(_LIBCPP_VERSION)
+#include <iterator>
+#if _LIBCPP_STD_VER > 17
+#define BOOST_MOVE_CONTIGUOUS_ITERATOR_TAG
+#endif
+#else
 #include <boost/move/detail/std_ns_begin.hpp>

 BOOST_MOVE_STD_NS_BEG
@@ -82,6 +88,7 @@ struct contiguous_iterator_tag;
 BOOST_MOVE_STD_NS_END

 #include <boost/move/detail/std_ns_end.hpp>
+#endif

 namespace boost{  namespace movelib{

EOF

# Copy the output adjoints into their owning matrix before the triangular
# solve. Eigen's solve assignment performs this same copy and in-place solve,
# but first compares the destination with the non-owning adjoint view. GCC 14
# diagnoses a possible free of that view's interior pointer after inlining the
# alias check. The fresh matrix cannot alias the arena-backed adjoints.
git -C math apply <<'EOF'
--- a/stan/math/rev/fun/mdivide_left_tri.hpp
+++ b/stan/math/rev/fun/mdivide_left_tri.hpp
@@ -74,12 +74,12 @@
   virtual void chain() {
     using Eigen::Map;
     matrix_d adjA;
-    matrix_d adjB;
+    matrix_d adjB = Map<matrix_vi>(variRefC_, M_, N_).adj();

-    adjB = Map<matrix_d>(A_, M_, M_)
-               .template triangularView<TriView>()
-               .transpose()
-               .solve(Map<matrix_vi>(variRefC_, M_, N_).adj());
+    Map<matrix_d>(A_, M_, M_)
+        .template triangularView<TriView>()
+        .transpose()
+        .solveInPlace(adjB);
     adjA = -adjB * Map<matrix_d>(C_, M_, N_).transpose();

     size_t pos = 0;
EOF

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

# grad() reloads the thread-local stack pointer after every virtual chain()
# call. In a Linux shared library each reload is a TLS lookup. Load it once.
git -C math apply <<'EOF'
--- a/stan/math/rev/core/grad.hpp
+++ b/stan/math/rev/core/grad.hpp
@@ -26,8 +26,9 @@ namespace math {
 static inline void grad() {
   size_t end = ChainableStack::instance_->var_stack_.size();
   size_t beginning = empty_nested() ? 0 : end - nested_size();
+  auto* const stack = ChainableStack::instance_;
   for (size_t i = end; i-- > beginning;) {
-    ChainableStack::instance_->var_stack_[i]->chain();
+    stack->var_stack_[i]->chain();
   }
 }

EOF

# Do not fetch stanc3's moving `nightly` release here. It is replaced in
# place, so its URL cannot identify the compiler bytes a release used.
# tools/dev_setup.sh builds the native compiler from STANC3_SRC_SHA when a
# requested mode needs it; the wheel workflow does the corresponding source
# build for Windows.
if [ -e stanc3/stanc ] &&
   { [ ! -x stanc3/stanc ] ||
     [ "$(cat stanc3/stanc.src 2>/dev/null || true)" != "$STANC3_SRC_SHA" ];
   }; then
  echo "discarding stanc without $STANC3_SRC_SHA provenance"
  rm -f stanc3/stanc stanc3/stanc.src
fi

echo "deps ready: math@$MATH_SHA stan@$STAN_SHA fmt@$FMT_SHA"

// A stan-math autodiff scalar that records value and partials into plain
// double buffers instead of building vari nodes or contracting tangents.
//
// prim/prob density functions compute their values and partials entirely in
// doubles; the autodiff scalar type only selects which partials to fill and
// what build() does with them. So rvar needs no arithmetic operators at all:
// registering the traits below routes the unmodified stan-math templates
// through our partials_propagator specialization, whose build() deposits the
// partials into the active sink.
//
// rvar is registered through stan::is_fvar. A dedicated is_rvar trait would
// need a one-line extension of stan's is_autodiff_scalar, which we avoid
// while stan-math is vendored read-only. No fvar code paths are otherwise
// reachable: rvar has no .d_ member, and any template that tries to use one
// fails to compile rather than misbehaving.
#ifndef STANLI_RECORDER_HPP
#define STANLI_RECORDER_HPP

#include <stanli/graph.hpp>

#include <stan/math/prim/meta.hpp>
#include <stan/math/prim/fun/Eigen.hpp>
#include <stan/math/prim/functor/operands_and_partials.hpp>
#include <stan/math/prim/functor/partials_propagator.hpp>
#include <stan/math/prim/functor/broadcast_array.hpp>

#include <cstddef>
#include <ostream>
#include <type_traits>

namespace stanli {

// The recording scalar. Constructible from double (densities early-return
// literal 0.0) but not convertible to double, so stan::is_constant<rvar>
// is false without a specialization.
struct rvar {
  // We register is_fvar<rvar>, and stan-math's fvar contract includes a
  // Scalar member: its value_type specialization is written
  // `typename std::decay_t<T>::Scalar` for anything is_fvar accepts, and
  // that applies to `const rvar&` as much as to `rvar`. Specializing
  // value_type for the bare type only, as this header used to, left every
  // cv-ref form reaching for a member that did not exist -- which is what
  // kept ordered_logistic and skew_double_exponential's cdfs out. Honour
  // the trait instead of patching its consumers one at a time.
  using Scalar = double;
  double val_{0};
  rvar() = default;
  rvar(double v) : val_(v) {}  // NOLINT: implicit on purpose
  // scalar_seq_view and check-message streaming touch these.
  double val() const { return val_; }
  friend std::ostream& operator<<(std::ostream& os, const rvar& v);
};

inline std::ostream& operator<<(std::ostream& os, const rvar& v) {
  return os << v.val_;
}

static_assert(sizeof(rvar) == sizeof(double), "rvar must alias double");
static_assert(alignof(rvar) == alignof(double), "rvar must alias double");
static_assert(std::is_standard_layout_v<rvar>, "rvar must alias double");
static_assert(std::is_trivially_copyable_v<rvar>, "rvar must alias double");

// Zero-copy promotion: view a double buffer as a column vector of rvar.
// Layout compatibility is asserted above; this is what lets one all-rvar
// kernel instantiation serve data and parameter arguments alike.
inline Eigen::Map<const Eigen::Matrix<rvar, -1, 1>> as_rvar(const Desc& d) {
  return Eigen::Map<const Eigen::Matrix<rvar, -1, 1>>(
      reinterpret_cast<const rvar*>(d.data), d.len);
}

// Where build() deposits partials: one buffer per propagator edge, in
// operand order. A null buf skips that edge's copy-out (its length is still
// reported). The executor points these at per-op scratch.
struct sink {
  static constexpr int kMaxEdges = 8;
  double* buf[kMaxEdges]{};
  int len[kMaxEdges]{};
  double value{0};
};

inline sink*& active_sink() {
  static thread_local sink* s = nullptr;
  return s;
}

}  // namespace stanli

namespace stan {

// Trait registration: rvar is an autodiff scalar whose partials are double.
template <>
struct is_fvar<stanli::rvar, void> : std::true_type {};

template <>
struct partials_type<stanli::rvar, void> {
  using type = double;
};

template <>
struct scalar_type<stanli::rvar, void> {
  using type = stanli::rvar;
};

template <>
struct base_type<stanli::rvar, void> {
  using type = stanli::rvar;
};

template <>
struct value_type<stanli::rvar, void> {
  using type = double;
};

namespace math {
inline double value_of(const stanli::rvar& v) { return v.val_; }
inline double value_of_rec(const stanli::rvar& v) { return v.val_; }
}  // namespace math

namespace math {
namespace internal {

// Edges accumulate the double partials the density computes. Scalar edges
// hold one double behind a broadcast_array (a density assigning a length-1
// expression collapses onto element 0, as the fwd/rev edges do).
template <typename ViewElt>
class ops_partials_edge<ViewElt, stanli::rvar, void> {
 public:
  double partial_{0};
  broadcast_array<double> partials_{partial_};

  ops_partials_edge() = default;
  explicit ops_partials_edge(const stanli::rvar& op)
      : partial_(0), partials_(partial_), operands_(op) {}
  ops_partials_edge(const ops_partials_edge& o)
      : partial_(o.partial_), partials_(partial_), operands_(o.operands_) {}

  stanli::rvar operands_{};

  int size() const { return 1; }
  void emit(double* dst) const { dst[0] = partial_; }
};

template <typename ViewElt, typename Op>
class ops_partials_edge<ViewElt, Op, require_eigen_vt<is_fvar, Op>> {
 public:
  using partials_t = Eigen::Array<double, -1, 1>;
  partials_t partials_;
  broadcast_array<partials_t> partials_vec_{partials_};

  template <typename OpT, require_eigen_vt<is_fvar, OpT>* = nullptr>
  explicit ops_partials_edge(const OpT& ops)
      : partials_(partials_t::Zero(ops.size())), size_(ops.size()) {}
  ops_partials_edge(const ops_partials_edge& o)
      : partials_(o.partials_), partials_vec_(partials_), size_(o.size_) {}

  Eigen::Index size_{0};
  int size() const { return static_cast<int>(size_); }
  void emit(double* dst) const {
    for (Eigen::Index i = 0; i < size_; ++i) dst[i] = partials_(i);
  }
};

template <typename ViewElt, typename Op>
class ops_partials_edge<ViewElt, Op, require_std_vector_vt<is_fvar, Op>> {
 public:
  using partials_t = Eigen::Array<double, -1, 1>;
  partials_t partials_;
  broadcast_array<partials_t> partials_vec_{partials_};

  explicit ops_partials_edge(const Op& ops)
      : partials_(partials_t::Zero(ops.size())), size_(ops.size()) {}
  ops_partials_edge(const ops_partials_edge& o)
      : partials_(o.partials_), partials_vec_(partials_), size_(o.size_) {}

  std::size_t size_{0};
  int size() const { return static_cast<int>(size_); }
  void emit(double* dst) const {
    for (std::size_t i = 0; i < size_; ++i) dst[i] = partials_(i);
  }
};

// Data operands select stan-math's own arithmetic edge, which carries no
// partials; detect and skip.
template <typename E, typename = void>
struct rt_has_emit : std::false_type {};
template <typename E>
struct rt_has_emit<E, std::void_t<decltype(std::declval<const E&>().emit(
                          std::declval<double*>()))>> : std::true_type {};

// Selected whenever return_type_t<Ops...> is rvar. Unlike the fwd
// specialization, build() copies partials out verbatim instead of
// contracting them against a tangent.
template <typename... Ops>
class partials_propagator<stanli::rvar, void, Ops...> {
 public:
  std::tuple<ops_partials_edge<double, std::decay_t<Ops>>...> edges_;

  template <typename... Types>
  explicit partials_propagator(Types&&... ops)
      : edges_(ops_partials_edge<double, std::decay_t<Ops>>(ops)...) {}

  stanli::rvar build(double value) {
    stanli::sink* s = stanli::active_sink();
    if (s != nullptr) {
      s->value = value;
      emit_all(s, std::index_sequence_for<Ops...>{});
    }
    return stanli::rvar(value);
  }

 private:
  template <std::size_t I>
  void emit_one(stanli::sink* s) {
    using E = std::tuple_element_t<I, decltype(edges_)>;
    if constexpr (rt_has_emit<E>::value) {
      s->len[I] = std::get<I>(edges_).size();
      if (s->buf[I] != nullptr) std::get<I>(edges_).emit(s->buf[I]);
    } else {
      s->len[I] = 0;  // data operand: no partials
    }
  }

  template <std::size_t... I>
  void emit_all(stanli::sink* s, std::index_sequence<I...>) {
    (void)std::initializer_list<int>{(emit_one<I>(s), 0)...};
  }
};

}  // namespace internal
}  // namespace math
}  // namespace stan

#endif

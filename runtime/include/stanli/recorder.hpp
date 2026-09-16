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

#include <stanli/kernel_types.hpp>

#include <stan/math/prim/meta.hpp>
#include <stan/math/prim/fun/Eigen.hpp>
#include <stan/math/prim/functor/operands_and_partials.hpp>
#include <stan/math/prim/functor/partials_propagator.hpp>
#include <stan/math/prim/functor/broadcast_array.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <ostream>
#include <type_traits>
#include <utility>
#include <vector>

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

// vector_seq_view lives in namespace stan and finds scalar-specific value
// extraction by ADL. Keep this in rvar's namespace, just as var's overloads
// are in var's namespace. The view is consumed in the caller's expression.
template <typename T,
          std::enable_if_t<std::is_same_v<typename T::Scalar, rvar>, int> = 0>
inline auto value_of(const T& values) {
  return values.unaryExpr([](const rvar& value) { return value.val(); });
}

// The same promotion for a matrix operand (a GLM's parameter-dependent
// design matrix). Column-major, like every kernel matrix map.
inline Eigen::Map<const Eigen::Matrix<rvar, -1, -1>> as_rvar_matrix(
    const Desc& d, int64_t rows, int64_t cols) {
  return Eigen::Map<const Eigen::Matrix<rvar, -1, -1>>(
      reinterpret_cast<const rvar*>(d.data), rows, cols);
}

// Where build() deposits partials: one buffer per propagator edge, in
// operand order. A null buf skips that edge's copy-out. len is the configured
// buffer width; it lets a probability function that returns before build()
// explicitly record a zero pullback. The executor points these at per-op
// scratch.
struct sink {
  static constexpr int kMaxEdges = 8;
  double* buf[kMaxEdges]{};
  int64_t len[kMaxEdges]{};
  double value{0};
  bool deposited{false};
  // Optional forward-to-reverse topology bit: 1 when build() created edges,
  // 0 when the probability function returned a disconnected scalar.
  double* connected{nullptr};
};

inline sink*& active_sink() {
  static thread_local sink* s = nullptr;
  return s;
}

// Recorder calls can throw on invalid domains. Restore the preceding
// thread-local sink on every exit so nested or subsequent calls never see a
// pointer to a dead stack object.
class sink_scope {
 public:
  explicit sink_scope(sink& current) : previous_(active_sink()) {
    active_sink() = &current;
  }
  ~sink_scope() { active_sink() = previous_; }
  sink_scope(const sink_scope&) = delete;
  sink_scope& operator=(const sink_scope&) = delete;

 private:
  sink* previous_;
};

inline double recorded_value(double value) { return value; }
inline double recorded_value(const rvar& value) { return value.val(); }

// Some Stan Math probability functions return a constant (typically
// LOG_ZERO or zero for an empty input) before constructing their partials
// propagator. Capture the function's return as well as build(): when build()
// did not deposit, the return is the value and every partial is zero.
template <typename F>
inline void record_probability_call(F&& f) {
  sink* s = active_sink();
  if (s != nullptr) s->deposited = false;
  const auto result = std::forward<F>(f)();
  if (s == nullptr) return;
  if (s->connected != nullptr) *s->connected = s->deposited ? 1.0 : 0.0;
  if (s->deposited) return;
  s->value = recorded_value(result);
  for (int k = 0; k < sink::kMaxEdges; ++k)
    if (s->buf[k] != nullptr)
      std::fill_n(s->buf[k], static_cast<std::size_t>(s->len[k]), 0.0);
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
class ops_partials_edge<ViewElt, Op, require_eigen_vector_vt<is_fvar, Op>> {
 public:
  using partials_t = Eigen::Map<Eigen::Array<double, -1, 1>>;
  Eigen::Index size_{0};
  bool owns_partials_{true};
  std::vector<double> owned_;
  partials_t partials_;
  // Stan Math's vector-sequence partials accept matrix expressions, while
  // scalar-vectorized densities use the array view above. Both alias the same
  // storage and copy construction must rebind both views to the new edge.
  Eigen::MatrixWrapper<partials_t> vector_partials_{partials_};
  broadcast_array<Eigen::MatrixWrapper<partials_t>> partials_vec_{
      vector_partials_};

  template <typename OpT, require_eigen_vt<is_fvar, OpT>* = nullptr>
  explicit ops_partials_edge(const OpT& ops, std::size_t idx)
      : ops_partials_edge(slot_for(idx, ops.size()), ops.size()) {}
  ops_partials_edge(const ops_partials_edge& o)
      : size_(o.size_),
        owns_partials_(o.owns_partials_),
        owned_(o.owns_partials_ ? o.owned_ : std::vector<double>()),
        partials_(o.owns_partials_ ? owned_.data()
                                   : const_cast<double*>(o.partials_.data()),
                  o.size_) {}

  int size() const { return static_cast<int>(size_); }
  void emit(double* dst) const {
    if (partials_.data() == dst) return;
    for (Eigen::Index i = 0; i < size_; ++i) dst[i] = partials_(i);
  }

 private:
  static double* slot_for(std::size_t idx, Eigen::Index n) {
    stanli::sink* s = stanli::active_sink();
    if (s != nullptr && s->buf[idx] != nullptr && s->len[idx] == n)
      return s->buf[idx];
    return nullptr;
  }
  ops_partials_edge(double* slot, Eigen::Index n)
      : size_(n),
        owns_partials_(slot == nullptr),
        owned_(owns_partials_ ? static_cast<std::size_t>(n) : std::size_t{0}),
        partials_(owns_partials_ ? owned_.data() : slot, n) {
    partials_.setZero();
  }
};

// Matrix operands (a GLM's parameter-dependent design matrix). The partials
// keep the operand's two-dimensional shape so stan-math's matrix-shaped
// derivative assignments dimension-check, and they sit directly on the sink
// slot column-major -- the same layout the kernel's input buffer uses, so
// emit is the identity whenever the slot matched.
template <typename ViewElt, typename Op>
class ops_partials_edge<ViewElt, Op,
                        require_eigen_matrix_dynamic_vt<is_fvar, Op>> {
 public:
  using partials_t = Eigen::Map<Eigen::Array<double, -1, -1>>;
  Eigen::Index rows_{0};
  Eigen::Index cols_{0};
  bool owns_partials_{true};
  std::vector<double> owned_;
  partials_t partials_;
  broadcast_array<partials_t> partials_vec_{partials_};

  template <typename OpT,
            require_eigen_matrix_dynamic_vt<is_fvar, OpT>* = nullptr>
  explicit ops_partials_edge(const OpT& ops, std::size_t idx)
      : ops_partials_edge(slot_for(idx, ops.size()), ops.rows(), ops.cols()) {}
  ops_partials_edge(const ops_partials_edge& o)
      : rows_(o.rows_),
        cols_(o.cols_),
        owns_partials_(o.owns_partials_),
        owned_(o.owns_partials_ ? o.owned_ : std::vector<double>()),
        partials_(o.owns_partials_ ? owned_.data()
                                   : const_cast<double*>(o.partials_.data()),
                  o.rows_, o.cols_) {}

  int size() const { return static_cast<int>(rows_ * cols_); }
  void emit(double* dst) const {
    if (partials_.data() == dst) return;
    for (Eigen::Index i = 0; i < rows_ * cols_; ++i)
      dst[i] = partials_.data()[i];
  }

 private:
  static double* slot_for(std::size_t idx, Eigen::Index n) {
    stanli::sink* s = stanli::active_sink();
    if (s != nullptr && s->buf[idx] != nullptr && s->len[idx] == n)
      return s->buf[idx];
    return nullptr;
  }
  ops_partials_edge(double* slot, Eigen::Index rows, Eigen::Index cols)
      : rows_(rows),
        cols_(cols),
        owns_partials_(slot == nullptr),
        owned_(owns_partials_ ? static_cast<std::size_t>(rows * cols)
                              : std::size_t{0}),
        partials_(owns_partials_ ? owned_.data() : slot, rows, cols) {
    partials_.setZero();
  }
};

template <typename ViewElt, typename Op>
class ops_partials_edge<ViewElt, Op, require_std_vector_vt<is_fvar, Op>> {
 public:
  using partials_t = Eigen::Map<Eigen::Array<double, -1, 1>>;
  std::size_t size_{0};
  bool owns_partials_{true};
  std::vector<double> owned_;
  partials_t partials_;
  broadcast_array<partials_t> partials_vec_{partials_};

  explicit ops_partials_edge(const Op& ops, std::size_t idx)
      : ops_partials_edge(slot_for(idx, ops.size()), ops.size()) {}
  ops_partials_edge(const ops_partials_edge& o)
      : size_(o.size_),
        owns_partials_(o.owns_partials_),
        owned_(o.owns_partials_ ? o.owned_ : std::vector<double>()),
        partials_(o.owns_partials_ ? owned_.data()
                                   : const_cast<double*>(o.partials_.data()),
                  o.size_) {}

  int size() const { return static_cast<int>(size_); }
  void emit(double* dst) const {
    if (partials_.data() == dst) return;
    for (std::size_t i = 0; i < size_; ++i) dst[i] = partials_(i);
  }

 private:
  static double* slot_for(std::size_t idx, std::size_t n) {
    stanli::sink* s = stanli::active_sink();
    if (s != nullptr && s->buf[idx] != nullptr &&
        s->len[idx] == static_cast<int64_t>(n))
      return s->buf[idx];
    return nullptr;
  }
  ops_partials_edge(double* slot, std::size_t n)
      : size_(n),
        owns_partials_(slot == nullptr),
        owned_(owns_partials_ ? n : std::size_t{0}),
        partials_(owns_partials_ ? owned_.data() : slot,
                  static_cast<Eigen::Index>(n)) {
    partials_.setZero();
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
      : edges_(build_edges(std::index_sequence_for<Ops...>{},
                           std::forward<Types>(ops)...)) {}

  stanli::rvar build(double value) {
    stanli::sink* s = stanli::active_sink();
    if (s != nullptr) {
      s->deposited = true;
      s->value = value;
      emit_all(s, std::index_sequence_for<Ops...>{});
    }
    return stanli::rvar(value);
  }

 private:
  template <std::size_t I, typename OpT>
  static auto make_edge(OpT&& op) {
    using E = std::tuple_element_t<I, decltype(edges_)>;
    if constexpr (std::is_constructible_v<E, OpT&&, std::size_t>) {
      return E(std::forward<OpT>(op), I);
    } else {
      return E(std::forward<OpT>(op));
    }
  }

  template <typename... Types, std::size_t... I>
  static std::tuple<ops_partials_edge<double, std::decay_t<Ops>>...>
  build_edges(std::index_sequence<I...>, Types&&... ops) {
    return {make_edge<I>(std::forward<Types>(ops))...};
  }

  template <std::size_t I>
  void emit_one(stanli::sink* s) {
    using E = std::tuple_element_t<I, decltype(edges_)>;
    if constexpr (rt_has_emit<E>::value) {
      auto& e = std::get<I>(edges_);
      if (s->buf[I] != nullptr && e.size() == s->len[I]) e.emit(s->buf[I]);
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

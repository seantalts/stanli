// stanli_check --interp: the whole model through the MIR interpreter on
// doubles, as the reference evaluator for the compiled path.
//
// Data are bound and checked as the lowering binds and checks them, and
// transformed data runs on MirInterp<double> with the lowering's hooks. The
// log density runs on MirInterp<double> as well. Each parameter is read
// through Stan's deserializer with the Jacobian, in CmdStan's unconstrained
// order, as the declared container type with its bounds. The gradient is a
// central finite difference of that value, so no derivative comes from
// stanli's kernels. The constrained values then feed WaInterp for the
// write_array row. Nothing here reads the compiled graph.
#ifndef STANLI_TOOLS_INTERP_ORACLE_HPP
#define STANLI_TOOLS_INTERP_ORACLE_HPP

#include <stanli/compile.hpp>
#include <stanli/data.hpp>
#include <stanli/higher_order_eval.hpp>
#include <stanli/message_sink.hpp>
#include <stanli/mir.hpp>
#include <stanli/mir_decode.hpp>
#include <stanli/mir_interp.hpp>
#include <stanli/unconstrain.hpp>
#include <stanli/wa_interp.hpp>

#include "../runtime/src/mir_tuples.hpp"

#include <stan/io/deserializer.hpp>
#include <stan/math.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace stanli::oracle {

using Entry = DataMap::Entry;
using Value = MirInterp<double>::Value;
using Deserializer = stan::io::deserializer<double>;

struct Refused : CompileError {
  using CompileError::CompileError;
};

inline bool int_declared(const mir::SizedType& t) {
  return t.base == "SInt" || (t.base == "SArray" && t.elem_base == "SInt");
}

inline int leaf_rank_of(const mir::SizedType& t) {
  const std::string& b = t.base == "SArray" ? t.elem_base : t.base;
  if (b == "SVector" || b == "SRowVector") return 1;
  if (b == "SMatrix") return 2;
  if (b == "SReal" || b == "SInt") return 0;
  throw Refused("oracle: parameter of type " + b + " is not supported");
}

// One parameter's shape: the outer array dims, then the leaf's.
struct ParamGeom {
  std::vector<int64_t> dims;
  size_t outer = 0;
  std::vector<int64_t> leaf;
  int64_t leaf_len = 1, n_leaves = 1, free_leaf = 1;
  int64_t free_len() const { return n_leaves * free_leaf; }
};

inline ParamGeom geometry(MirInterp<double>& in, const mir::SizedType& type,
                          const mir::Transform& tr) {
  if (mir::is_complex(type))
    throw Refused("oracle: complex parameters are not supported");
  if (tr.kind == mir::Transform::Unsupported)
    throw Refused("oracle: unsupported transform " + tr.raw);
  ParamGeom g;
  for (const auto& d : type.dims) g.dims.push_back(in.as_int(d));
  const int rank = leaf_rank_of(type);
  if ((int)g.dims.size() < rank)
    throw Refused("oracle: parameter dims shorter than its leaf");
  g.outer = g.dims.size() - rank;
  g.leaf.assign(g.dims.begin() + g.outer, g.dims.end());
  for (size_t d = 0; d < g.outer; ++d) g.n_leaves *= g.dims[d];
  for (int64_t d : g.leaf) g.leaf_len *= d;
  g.free_leaf = mir::is_structured_check(tr.kind)
                    ? free_leaf_size(tr.kind, g.leaf)
                    : g.leaf_len;
  return g;
}

// Flat first-index-fast position of element `e` (leaf column-major) of leaf
// number `l` (outer array indices row-major, as the deserializer reads).
inline int64_t flat_pos(const ParamGeom& g, int64_t l, int64_t e) {
  std::vector<int64_t> idx(g.dims.size(), 0);
  for (size_t d = g.outer; d-- > 0;) {
    idx[d] = l % g.dims[d];
    l /= g.dims[d];
  }
  for (size_t d = g.outer; d < g.dims.size(); ++d) {
    idx[d] = e % g.dims[d];
    e /= g.dims[d];
  }
  int64_t pos = 0, stride = 1;
  for (size_t d = 0; d < g.dims.size(); ++d) {
    pos += idx[d] * stride;
    stride *= g.dims[d];
  }
  return pos;
}

// The C++ type stanc3 declares a parameter with: `Depth` std::vector levels
// around a double, vector, row vector or matrix.
template <typename Leaf, size_t Depth>
struct Nested {
  using type = std::vector<typename Nested<Leaf, Depth - 1>::type>;
};
template <typename Leaf>
struct Nested<Leaf, 0> {
  using type = Leaf;
};

template <typename Leaf>
constexpr size_t leaf_rank_v = std::is_same_v<Leaf, double>            ? 0
                               : std::is_same_v<Leaf, Eigen::MatrixXd> ? 2
                                                                       : 1;

// Conversion between a typed value and MirInterp's first-index-fast
// storage. `d` is the first of `dims` the value spans, `offset` its first
// element, `stride` the distance between its consecutive first indices.
template <typename X>
struct Layout;

template <>
struct Layout<double> {
  static double from(const std::vector<double>& r, const std::vector<int64_t>&,
                     size_t, int64_t offset, int64_t) {
    return r.at((size_t)offset);
  }
  static void into(double x, std::vector<double>& r,
                   const std::vector<int64_t>&, size_t, int64_t offset,
                   int64_t) {
    r.at((size_t)offset) = x;
  }
};

template <typename V>
struct VectorLayout {
  static V from(const std::vector<double>& r, const std::vector<int64_t>& dims,
                size_t d, int64_t offset, int64_t stride) {
    V v((Eigen::Index)dims[d]);
    for (int64_t i = 0; i < dims[d]; ++i)
      v((Eigen::Index)i) = r.at((size_t)(offset + i * stride));
    return v;
  }
  static void into(const V& v, std::vector<double>& r,
                   const std::vector<int64_t>&, size_t, int64_t offset,
                   int64_t stride) {
    for (Eigen::Index i = 0; i < v.size(); ++i)
      r.at((size_t)(offset + i * stride)) = v(i);
  }
};
template <>
struct Layout<Eigen::VectorXd> : VectorLayout<Eigen::VectorXd> {};
template <>
struct Layout<Eigen::RowVectorXd> : VectorLayout<Eigen::RowVectorXd> {};

template <>
struct Layout<Eigen::MatrixXd> {
  static Eigen::MatrixXd from(const std::vector<double>& r,
                              const std::vector<int64_t>& dims, size_t d,
                              int64_t offset, int64_t stride) {
    Eigen::MatrixXd m((Eigen::Index)dims[d], (Eigen::Index)dims[d + 1]);
    for (int64_t j = 0; j < dims[d + 1]; ++j)
      for (int64_t i = 0; i < dims[d]; ++i)
        m((Eigen::Index)i, (Eigen::Index)j) =
            r.at((size_t)(offset + (i + j * dims[d]) * stride));
    return m;
  }
  static void into(const Eigen::MatrixXd& m, std::vector<double>& r,
                   const std::vector<int64_t>&, size_t, int64_t offset,
                   int64_t stride) {
    for (Eigen::Index j = 0; j < m.cols(); ++j)
      for (Eigen::Index i = 0; i < m.rows(); ++i)
        r.at((size_t)(offset + (i + j * m.rows()) * stride)) = m(i, j);
  }
};

template <typename X>
struct Layout<std::vector<X>> {
  static std::vector<X> from(const std::vector<double>& r,
                             const std::vector<int64_t>& dims, size_t d,
                             int64_t offset, int64_t stride) {
    std::vector<X> out;
    out.reserve((size_t)dims[d]);
    for (int64_t i = 0; i < dims[d]; ++i)
      out.push_back(Layout<X>::from(r, dims, d + 1, offset + i * stride,
                                    stride * dims[d]));
    return out;
  }
  static void into(const std::vector<X>& xs, std::vector<double>& r,
                   const std::vector<int64_t>& dims, size_t d, int64_t offset,
                   int64_t stride) {
    for (size_t i = 0; i < xs.size(); ++i)
      Layout<X>::into(xs[i], r, dims, d + 1, offset + (int64_t)i * stride,
                      stride * dims[d]);
  }
};

// Call `f` with a bound as stanc3 types it: a scalar, one leaf (a bound on
// every element of an array), or the variable's own type.
template <typename Leaf, size_t Depth, typename F>
void with_bound(const Value& v, F&& f) {
  using Ret = typename Nested<Leaf, Depth>::type;
  constexpr size_t rank = leaf_rank_v<Leaf>;
  if (v.dims.empty()) {
    f(v.r.at(0));
    return;
  }
  int64_t n = 1;
  for (int64_t d : v.dims) n *= d;
  if (n != (int64_t)v.r.size())
    throw Refused("oracle: a bound's values do not fill its dims");
  if constexpr (Depth > 0 && rank > 0) {
    if (v.dims.size() == rank) {
      f(Layout<Leaf>::from(v.r, v.dims, 0, 0, 1));
      return;
    }
  }
  if (v.dims.size() == Depth + rank) {
    f(Layout<Ret>::from(v.r, v.dims, 0, 0, 1));
    return;
  }
  throw Refused(
      "oracle: a bound that is neither a scalar, one element nor the whole "
      "variable");
}

template <size_t... I, typename F>
auto with_sizes(const std::vector<int64_t>& dims, std::index_sequence<I...>,
                F&& f) {
  return f((Eigen::Index)dims.at(I)...);
}

// An elementwise transform (identity, bounds, offset and multiplier) reads
// the whole declaration at once, as stanc3's generated code does, so Stan
// Math checks the bounds and names an element as CmdStan does.
template <typename Leaf, size_t Depth>
void read_elementwise(Deserializer& d, const mir::Transform& tr,
                      const std::vector<Value>& args, const ParamGeom& g,
                      double& lp, Value& out) {
  using Ret = typename Nested<Leaf, Depth>::type;
  const auto sizes = std::make_index_sequence<Depth + leaf_rank_v<Leaf>>{};
  const auto arg = [&](size_t k) -> const Value& {
    if (k >= args.size())
      throw Refused("oracle: transform " + tr.raw + " is missing a bound");
    return args[k];
  };
  Ret x;
  switch (tr.kind) {
    case mir::Transform::Identity:
      x = with_sizes(g.dims, sizes, [&](auto... s) {
        return Ret(d.template read<Ret>(s...));
      });
      break;
    case mir::Transform::Lower:
      with_bound<Leaf, Depth>(arg(0), [&](const auto& lb) {
        x = with_sizes(g.dims, sizes, [&](auto... s) {
          return Ret(d.template read_constrain_lb<Ret, true>(lb, lp, s...));
        });
      });
      break;
    case mir::Transform::Upper:
      with_bound<Leaf, Depth>(arg(0), [&](const auto& ub) {
        x = with_sizes(g.dims, sizes, [&](auto... s) {
          return Ret(d.template read_constrain_ub<Ret, true>(ub, lp, s...));
        });
      });
      break;
    case mir::Transform::LowerUpper:
      with_bound<Leaf, Depth>(arg(0), [&](const auto& lb) {
        with_bound<Leaf, Depth>(arg(1), [&](const auto& ub) {
          x = with_sizes(g.dims, sizes, [&](auto... s) {
            return Ret(
                d.template read_constrain_lub<Ret, true>(lb, ub, lp, s...));
          });
        });
      });
      break;
    case mir::Transform::Offset:
      with_bound<Leaf, Depth>(arg(0), [&](const auto& offset) {
        x = with_sizes(g.dims, sizes, [&](auto... s) {
          return Ret(d.template read_constrain_offset_multiplier<Ret, true>(
              offset, 1.0, lp, s...));
        });
      });
      break;
    case mir::Transform::Multiplier:
      with_bound<Leaf, Depth>(arg(0), [&](const auto& multiplier) {
        x = with_sizes(g.dims, sizes, [&](auto... s) {
          return Ret(d.template read_constrain_offset_multiplier<Ret, true>(
              0.0, multiplier, lp, s...));
        });
      });
      break;
    case mir::Transform::OffsetMultiplier:
      with_bound<Leaf, Depth>(arg(0), [&](const auto& offset) {
        with_bound<Leaf, Depth>(arg(1), [&](const auto& multiplier) {
          x = with_sizes(g.dims, sizes, [&](auto... s) {
            return Ret(d.template read_constrain_offset_multiplier<Ret, true>(
                offset, multiplier, lp, s...));
          });
        });
      });
      break;
    default:
      throw Refused("oracle: unhandled transform " + tr.raw);
  }
  Layout<Ret>::into(x, out.r, g.dims, 0, 0, 1);
}

template <typename Leaf>
void read_elementwise_at_depth(Deserializer& d, const mir::Transform& tr,
                               const std::vector<Value>& args,
                               const ParamGeom& g, double& lp, Value& out) {
  switch (g.outer) {
    case 0:
      return read_elementwise<Leaf, 0>(d, tr, args, g, lp, out);
    case 1:
      return read_elementwise<Leaf, 1>(d, tr, args, g, lp, out);
    case 2:
      return read_elementwise<Leaf, 2>(d, tr, args, g, lp, out);
    case 3:
      return read_elementwise<Leaf, 3>(d, tr, args, g, lp, out);
    default:
      throw Refused("oracle: parameter arrays deeper than 3 are not supported");
  }
}

// A structured transform reads one leaf at a time, which is what Stan's
// std::vector overloads do too.
inline void read_structured(Deserializer& d, const mir::Transform& tr,
                            const ParamGeom& g, double& lp, Value& out) {
  using VecV = Eigen::VectorXd;
  using MatV = Eigen::MatrixXd;
  const auto place = [&](int64_t l, const double* leafval) {
    for (int64_t e = 0; e < g.leaf_len; ++e)
      out.r[(size_t)flat_pos(g, l, e)] = leafval[e];
  };
  const int64_t K = g.leaf.empty() ? 1 : g.leaf[0];
  const int64_t C = g.leaf.size() > 1 ? g.leaf[1] : 1;
  for (int64_t l = 0; l < g.n_leaves; ++l) {
    switch (tr.kind) {
      case mir::Transform::Simplex: {
        VecV v = d.template read_constrain_simplex<VecV, true>(lp, K);
        place(l, v.data());
        break;
      }
      case mir::Transform::Ordered: {
        VecV v = d.template read_constrain_ordered<VecV, true>(lp, K);
        place(l, v.data());
        break;
      }
      case mir::Transform::PositiveOrdered: {
        VecV v = d.template read_constrain_positive_ordered<VecV, true>(lp, K);
        place(l, v.data());
        break;
      }
      case mir::Transform::UnitVector: {
        VecV v = d.template read_constrain_unit_vector<VecV, true>(lp, K);
        place(l, v.data());
        break;
      }
      case mir::Transform::SumToZero: {
        if (g.leaf.size() == 2) {
          MatV v = d.template read_constrain_sum_to_zero<MatV, true>(lp, K, C);
          place(l, v.data());
        } else {
          VecV v = d.template read_constrain_sum_to_zero<VecV, true>(lp, K);
          place(l, v.data());
        }
        break;
      }
      case mir::Transform::CholeskyCorr: {
        MatV v =
            d.template read_constrain_cholesky_factor_corr<MatV, true>(lp, K);
        place(l, v.data());
        break;
      }
      case mir::Transform::CholeskyCov: {
        MatV v =
            d.template read_constrain_cholesky_factor_cov<MatV, true>(lp, K, C);
        place(l, v.data());
        break;
      }
      case mir::Transform::Correlation: {
        MatV v = d.template read_constrain_corr_matrix<MatV, true>(lp, K);
        place(l, v.data());
        break;
      }
      case mir::Transform::Covariance: {
        MatV v = d.template read_constrain_cov_matrix<MatV, true>(lp, K);
        place(l, v.data());
        break;
      }
      case mir::Transform::StochasticColumn: {
        MatV v =
            d.template read_constrain_stochastic_column<MatV, true>(lp, K, C);
        place(l, v.data());
        break;
      }
      case mir::Transform::StochasticRow: {
        MatV v = d.template read_constrain_stochastic_row<MatV, true>(lp, K, C);
        place(l, v.data());
        break;
      }
      default:
        throw Refused("oracle: unhandled transform " + tr.raw);
    }
  }
}

// Read one parameter declaration: constrain it with the Jacobian into `lp`,
// bind it in `in`, and record its value for write_array.
inline void read_param(MirInterp<double>& in, const mir::Stmt& st,
                       Deserializer& d, double& lp,
                       std::map<std::string, Entry>* record) {
  const mir::Transform& tr = *st.read_transform;
  const ParamGeom g = geometry(in, st.decl_type, tr);
  Value out;
  out.dims = g.dims;
  out.r.assign((size_t)(g.n_leaves * g.leaf_len), 0.0);
  if (mir::is_structured_check(tr.kind)) {
    read_structured(d, tr, g, lp, out);
  } else {
    std::vector<Value> args;
    for (const auto& a : tr.args) args.push_back(in.eval(a));
    const std::string& b = st.decl_type.base == "SArray"
                               ? st.decl_type.elem_base
                               : st.decl_type.base;
    if (b == "SVector")
      read_elementwise_at_depth<Eigen::VectorXd>(d, tr, args, g, lp, out);
    else if (b == "SRowVector")
      read_elementwise_at_depth<Eigen::RowVectorXd>(d, tr, args, g, lp, out);
    else if (b == "SMatrix")
      read_elementwise_at_depth<Eigen::MatrixXd>(d, tr, args, g, lp, out);
    else
      read_elementwise_at_depth<double>(d, tr, args, g, lp, out);
  }
  if (record) (*record)[st.decl_id] = out;
  in.set_declared_dims(st.decl_id, g.dims);
  in.env()[st.decl_id] = std::move(out);
}

struct Model {
  std::shared_ptr<const mir::Program> prog;
  std::map<std::string, const mir::FunDef*> funs;
  std::shared_ptr<const std::map<std::string, Entry>> shared_env;
  std::unique_ptr<WaInterp> wa;
  int64_t n_free = 0;
  // The unconstrained value the deserializer reads k-th is free_order[k] of
  // CmdStan's unconstrained vector. They differ for an array of tuples,
  // which CmdStan holds element by element and the leaves read one by one.
  std::vector<int64_t> free_order;
};

// CmdStan reads the data block in declaration order: each variable's sizes,
// whether it is there, and its dims, before the next variable. This applies
// the lowering's rules and messages (Lowering::bind_data).
class DataBinder {
 public:
  DataBinder(const mir::Program& p, const DataMap& data) : p_(p), data_(data) {
    for (const auto& [name, type] : p.input_vars) types_[name] = &type;
  }

  const Entry* entry(const std::string& key) const {
    if (bound_.has(key)) return &bound_.at(key);
    if (data_.has(key)) return &data_.at(key);
    return nullptr;
  }

  // Bind each input with its declared dims where those can be computed
  // from the inputs before it.
  void bind(const std::map<std::string, const mir::FunDef*>& funs) {
    MirInterp<double> shapes(funs, "data shapes");
    for (const auto& [name, type] : p_.input_vars) {
      const std::string& key = mir::detail::data_key(p_, name);
      const mir::detail::TupleLeaf* leaf = mir::detail::tuple_leaf(p_, name);
      std::optional<std::vector<int64_t>> dims;
      try {
        std::vector<int64_t> found;
        for (const auto& d : type.dims) found.push_back(shapes.as_int(d));
        dims = std::move(found);
      } catch (const CompileError&) {
      }
      if (dims) {
        std::optional<Entry> value = declared_shape_value(
            *dims, int_declared(type),
            data_.has(key) ? &data_.at(key) : nullptr, [&] {
              return leaf ? mir::detail::reader_order(*leaf, *dims)
                          : std::vector<size_t>{};
            });
        if (value) bound_.set(key, std::move(*value));
      }
      if (const Entry* e = entry(key)) shapes.env()[name] = *e;
    }
  }

  bool is_input(const std::string& name) const { return types_.count(name); }

  void check(const std::string& name, MirInterp<double>& in) {
    if (!checked_.insert(name).second) return;
    const std::string& key = mir::detail::data_key(p_, name);
    if (!entry(key)) report_missing();
    if (!data_.has(key)) return;
    const mir::SizedType& t = *types_.at(name);
    std::vector<int64_t> declared;
    for (const auto& d : t.dims) declared.push_back(in.as_int(d));
    for (int64_t d : declared)
      if (d < 0)
        throw CompileError("negative extent for data " + key +
                           (t.raw.empty() ? "" : " | in: " + t.raw));
    check_declared_dims(VarStage::Data, key, declared, data_.at(key),
                        int_declared(t));
  }

  void check_all(MirInterp<double>& in) {
    for (const auto& input : p_.input_vars) check(input.first, in);
  }

 private:
  [[noreturn]] void report_missing() const {
    std::string names;
    for (const auto& input : p_.input_vars) {
      const std::string& key = mir::detail::data_key(p_, input.first);
      if (!entry(key)) names += (names.empty() ? "" : ", ") + key;
    }
    throw CompileError("missing data for " + names +
                       "; the data must give every variable in the data block");
  }

  const mir::Program& p_;
  const DataMap& data_;
  DataMap bound_;
  std::map<std::string, const mir::SizedType*> types_;
  std::set<std::string> checked_;
};

// Where each parameter's free values sit in CmdStan's unconstrained vector.
// An array of tuples holds each element's leaves together; its leaf
// variables read their own values one after another (Lowering's
// tuple_leaf_free).
inline void lay_out_parameters(Model& m) {
  const mir::Program& p = *m.prog;
  MirInterp<double> sizes(m.funs, "parameter sizes");
  sizes.set_parent_env(m.shared_env.get());
  struct Block {
    std::map<std::string, std::vector<int64_t>> positions;
    size_t next_leaf = 0;
  };
  std::map<std::string, Block> blocks;
  std::function<void(const std::vector<mir::Stmt>&)> walk =
      [&](const std::vector<mir::Stmt>& body) {
        for (const auto& st : body) {
          if (st.kind == mir::Stmt::Block || st.kind == mir::Stmt::SList) {
            walk(st.body);
            continue;
          }
          if (!st.read_transform) continue;
          if (st.kind != mir::Stmt::Decl)
            throw Refused("oracle: FnReadParam outside a declaration");
          const ParamGeom g = geometry(sizes, st.decl_type, *st.read_transform);
          const mir::detail::TupleLeaf* leaf =
              mir::detail::tuple_leaf(p, st.decl_id);
          if (leaf == nullptr) {
            for (int64_t j = 0; j < g.free_len(); ++j)
              m.free_order.push_back(m.n_free + j);
            m.n_free += g.free_len();
            continue;
          }
          const std::vector<std::string>& names =
              p.tuples->leaves_of.at(leaf->base);
          auto [found, first] = blocks.try_emplace(leaf->base);
          Block& block = found->second;
          if (first) {
            CompiledModel::UncParam u;
            for (const std::string& name : names) {
              const mir::detail::TupleLeaf& other = p.tuples->leaves.at(name);
              if (!other.read_transform)
                throw Refused("oracle: tuple parameter " + leaf->base +
                              " has a leaf without a read");
              const ParamGeom og =
                  geometry(sizes, other.type, *other.read_transform);
              CompiledModel::UncParam::Leaf free;
              for (const mir::detail::TupleSlot& slot : other.path)
                free.path.emplace_back(slot.before, slot.component);
              free.dims = og.dims;
              free.len = og.free_len();
              u.leaves.push_back(std::move(free));
            }
            std::vector<std::vector<int64_t>> gathers(names.size());
            for (size_t k = 0; k < names.size(); ++k)
              gathers[k].resize((size_t)u.leaves[k].len);
            int64_t next = 0;
            u.for_each_element(
                [&](size_t k, const std::vector<int64_t>& index) {
                  const CompiledModel::UncParam::Leaf& free = u.leaves[k];
                  const int64_t per = free.len / free.elements();
                  int64_t element = 0;
                  for (size_t dd = 0; dd < index.size(); ++dd)
                    element = element * free.dims[dd] + index[dd];
                  for (int64_t j = 0; j < per; ++j)
                    gathers[k][(size_t)(element * per + j)] = m.n_free + next++;
                });
            for (size_t k = 0; k < names.size(); ++k)
              block.positions[names[k]] = std::move(gathers[k]);
            m.n_free += next;
          }
          if (block.next_leaf >= names.size() ||
              names[block.next_leaf] != st.decl_id)
            throw Refused("oracle: the leaves of tuple parameter " +
                          leaf->base + " are read out of order");
          ++block.next_leaf;
          const std::vector<int64_t>& at = block.positions.at(st.decl_id);
          if ((int64_t)at.size() != g.free_len())
            throw Refused("oracle: tuple parameter leaf " + st.decl_id +
                          " reads a different number of free values than its "
                          "block holds");
          m.free_order.insert(m.free_order.end(), at.begin(), at.end());
        }
      };
  walk(p.log_prob);
}

inline std::unique_ptr<Model> prepare(const std::string& mir_text,
                                      const DataMap& data, unsigned seed) {
  auto m = std::make_unique<Model>();
  auto prog = std::make_shared<mir::Program>(decode_program(mir_text));
  m->prog = prog;
  for (const auto& f : prog->fun_defs) m->funs[mir::function_key(f)] = &f;
  DataBinder binder(*prog, data);
  binder.bind(m->funs);
  WaRng td_rng(seed);
  MirHooks h;
  h.data = [&](const std::string& n) { return binder.entry(n); };
  h.fun = [&](MirInterp<double>& in, const mir::Expr& e, Entry* out) {
    if (interpreted_rng_call(in, e, out, td_rng)) return true;
    return evaluate_retained_higher_order(
        m->funs, e, [&in](const mir::Expr& a) { return in.eval(a); }, out);
  };
  MirInterp<double> td(m->funs, "prepare_data", h);
  for (const auto& st : prog->prepare_data) {
    if (st.kind == mir::Stmt::Decl && binder.is_input(st.decl_id))
      binder.check(st.decl_id, td);
    if (td.exec(st) != MirInterp<double>::Flow::Next) break;
  }
  binder.check_all(td);
  // The interpreter keeps a block's locals after the block ends; only the
  // inputs and the top-level declarations are transformed data.
  std::set<std::string> prepared;
  for (const auto& input : prog->input_vars) prepared.insert(input.first);
  std::function<void(const mir::Stmt&)> top_level = [&](const mir::Stmt& st) {
    if (st.kind == mir::Stmt::Decl) prepared.insert(st.decl_id);
    if (st.kind == mir::Stmt::SList)
      for (const auto& k : st.body) top_level(k);
  };
  for (const auto& st : prog->prepare_data) top_level(st);
  std::map<std::string, Entry> env = std::move(td.env());
  for (auto it = env.begin(); it != env.end();)
    it = prepared.count(it->first) ? std::next(it) : env.erase(it);
  m->shared_env =
      std::make_shared<const std::map<std::string, Entry>>(std::move(env));
  lay_out_parameters(*m);
  if (!prog->generate_quantities.empty()) {
    std::map<std::string, Entry> flags;
    for (const char* flag :
         {"emit_transformed_parameters__", "emit_generated_quantities__"}) {
      Entry one;
      one.is_int = true;
      one.i = {1};
      one.r = {1.0};
      flags[flag] = one;
    }
    m->wa = std::make_unique<WaInterp>(prog, m->shared_env, std::move(flags));
  }
  return m;
}

// The log density with the Jacobian, as CmdStan's
// log_prob_propto_jacobian, on doubles. propto drops the terms CmdStan's
// var instantiation drops (set_model_types). `record` receives the
// constrained parameters.
inline double log_density(const Model& m, const std::vector<double>& theta,
                          std::map<std::string, Entry>* record) {
  std::vector<double> th(theta.size());
  for (size_t k = 0; k < th.size(); ++k)
    th[k] = theta.at((size_t)m.free_order.at(k));
  std::vector<int> ints;
  Deserializer d(th, ints);
  double jacobian = 0.0, target = 0.0;
  MirInterp<double>* current = nullptr;
  MirHooks h;
  h.stmt = [&](const mir::Stmt& st) {
    if (st.kind != mir::Stmt::Decl || !st.read_transform) return false;
    read_param(*current, st, d, jacobian, record);
    return true;
  };
  h.fun = [&](MirInterp<double>& in, const mir::Expr& e, Entry* out) {
    if (mir::stateful_intrinsic_kind(e)) {
      out->r = {jacobian + target};
      return true;
    }
    return evaluate_retained_higher_order(
        m.funs, e, [&in](const mir::Expr& a) { return in.eval(a); }, out);
  };
  MirInterp<double> in(m.funs, "log_prob", std::move(h));
  current = &in;
  in.set_native_extrema(true);
  in.set_model_types(true);
  in.set_parent_env(m.shared_env.get());
  in.set_target_sink(&target);
  in.set_jacobian_sink(&jacobian);
  in.run(m.prog->log_prob);
  return jacobian + target;
}

struct PointResult {
  double lp = 0;
  std::vector<double> grad;
  // The estimated absolute error of each gradient component; infinite
  // where an evaluation near the point failed.
  std::vector<double> grad_err;
  std::map<std::string, Entry> constrained;
};

// The log density at `theta`, and its gradient by central differences with
// steps h = cbrt(eps) max(1, |x|) and h/2, combined by Richardson
// extrapolation. The two estimates' difference bounds the truncation
// error; the rounding error of the differences is added to it.
inline PointResult evaluate(
    const Model& m, const std::vector<double>& theta, bool gradient = true,
    std::chrono::steady_clock::time_point* lp_done = nullptr) {
  PointResult res;
  res.lp = log_density(m, theta, &res.constrained);
  if (lp_done) *lp_done = std::chrono::steady_clock::now();
  const size_t n = theta.size();
  res.grad.assign(n, std::numeric_limits<double>::quiet_NaN());
  res.grad_err.assign(n, std::numeric_limits<double>::infinity());
  if (!gradient || !std::isfinite(res.lp)) return res;
  set_message_sink([](const char*, std::size_t) {});
  struct RestoreSink {
    ~RestoreSink() { set_message_sink(nullptr); }
  } restore;
  const double eps = std::numeric_limits<double>::epsilon();
  std::vector<double> x(theta);
  const auto at = [&](size_t i, double xi) -> std::optional<double> {
    x[i] = xi;
    std::optional<double> v;
    try {
      const double f = log_density(m, x, nullptr);
      if (std::isfinite(f)) v = f;
    } catch (const std::exception&) {
    }
    x[i] = theta[i];
    return v;
  };
  for (size_t i = 0; i < n; ++i) {
    const double h = std::cbrt(eps) * std::max(1.0, std::abs(theta[i]));
    const auto central = [&](double step) -> std::optional<double> {
      const double hi = theta[i] + step, lo = theta[i] - step;
      const std::optional<double> a = at(i, hi);
      if (!a) return std::nullopt;
      const std::optional<double> b = at(i, lo);
      if (!b) return std::nullopt;
      return (*a - *b) / (hi - lo);
    };
    const std::optional<double> coarse = central(h);
    if (!coarse) continue;
    const std::optional<double> fine = central(h / 2);
    if (!fine) continue;
    res.grad[i] = *fine + (*fine - *coarse) / 3;
    res.grad_err[i] = std::abs(*fine - *coarse) +
                      4 * eps * std::max(1.0, std::abs(res.lp)) / h;
  }
  return res;
}

}  // namespace stanli::oracle

#endif

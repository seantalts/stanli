// stanli_check --interp: the whole model through the MIR interpreter alone,
// for measuring how far MirInterp is from serving as the reference oracle.
//
// Transformed data runs on MirInterp<double> with the hooks the lowering
// installs. log_prob runs on MirInterp<var>; this driver walks its top-level
// statements so that it can read each parameter itself, through Stan's own
// deserializer with the Jacobian, in CmdStan's unconstrained order. The
// constrained values then feed WaInterp for the write_array row. Nothing here
// reads the compiled graph.
#ifndef STANLI_TOOLS_INTERP_ORACLE_HPP
#define STANLI_TOOLS_INTERP_ORACLE_HPP

#include <stanli/compile.hpp>
#include <stanli/data.hpp>
#include <stanli/higher_order_eval.hpp>
#include <stanli/large_stack.hpp>
#include <stanli/mir.hpp>
#include <stanli/mir_decode.hpp>
#include <stanli/mir_interp.hpp>
#include <stanli/unconstrain.hpp>
#include <stanli/wa_interp.hpp>

#include <stan/io/deserializer.hpp>
#include <stan/math.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace stanli::oracle {

using stan::math::var;
using Entry = DataMap::Entry;
using VarVal = MirVal<var>;

struct Refused : CompileError {
  using CompileError::CompileError;
};

inline int leaf_rank_of(const mir::SizedType& t) {
  const std::string& b = t.base == "SArray" ? t.elem_base : t.base;
  if (b == "SVector" || b == "SRowVector") return 1;
  if (b == "SMatrix") return 2;
  if (b == "SReal" || b == "SInt") return 0;
  throw Refused("oracle: parameter of type " + b + " is not supported");
}

inline bool reads_param(const mir::Stmt& st) {
  if (st.read_transform) return true;
  for (const auto& k : st.body)
    if (reads_param(k)) return true;
  return false;
}

struct Model {
  std::shared_ptr<const mir::Program> prog;
  std::map<std::string, const mir::FunDef*> funs;
  std::map<std::string, Entry> td_env;
  std::shared_ptr<const std::map<std::string, Entry>> shared_env;
  std::unique_ptr<WaInterp> wa;
  int64_t n_free = 0;
};

// One parameter read: geometry evaluated once per model.
struct ParamGeom {
  std::vector<int64_t> dims;
  size_t outer = 0;
  std::vector<int64_t> leaf;
  int64_t leaf_len = 1, n_leaves = 1, free_leaf = 1;
};

inline ParamGeom geometry(MirInterp<double>& in, const mir::Stmt& st) {
  if (st.kind != mir::Stmt::Decl)
    throw Refused("oracle: FnReadParam outside a declaration");
  if (mir::is_complex(st.decl_type))
    throw Refused("oracle: complex parameters are not supported");
  ParamGeom g;
  for (const auto& d : st.decl_type.dims) g.dims.push_back(in.as_int(d));
  const int rank = leaf_rank_of(st.decl_type);
  if ((int)g.dims.size() < rank)
    throw Refused("oracle: parameter dims shorter than its leaf");
  g.outer = g.dims.size() - rank;
  g.leaf.assign(g.dims.begin() + g.outer, g.dims.end());
  for (size_t d = 0; d < g.outer; ++d) g.n_leaves *= g.dims[d];
  for (int64_t d : g.leaf) g.leaf_len *= d;
  const auto kind = st.read_transform->kind;
  if (kind == mir::Transform::Unsupported)
    throw Refused("oracle: unsupported transform " + st.read_transform->raw);
  g.free_leaf = mir::is_structured_check(kind)
                    ? free_leaf_size(kind, g.leaf)
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

template <typename T>
const T& arg_at(const typename MirInterp<T>::Value& v, int64_t pos) {
  return v.r.size() == 1 ? v.r[0] : v.r.at((size_t)pos);
}

// Read one parameter declaration: constrain with the Jacobian into `lp`,
// bind it in `in`, and record its value for write_array.
// T = var for log_prob, T = double for write_array, which CmdStan also
// constrains on doubles (the double overloads check more, unit_vector's norm
// for one).
template <typename T>
void read_param(MirInterp<T>& in, const mir::Stmt& st, const ParamGeom& g,
                stan::io::deserializer<T>& d, T& lp,
                std::map<std::string, Entry>* record) {
  using VecV = Eigen::Matrix<T, -1, 1>;
  using MatV = Eigen::Matrix<T, -1, -1>;
  using Val = typename MirInterp<T>::Value;
  const mir::Transform& tr = *st.read_transform;
  std::vector<Val> args;
  for (const auto& a : tr.args) args.push_back(in.eval(a));
  Val out;
  out.dims = g.dims;
  int64_t total = g.n_leaves * g.leaf_len;
  out.r.assign((size_t)total, T(0.0));
  const auto place = [&](int64_t l, const auto& leafval) {
    for (int64_t e = 0; e < g.leaf_len; ++e)
      out.r[(size_t)flat_pos(g, l, e)] = leafval(e);
  };
  for (int64_t l = 0; l < g.n_leaves; ++l) {
    const int64_t K = g.leaf.empty() ? 1 : g.leaf[0];
    const int64_t C = g.leaf.size() > 1 ? g.leaf[1] : 1;
    switch (tr.kind) {
      case mir::Transform::Simplex: {
        VecV v = d.template read_constrain_simplex<VecV, true>(lp, K);
        place(l, [&](int64_t e) { return v(e); });
        break;
      }
      case mir::Transform::Ordered: {
        VecV v = d.template read_constrain_ordered<VecV, true>(lp, K);
        place(l, [&](int64_t e) { return v(e); });
        break;
      }
      case mir::Transform::PositiveOrdered: {
        VecV v = d.template read_constrain_positive_ordered<VecV, true>(lp, K);
        place(l, [&](int64_t e) { return v(e); });
        break;
      }
      case mir::Transform::UnitVector: {
        VecV v = d.template read_constrain_unit_vector<VecV, true>(lp, K);
        place(l, [&](int64_t e) { return v(e); });
        break;
      }
      case mir::Transform::SumToZero: {
        if (g.leaf.size() == 2) {
          MatV v = d.template read_constrain_sum_to_zero<MatV, true>(lp, K, C);
          place(l, [&](int64_t e) { return v.data()[e]; });
        } else {
          VecV v = d.template read_constrain_sum_to_zero<VecV, true>(lp, K);
          place(l, [&](int64_t e) { return v(e); });
        }
        break;
      }
      case mir::Transform::CholeskyCorr: {
        MatV v = d.template read_constrain_cholesky_factor_corr<MatV, true>(lp, K);
        place(l, [&](int64_t e) { return v.data()[e]; });
        break;
      }
      case mir::Transform::CholeskyCov: {
        MatV v = d.template read_constrain_cholesky_factor_cov<MatV, true>(lp, K, C);
        place(l, [&](int64_t e) { return v.data()[e]; });
        break;
      }
      case mir::Transform::Correlation: {
        MatV v = d.template read_constrain_corr_matrix<MatV, true>(lp, K);
        place(l, [&](int64_t e) { return v.data()[e]; });
        break;
      }
      case mir::Transform::Covariance: {
        MatV v = d.template read_constrain_cov_matrix<MatV, true>(lp, K);
        place(l, [&](int64_t e) { return v.data()[e]; });
        break;
      }
      case mir::Transform::StochasticColumn: {
        MatV v = d.template read_constrain_stochastic_column<MatV, true>(lp, K, C);
        place(l, [&](int64_t e) { return v.data()[e]; });
        break;
      }
      case mir::Transform::StochasticRow: {
        MatV v = d.template read_constrain_stochastic_row<MatV, true>(lp, K, C);
        place(l, [&](int64_t e) { return v.data()[e]; });
        break;
      }
      default: {
        // Elementwise: one scalar at a time, in the deserializer's order.
        for (int64_t e = 0; e < g.leaf_len; ++e) {
          const int64_t pos = flat_pos(g, l, e);
          T x;
          switch (tr.kind) {
            case mir::Transform::Identity:
              x = d.template read<T>();
              break;
            case mir::Transform::Lower:
              x = d.template read_constrain_lb<T, true>(arg_at<T>(args.at(0), pos), lp);
              break;
            case mir::Transform::Upper:
              x = d.template read_constrain_ub<T, true>(arg_at<T>(args.at(0), pos), lp);
              break;
            case mir::Transform::LowerUpper:
              x = d.template read_constrain_lub<T, true>(
                  arg_at<T>(args.at(0), pos), arg_at<T>(args.at(1), pos), lp);
              break;
            case mir::Transform::Offset:
              x = d.template read_constrain_offset_multiplier<T, true>(
                  arg_at<T>(args.at(0), pos), 1.0, lp);
              break;
            case mir::Transform::Multiplier:
              x = d.template read_constrain_offset_multiplier<T, true>(
                  0.0, arg_at<T>(args.at(0), pos), lp);
              break;
            case mir::Transform::OffsetMultiplier:
              x = d.template read_constrain_offset_multiplier<T, true>(
                  arg_at<T>(args.at(0), pos), arg_at<T>(args.at(1), pos), lp);
              break;
            default:
              throw Refused("oracle: unhandled transform " + tr.raw);
          }
          out.r[(size_t)pos] = x;
        }
      }
    }
  }
  if (record) {
    Entry rec;
    rec.dims = g.dims;
    for (const T& x : out.r) rec.r.push_back(stan::math::value_of(x));
    (*record)[st.decl_id] = std::move(rec);
  }
  in.set_declared_dims(st.decl_id, g.dims);
  in.env()[st.decl_id] = std::move(out);
}

inline std::unique_ptr<Model> prepare(const std::string& mir_text,
                                      const DataMap& data, unsigned seed) {
  auto m = std::make_unique<Model>();
  auto prog = std::make_shared<mir::Program>(decode_program(mir_text));

  m->prog = prog;
  for (const auto& f : prog->fun_defs) m->funs[mir::function_key(f)] = &f;
  WaRng td_rng(seed);
  // An input with no elements may be left out of the data, as CmdStan
  // allows; it is synthesized at its declaration.
  std::map<std::string, Entry> empty_inputs;
  MirHooks h;
  h.data = [&](const std::string& n) -> const Entry* {
    if (data.has(n)) return &data.at(n);
    auto it = empty_inputs.find(n);
    return it == empty_inputs.end() ? nullptr : &it->second;
  };
  h.fun = [&](MirInterp<double>& in, const mir::Expr& e, Entry* out) {
    if (interpreted_rng_call(in, e, out, td_rng)) return true;
    return evaluate_retained_higher_order(
        m->funs, e, [&in](const mir::Expr& a) { return in.eval(a); }, out);
  };
  MirInterp<double> td(m->funs, "prepare_data", h);
  std::map<std::string, const mir::SizedType*> inputs;
  for (const auto& [name, type] : prog->input_vars) inputs[name] = &type;
  for (const auto& st : prog->prepare_data) {
    if (st.kind == mir::Stmt::Decl && inputs.count(st.decl_id) &&
        !data.has(st.decl_id)) {
      Entry e;
      int64_t n = 1;
      for (const auto& d : st.decl_type.dims) {
        e.dims.push_back(td.as_int(d));
        n *= e.dims.back();
      }
      e.is_int = st.decl_type.base == "SInt" ||
                 (st.decl_type.base == "SArray" &&
                  st.decl_type.elem_base == "SInt");
      if (n == 0 && !e.dims.empty()) empty_inputs[st.decl_id] = e;
    }
    if (td.exec(st) != MirInterp<double>::Flow::Next) break;
  }
  m->td_env = td.env();
  m->shared_env =
      std::make_shared<const std::map<std::string, Entry>>(m->td_env);
  // The parameter count, from the declarations' data-only sizes.
  MirInterp<double> sizes(m->funs, "parameter sizes");
  sizes.set_parent_env(m->shared_env.get());
  std::function<void(const std::vector<mir::Stmt>&)> count =
      [&](const std::vector<mir::Stmt>& body) {
        for (const auto& st : body) {
          if (st.kind == mir::Stmt::Block || st.kind == mir::Stmt::SList) {
            count(st.body);
          } else if (st.read_transform) {
            const ParamGeom g = geometry(sizes, st);
            m->n_free += g.n_leaves * g.free_leaf;
          }
        }
      };
  count(prog->log_prob);
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

struct PointResult {
  double lp = 0;
  std::vector<double> grad;
  std::map<std::string, Entry> constrained;
};

inline PointResult log_density(Model& m, const std::vector<double>& theta) {
  PointResult res;
  std::vector<var> th(theta.begin(), theta.end());
  std::vector<int> ints;
  stan::io::deserializer<var> d(th, ints);
  var jac = 0.0, target = 0.0;
  // Data and transformed data as constants on this point's tape.
  std::map<std::string, VarVal> parent;
  for (const auto& [name, e] : m.td_env) {
    VarVal v;
    v.is_int = e.is_int;
    v.i = e.i;
    v.dims = e.dims;
    v.r.reserve(e.r.size());
    for (double x : e.r) v.r.emplace_back(x);
    parent.emplace(name, std::move(v));
  }
  MirInterp<var> in(m.funs, "log_prob");
  in.set_native_extrema(true);
  in.set_parent_env(&parent);
  in.set_target_sink(&target);
  MirInterp<double> sizes(m.funs, "parameter sizes");
  sizes.set_parent_env(m.shared_env.get());
  std::function<void(const std::vector<mir::Stmt>&)> walk =
      [&](const std::vector<mir::Stmt>& body) {
        for (const auto& st : body) {
          if (st.kind == mir::Stmt::Block || st.kind == mir::Stmt::SList) {
            walk(st.body);
          } else if (st.read_transform) {
            read_param<var>(in, st, geometry(sizes, st), d, jac, nullptr);
          } else if (st.kind == mir::Stmt::TargetPE && st.jacobian) {
            // MirInterp drops `jacobian +=` (write_array semantics); the
            // driver adds the top-level ones itself.
            for (const var& t : in.eval(st.target).r) jac += t;
          } else {
            if (reads_param(st))
              throw Refused("oracle: parameter read inside control flow");
            if (in.exec(st) != MirInterp<var>::Flow::Next) return;
          }
        }
      };
  walk(m.prog->log_prob);
  var lp = jac + target;
  res.lp = lp.val();
  stan::math::grad(lp.vi_);
  res.grad.resize(th.size());
  for (size_t i = 0; i < th.size(); ++i) res.grad[i] = th[i].adj();
  return res;
}

// The constrained parameters on doubles, as CmdStan's write_array reads
// them. Bounds see data, transformed data and earlier parameters.
inline std::map<std::string, Entry> constrained(
    Model& m, const std::vector<double>& theta) {
  std::map<std::string, Entry> out;
  std::vector<double> th(theta);
  std::vector<int> ints;
  stan::io::deserializer<double> d(th, ints);
  double lp = 0.0;
  MirInterp<double> in(m.funs, "write_array parameters");
  in.set_parent_env(m.shared_env.get());
  std::function<void(const std::vector<mir::Stmt>&)> walk =
      [&](const std::vector<mir::Stmt>& body) {
        for (const auto& st : body) {
          if (st.kind == mir::Stmt::Block || st.kind == mir::Stmt::SList)
            walk(st.body);
          else if (st.read_transform)
            read_param<double>(in, st, geometry(in, st), d, lp, &out);
        }
      };
  walk(m.prog->log_prob);
  return out;
}

}  // namespace stanli::oracle

#endif

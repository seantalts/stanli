// The MIR interpreter: one evaluator for every place stanli executes MIR
// directly instead of lowering it to ops.
//
// Two call sites share it. The lowering runs it on `double` over the
// prepare_data statements (transformed data is DataOnly, so it reduces to
// concrete values before any op is emitted) and on data-only conditions and
// size expressions inside log_prob. The ODE kernels run it on `double` and
// on `var` to evaluate a right-hand side at whatever times the integrator
// picks, when ode_prog could not compile the body (see ode_prog.cpp for the
// fast path). One interpreter means one definition of indexing,
// broadcasting, statement semantics, and the function vocabulary; a
// function that works in transformed data works in an ODE body and vice
// versa.
//
// Values carry a real buffer, an int mirror where the value is integer, and
// dims (empty = scalar; matrices are column-major). On `double` the value
// type is DataMap::Entry itself, so the lowering reads interpreter results
// with no conversion. Anything the interpreter does not support raises a
// CompileError naming the construct, never a silent wrong answer.
#ifndef STANLI_MIR_INTERP_HPP
#define STANLI_MIR_INTERP_HPP

#include <stanli/compile.hpp>
#include <stanli/data.hpp>
#include <stanli/message_sink.hpp>
#include <stanli/mir.hpp>
#include <stanli/optable.hpp>  // STANLI_SCALAR_UNARY_LIST
#include <stanli/program.hpp>
#include <stanli/structured_check.hpp>

#include <stan/math.hpp>

#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace stanli {

template <typename T>
struct MirVal {
  bool is_int = false;
  std::vector<T> r;
  std::vector<int> i;
  std::vector<int64_t> dims;  // empty = scalar
};

// On double the value IS the data container's entry: interpreter results
// flow into the lowering (fills, shapes, int environments) without copies
// or conversions.
template <typename T>
struct MirValFor {
  using type = MirVal<T>;
};
template <>
struct MirValFor<double> {
  using type = DataMap::Entry;
};

// Host hooks, all optional. The lowering installs the first two; the
// interpreted write_array installs the last two (they only fire on the
// double instantiation); the ODE kernels install none.
struct MirHooks {
  // Fetch a data variable by name (FnReadData and bare data-block decls).
  std::function<const DataMap::Entry*(const std::string&)> data;
  // Variable fallback: the lowering's unrolled-loop indices, which live
  // outside the interpreter's environment.
  std::function<bool(const std::string&, long*)> int_var;
  // First shot at any StanLib call the host owns (RNG draws, which carry
  // state the pure interpreter must not). Return true when handled.
  std::function<bool(const mir::Expr&, DataMap::Entry*)> fun;
  // First shot at any statement (FnReadParam and FnWriteParam in the
  // interpreted write_array). Return true when handled.
  std::function<bool(const mir::Stmt&)> stmt;
};

template <typename T>
class MirInterp {
 public:
  using Value = typename MirValFor<T>::type;

  MirInterp(const std::map<std::string, const mir::FunDef*>& funs,
            std::string where, MirHooks hooks = {})
      : funs_(funs), where_(std::move(where)), hooks_(std::move(hooks)) {}

  std::map<std::string, Value>& env() { return env_; }

  Value* find(const std::string& n) {
    auto it = env_.find(n);
    return it == env_.end() ? nullptr : &it->second;
  }

  // Evaluate and coerce to a compile-time integer.
  long as_int(const mir::Expr& e) {
    Value v = eval(e);
    if (v.is_int && v.i.size() == 1) return v.i[0];
    if (v.r.size() == 1) return (long)val(v.r[0]);
    fail("expected int scalar", e.raw);
  }

  // Bind arguments positionally by declared type and evaluate the body to
  // its return value. This is the ODE entry point: real-typed parameters
  // consume `args` in order, int-typed ones consume `int_args`.
  std::vector<T> call(const mir::FunDef& f,
                      const std::vector<std::vector<T>>& args,
                      const std::vector<std::vector<int>>& int_args) {
    MirInterp sub(funs_, where_, hooks_);
    sub.propto_ctx_ = propto_ctx_;
    size_t ai = 0, ii = 0;
    for (size_t k = 0; k < f.arg_names.size(); ++k) {
      const bool is_int = f.arg_types[k].find("UInt") != std::string::npos;
      Value v;
      if (is_int && ii < int_args.size()) {
        v.is_int = true;
        v.i = int_args[ii++];
        v.r.assign(v.i.begin(), v.i.end());
        if (f.arg_types[k] != "UInt") v.dims = {(int64_t)v.i.size()};
      } else if (ai < args.size()) {
        v.r = args[ai++];
        if (f.arg_types[k] != "UReal") v.dims = {(int64_t)v.r.size()};
      }
      sub.env_[f.arg_names[k]] = std::move(v);
    }
    try {
      for (const auto& s : f.body) sub.exec(s);
    } catch (ReturnV& r) {
      return std::move(r.v.r);
    }
    fail("function returned no value: " + f.name);
  }

  Value eval(const mir::Expr& e) {
    Value r;
    switch (e.kind) {
      case mir::Expr::LitInt:
        r.is_int = true;
        r.i = {(int)e.lit_i};
        r.r = {T((double)e.lit_i)};
        return r;
      case mir::Expr::LitReal:
        r.r = {T(e.lit)};
        return r;
      case mir::Expr::Var: {
        Value* en = find(e.name);
        if (en) return *en;
        // The lowering evaluates data-only conditions of unrolled log_prob
        // loops here; the loop variables live in its own int environment.
        long iv = 0;
        if (hooks_.int_var && hooks_.int_var(e.name, &iv)) {
          r.is_int = true;
          r.i = {(int)iv};
          r.r = {T((double)iv)};
          return r;
        }
        fail("unknown variable " + e.name + " (type " + e.type_ + ")", e.raw);
      }
      case mir::Expr::TernaryIf: {
        const bool c = val(eval(e.args[0]).r.at(0)) != 0.0;
        return eval(e.args[c ? 1 : 2]);
      }
      case mir::Expr::EOr:
      case mir::Expr::EAnd: {
        // Short-circuit like the language does.
        const bool a = val(eval(e.args[0]).r.at(0)) != 0.0;
        bool v = a;
        if (e.kind == mir::Expr::EOr ? !a : a)
          v = val(eval(e.args[1]).r.at(0)) != 0.0;
        r.is_int = true;
        r.i = {v ? 1 : 0};
        r.r = {T(v ? 1.0 : 0.0)};
        return r;
      }
      case mir::Expr::Indexed:
        return eval_indexed(e);
      case mir::Expr::FunApp:
        return eval_fun(e);
      default:
        fail("unsupported expression", e.raw);
    }
  }

  // Execute a statement list to completion; a bare top-level Return (the
  // write_array emission guards end this way) stops it cleanly.
  void run(const std::vector<mir::Stmt>& body) {
    try {
      for (const auto& st : body) exec(st);
    } catch (ReturnV&) {
    }
  }

  void exec(const mir::Stmt& st) {
    if constexpr (std::is_same_v<T, double>) {
      if (hooks_.stmt && hooks_.stmt(st)) return;
    }
    switch (st.kind) {
      case mir::Stmt::Decl: {
        // Keep the declaration's evaluated geometry available to its later
        // FnCheck. A flat FnReadData assignment intentionally erases runtime
        // dims, and JSON [] cannot carry trailing zero-batch leaf extents.
        std::vector<int64_t>& declared_dims = decl_dims_[st.decl_id];
        declared_dims.clear();
        declared_dims.reserve(st.decl_type.dims.size());
        for (const auto& d : st.decl_type.dims)
          declared_dims.push_back(as_int(d));
        Value e;
        if (st.decl_type.base == "SInt" ||
            (st.decl_type.base == "SArray" && st.decl_type.elem_base == "SInt"))
          e.is_int = true;
        if (st.has_init && st.init.kind == mir::Expr::FunApp &&
            st.init.fn_lib == mir::Expr::Lib::Internal &&
            st.init.name == "FnReadData") {
          // Reads name the source data variable in their argument.
          e = read_data(st.init.args.at(0).lit_s, st.raw);
        } else if (st.has_init &&
                   !(st.init.kind == mir::Expr::FunApp &&
                     st.init.fn_lib == mir::Expr::Lib::Internal)) {
          const bool want_int = e.is_int;
          e = eval(st.init);
          if (want_int && !e.is_int) {
            // Declared int, computed through real arithmetic: coerce back.
            e.is_int = true;
            e.i.clear();
            for (const T& v : e.r) e.i.push_back((int)val(v));
          }
        } else if (const DataMap::Entry* dp = data_lookup(st.decl_id)) {
          e = from_entry(*dp);
        } else if (st.decl_type.base == "SInt" ||
                   st.decl_type.base == "SReal") {
          // Bare scalar decl. Reals fill with NaN, matching CmdStan's
          // uninitialized value: hmm_drive writes best_logp[1, K] where it
          // means best_logp[1, k], and its Viterbi only matches CmdStan's
          // because the never-written element loses every comparison.
          if (e.is_int) e.i = {0};
          e.r = {e.is_int ? T(0.0)
                          : T(std::numeric_limits<double>::quiet_NaN())};
        } else if (!st.decl_type.base.empty()) {
          // Bare sized decl: allocate so element writes work; real elements
          // are NaN until written (see the scalar case above).
          int64_t n = 1;
          for (int64_t d : declared_dims) n *= d;
          e.r.assign(n, e.is_int ? T(0.0)
                                 : T(std::numeric_limits<double>::quiet_NaN()));
          if (e.is_int) e.i.assign(n, 0);
          e.dims = declared_dims;
        }
        env_[st.decl_id] = std::move(e);
        return;
      }
      case mir::Stmt::Assignment: {
        // Data reads: the FnReadData argument names the source variable.
        std::string read_name;
        std::function<void(const mir::Expr&)> scan = [&](const mir::Expr& x) {
          if (x.kind == mir::Expr::FunApp && x.name == "FnReadData" &&
              !x.args.empty())
            read_name = x.args[0].lit_s;
          for (const auto& a : x.args) scan(a);
        };
        scan(st.rhs);
        if (!read_name.empty()) {
          // The flat read buffer is consumed with sequential 1-D indexing
          // regardless of the source variable's shape.
          Value flat = read_data(read_name, st.raw);
          const auto declared = decl_dims_.find(st.lhs);
          if (declared != decl_dims_.end() && declared->second.empty())
            flat.dims.clear();
          else
            flat.dims = {(int64_t)std::max(flat.r.size(), flat.i.size())};
          env_[st.lhs] = std::move(flat);
          return;
        }
        if (st.lhs_idx.empty()) {
          env_[st.lhs] = eval(st.rhs);
          return;
        }
        Value* en = find(st.lhs);
        if (!en) fail("assignment to unknown " + st.lhs);
        Value v = eval(st.rhs);
        if (st.lhs_idx.size() == 1 && st.lhs_idx[0].name == "IndexSingle") {
          const long ix = as_int(st.lhs_idx[0].args[0]);
          if (en->dims.size() == 2) {
            // Row write into a matrix (A[i] = row_vector), col-major strided.
            const int64_t R = en->dims[0], C = en->dims[1];
            if ((int64_t)v.r.size() != C) fail("row write size mismatch");
            for (int64_t j = 0; j < C; ++j) en->r.at(j * R + (ix - 1)) = v.r[j];
            return;
          }
          if ((size_t)ix > en->r.size())
            en->r.resize(ix, en->is_int
                                 ? T(0.0)
                                 : T(std::numeric_limits<double>::quiet_NaN()));
          en->r[ix - 1] = v.r.at(0);
          if (en->is_int) {
            if ((size_t)ix > en->i.size()) en->i.resize(ix, 0);
            en->i[ix - 1] =
                v.is_int && !v.i.empty() ? v.i[0] : (int)val(v.r.at(0));
          }
          return;
        }
        // General all-Single N-D element write.
        if (st.lhs_idx.size() == en->dims.size()) {
          bool all_single = true;
          for (const auto& ix : st.lhs_idx)
            if (ix.name != "IndexSingle") all_single = false;
          if (all_single) {
            int64_t flatpos = 0, stride = 1;
            for (size_t d = 0; d < en->dims.size(); ++d) {
              flatpos += (as_int(st.lhs_idx[d].args[0]) - 1) * stride;
              stride *= en->dims[d];
            }
            en->r.at(flatpos) = v.r.at(0);
            if (en->is_int)
              en->i.at(flatpos) =
                  v.is_int && !v.i.empty() ? v.i[0] : (int)val(v.r.at(0));
            return;
          }
        }
        // Column write Xc[:, j] = vector.
        if (st.lhs_idx.size() == 2 && st.lhs_idx[0].name == "IndexAll" &&
            st.lhs_idx[1].name == "IndexSingle" && en->dims.size() == 2) {
          const long j = as_int(st.lhs_idx[1].args[0]);
          const int64_t R = en->dims[0];
          for (int64_t i = 0; i < R; ++i) en->r.at((j - 1) * R + i) = v.r.at(i);
          return;
        }
        // Column-segment write X[a:b, j] = vector.
        if (st.lhs_idx.size() == 2 && st.lhs_idx[0].name == "IndexBetween" &&
            st.lhs_idx[1].name == "IndexSingle" && en->dims.size() == 2) {
          const long a = as_int(st.lhs_idx[0].args[0]);
          const long b = as_int(st.lhs_idx[0].args[1]);
          const long j = as_int(st.lhs_idx[1].args[0]);
          const int64_t R = en->dims[0];
          for (long k = a; k <= b; ++k)
            en->r.at((j - 1) * R + (k - 1)) = v.r.at((size_t)(k - a));
          return;
        }
        {
          std::string what = "unsupported indexed assignment: dims=" +
                             std::to_string(en->dims.size());
          for (const auto& ix : st.lhs_idx) what += " [" + ix.name + "]";
          fail(what, st.raw);
        }
      }
      case mir::Stmt::For: {
        const long lo = as_int(st.lower), hi = as_int(st.upper);
        for (long v = lo; v <= hi; ++v) {
          Value lv;
          lv.is_int = true;
          lv.i = {(int)v};
          lv.r = {T((double)v)};
          env_[st.loopvar] = lv;
          for (const auto& k : st.body) exec(k);
        }
        env_.erase(st.loopvar);
        return;
      }
      case mir::Stmt::IfElse: {
        const bool c = val(eval(st.cond).r.at(0)) != 0.0;
        if (c && !st.body.empty()) exec(st.body[0]);
        if (!c && st.body.size() > 1) exec(st.body[1]);
        return;
      }
      case mir::Stmt::Block:
      case mir::Stmt::SList:
        for (const auto& k : st.body) exec(k);
        return;
      case mir::Stmt::While: {
        int64_t guard = 0;
        while (val(eval(st.cond).r.at(0)) != 0.0) {
          if (++guard > 100000000) fail("while loop did not terminate");
          for (const auto& k : st.body) exec(k);
        }
        return;
      }
      case mir::Stmt::Return:
        throw ReturnV{st.has_init ? eval(st.rhs) : Value{}};
      case mir::Stmt::NRFunApp:
        // Constraint checks are not executed here, but reject() and
        // print() are: a `reject` in transformed data is how a model
        // validates its data, and CmdStan fails to construct the model
        // there rather than sampling from a model whose data is wrong.
        // Skipping it would mean stanli happily sampled a model CmdStan
        // refuses to build.
        if (st.fn_name == "FnReject" || st.fn_name == "FnPrint") {
          std::string msg;
          for (const auto& a : st.fn_args) {
            if (a.kind == mir::Expr::LitStr) {
              msg += a.lit_s;
              continue;
            }
            const Value v = eval(a);
            if (v.r.size() == 1) {
              msg += fmt_num(val(v.r[0]));
            } else {
              msg += '[';
              for (size_t i = 0; i < v.r.size(); ++i) {
                if (i) msg += ',';
                msg += fmt_num(val(v.r[i]));
              }
              msg += ']';
            }
          }
          if (st.fn_name == "FnReject") throw std::domain_error(msg);
          emit_message(msg);
          return;
        }
        if (st.fn_name == "FnCheck") {
          exec_check(st);
          return;
        }
        if (st.fn_name == "check_matching_dims") {
          exec_matching_dims(st);
          return;
        }
        // These size-validation statements remain a narrow compatibility
        // debt. `check_greater_or_equal` is only a static shape check in the
        // existing newtrans input; the function name is not generally pure.
        // New names must never silently join this list.
        if (st.fn_name == "FnValidateSize" ||
            st.fn_name == "FnValidateSizePositive" ||
            st.fn_name == "check_greater_or_equal")
          return;
        fail("unsupported statement function " + st.fn_name, st.raw);
      case mir::Stmt::Skip:
        return;  // constraint checks are not executed here
      default:
        fail("unsupported statement", st.raw);
    }
  }

 private:
  // How reject()/print() render a number. ostream's default formatting is
  // what the OP_REJECT kernel uses on the graph side, so the two paths
  // spell the same value the same way -- and it is also what stan-math's
  // own reject produces.
  static std::string fmt_num(double v) {
    std::ostringstream os;
    os << v;
    return os.str();
  }

  // Thrown by a Return statement inside an interpreted function body.
  struct ReturnV {
    Value v;
  };

  const std::map<std::string, const mir::FunDef*>& funs_;
  std::string where_;
  MirHooks hooks_;
  std::map<std::string, Value> env_;
  std::map<std::string, std::vector<int64_t>> decl_dims_;
  int udf_depth_ = 0;
  bool propto_ctx_ = true;

  [[noreturn]] void fail(const std::string& msg,
                         const std::string& raw = "") const {
    throw CompileError("stanli " + where_ + ": " + msg +
                       (raw.empty() ? "" : " | in: " + raw));
  }

  static double val(const T& x) { return stan::math::value_of(x); }

  // Arity of a bound transform called as a function, or 0 for any other
  // name. The three directions of one transform share an arity, so the name
  // alone decides how many arguments to expect.
  static size_t bound_transform_arity(const std::string& name) {
    static const std::pair<const char*, size_t> kStems[] = {
        {"lower_bound_", 2},
        {"upper_bound_", 2},
        {"lower_upper_bound_", 3},
        {"offset_multiplier_", 3}};
    for (const auto& stem : kStems) {
      const std::string prefix(stem.first);
      if (name.compare(0, prefix.size(), prefix) != 0) continue;
      const std::string tail = name.substr(prefix.size());
      if (tail == "constrain" || tail == "jacobian" || tail == "unconstrain")
        return stem.second;
    }
    return 0;
  }

  // One element of a bound transform, through stan-math's own scalar
  // overloads: the free direction is stan-math's inverse rather than a
  // hand-written one, so the two directions cannot drift apart here. `b2` is
  // unread by the two-argument transforms.
  static T bound_transform(const std::string& name, const T& x, const T& b1,
                           const T& b2) {
    if (name == "lower_bound_unconstrain") return stan::math::lb_free(x, b1);
    if (name == "upper_bound_unconstrain") return stan::math::ub_free(x, b1);
    if (name == "lower_upper_bound_unconstrain")
      return stan::math::lub_free(x, b1, b2);
    if (name == "offset_multiplier_unconstrain")
      return stan::math::offset_multiplier_free(x, b1, b2);
    // The constrain and jacobian directions differ only in a target
    // increment, and this path has no target.
    if (name == "lower_bound_constrain" || name == "lower_bound_jacobian")
      return stan::math::lb_constrain(x, b1);
    if (name == "upper_bound_constrain" || name == "upper_bound_jacobian")
      return stan::math::ub_constrain(x, b1);
    if (name == "lower_upper_bound_constrain" ||
        name == "lower_upper_bound_jacobian")
      return stan::math::lub_constrain(x, b1, b2);
    return stan::math::offset_multiplier_constrain(x, b1, b2);
  }

  static bool check_scalar_type(const mir::Expr& e) {
    return e.unsized.depth == 0 && (e.unsized.leaf == mir::UnsizedLeaf::Int ||
                                    e.unsized.leaf == mir::UnsizedLeaf::Real);
  }

  static bool check_container_type(const mir::Expr& e) {
    if (e.unsized.leaf == mir::UnsizedLeaf::Unknown ||
        e.unsized.leaf == mir::UnsizedLeaf::Complex)
      return false;
    return e.unsized.depth != 0 || e.unsized.leaf == mir::UnsizedLeaf::Vector ||
           e.unsized.leaf == mir::UnsizedLeaf::RowVector ||
           e.unsized.leaf == mir::UnsizedLeaf::Matrix;
  }

  std::vector<int64_t> check_dims(const mir::Expr& e, const Value& v) const {
    // FnReadData is a sequential flat buffer, so its assignment erases a
    // declaration's dimensions in env_. The source DataMap still owns the
    // complete logical shape. Computed transformed-data values keep theirs.
    if (e.kind == mir::Expr::Var) {
      auto decl = decl_dims_.find(e.name);
      if (decl != decl_dims_.end()) return decl->second;
      if (const DataMap::Entry* d = data_lookup(e.name)) return d->dims;
    }
    return v.dims;
  }

  [[noreturn]] void check_shape_fail(const std::string& name) const {
    throw std::invalid_argument("stanli " + where_ +
                                ": constraint shapes do not match for " + name);
  }

  void exec_matching_dims(const mir::Stmt& st) {
    if (st.fn_args.size() != 5 || st.fn_args[0].kind != mir::Expr::LitStr ||
        st.fn_args[1].kind != mir::Expr::LitStr ||
        st.fn_args[3].kind != mir::Expr::LitStr)
      fail("malformed check_matching_dims", st.raw);
    const Value a = eval(st.fn_args[2]);
    const Value b = eval(st.fn_args[4]);
    const bool a_scalar = check_scalar_type(st.fn_args[2]);
    const bool b_scalar = check_scalar_type(st.fn_args[4]);
    const bool a_container = check_container_type(st.fn_args[2]);
    const bool b_container = check_container_type(st.fn_args[4]);
    if ((!a_scalar && !a_container) || (!b_scalar && !b_container))
      fail("unsupported check_matching_dims operand type", st.raw);
    const bool shapes_match =
        (a_scalar && b_scalar) ||
        (a_container && b_container &&
         st.fn_args[2].unsized.depth == st.fn_args[4].unsized.depth &&
         st.fn_args[2].unsized.leaf == st.fn_args[4].unsized.leaf &&
         check_dims(st.fn_args[2], a) == check_dims(st.fn_args[4], b) &&
         a.r.size() == b.r.size());
    if (!shapes_match) check_shape_fail(st.fn_args[1].lit_s);
  }

  // Execute stanc's generated declaration check once, at the statement's
  // original position. Stan Math spells these as negated comparisons so a
  // NaN in either operand rejects rather than slipping through.
  void exec_check(const mir::Stmt& st) {
    if (!st.check_transform) fail("malformed FnCheck", st.raw);
    const auto kind = st.check_transform->kind;
    if (mir::is_structured_check(kind)) {
      if (!st.check_transform->args.empty() || st.fn_args.size() != 1)
        fail("malformed structured FnCheck", st.raw);
      const Value y = eval(st.fn_args[0]);
      StructuredCheckSpec spec;
      spec.kind = kind;
      spec.storage = StructuredStorage::FirstIndexFast;
      spec.name =
          st.check_var_name.empty() ? st.fn_args[0].name : st.check_var_name;
      if (st.fn_args[0].unsized.leaf == mir::UnsizedLeaf::Vector)
        spec.leaf = StructuredLeaf::Vector;
      else if (st.fn_args[0].unsized.leaf == mir::UnsizedLeaf::Matrix)
        spec.leaf = StructuredLeaf::Matrix;
      else
        fail("structured FnCheck requires vector or matrix leaves", st.raw);
      spec.dims = check_dims(st.fn_args[0], y);
      const size_t leaf_rank = spec.leaf == StructuredLeaf::Matrix ? 2 : 1;
      if (spec.dims.size() != st.fn_args[0].unsized.depth + leaf_rank)
        fail("structured FnCheck dimensions do not match its type", st.raw);

      int64_t expected = 1;
      for (int64_t d : spec.dims) {
        if (d < 0 ||
            (d != 0 && expected > std::numeric_limits<int64_t>::max() / d))
          fail("invalid or overflowing structured FnCheck extent", st.raw);
        expected *= d;
      }
      if (expected != static_cast<int64_t>(y.r.size()))
        fail("structured FnCheck width does not match its dimensions", st.raw);

      if constexpr (std::is_same_v<T, double>) {
        check_structured_value(y.r.data(), expected, spec);
      } else {
        std::vector<double> values;
        values.reserve(y.r.size());
        for (const T& v : y.r) values.push_back(val(v));
        check_structured_value(values.data(), expected, spec);
      }
      return;
    }
    if (kind != mir::Transform::Lower && kind != mir::Transform::Upper)
      fail("unsupported FnCheck transform", st.raw);
    if (st.check_transform->args.size() != 1 || st.fn_args.size() != 2)
      fail("malformed FnCheck", st.raw);

    const Value y = eval(st.fn_args[0]);
    const Value bound = eval(st.fn_args[1]);
    const std::string& name =
        st.check_var_name.empty() ? st.fn_args[0].name : st.check_var_name;
    const bool y_scalar = check_scalar_type(st.fn_args[0]);
    const bool y_container = check_container_type(st.fn_args[0]);
    const bool b_scalar = check_scalar_type(st.fn_args[1]);
    const bool b_container = check_container_type(st.fn_args[1]);
    if ((!y_scalar && !y_container) || (!b_scalar && !b_container))
      fail("unsupported FnCheck operand type", st.raw);
    if (y_scalar && (y.r.size() != 1 || !b_scalar)) check_shape_fail(name);
    if (b_scalar && bound.r.size() != 1) check_shape_fail(name);
    if (y_container && b_container) {
      if (st.fn_args[0].unsized.depth != st.fn_args[1].unsized.depth ||
          st.fn_args[0].unsized.leaf != st.fn_args[1].unsized.leaf ||
          check_dims(st.fn_args[0], y) != check_dims(st.fn_args[1], bound) ||
          y.r.size() != bound.r.size())
        check_shape_fail(name);
    }
    for (size_t i = 0; i < y.r.size(); ++i) {
      const double b = val(bound.r[b_scalar ? 0 : i]);
      if (kind == mir::Transform::Lower)
        stan::math::check_greater_or_equal("stanli MIR check", name.c_str(),
                                           val(y.r[i]), b);
      else
        stan::math::check_less_or_equal("stanli MIR check", name.c_str(),
                                        val(y.r[i]), b);
    }
  }

  // max-shifted log-sum-exp over a buffer; -inf on an empty or all
  // -inf input rather than NaN.
  static T lse(const std::vector<T>& xs) {
    T m = T(-std::numeric_limits<double>::infinity());
    for (const T& x : xs)
      if (val(x) > val(m)) m = x;
    if (!(val(m) > -std::numeric_limits<double>::infinity())) return m;
    T s = T(0.0);
    for (const T& x : xs) s += stan::math::exp(x - m);
    return m + stan::math::log(s);
  }

  static Value from_entry(const DataMap::Entry& d) {
    if constexpr (std::is_same_v<T, double>) {
      return d;
    } else {
      Value v;
      v.is_int = d.is_int;
      v.r.assign(d.r.begin(), d.r.end());
      v.i = d.i;
      v.dims = d.dims;
      return v;
    }
  }

  const DataMap::Entry* data_lookup(const std::string& name) const {
    return hooks_.data ? hooks_.data(name) : nullptr;
  }

  Value read_data(const std::string& name, const std::string& raw) {
    const DataMap::Entry* dp = data_lookup(name);
    if (!dp) fail("unknown data variable " + name, raw);
    return from_entry(*dp);
  }

  Value call_udf(const mir::Expr& e) {
    auto it = funs_.find(e.name);
    if (it == funs_.end()) fail("unknown function " + e.name, e.raw);
    const mir::FunDef& f = *it->second;
    if (e.args.size() != f.arg_names.size()) fail(e.name + " arity mismatch");
    if (udf_depth_ > 64) fail("UDF recursion too deep");
    // Function bodies see their arguments and nothing else.
    MirInterp sub(funs_, where_, hooks_);
    sub.udf_depth_ = udf_depth_ + 1;
    sub.propto_ctx_ = propto_ctx_ && e.fn_propto;
    for (size_t i = 0; i < e.args.size(); ++i)
      sub.env_[f.arg_names[i]] = eval(e.args[i]);
    try {
      for (const auto& st : f.body) sub.exec(st);
    } catch (ReturnV& r) {
      return std::move(r.v);
    }
    return Value{};
  }

  Value eval_indexed(const mir::Expr& e) {
    Value r;
    // Index a named value in place. Evaluating the base by value copies
    // the whole array per read, which is quadratic when a loop indexes
    // a large data array (60k-row models spent minutes here).
    const Value* base_ptr = nullptr;
    Value base_storage;
    if (e.args[0].kind == mir::Expr::Var) base_ptr = find(e.args[0].name);
    if (base_ptr == nullptr) {
      base_storage = eval(e.args[0]);
      base_ptr = &base_storage;
    }
    const Value& base = *base_ptr;
    if (e.args.size() == 2 && e.args[1].name == "IndexSingle" &&
        base.dims.size() <= 1) {
      const long ix = as_int(e.args[1].args[0]);
      r.is_int = base.is_int;
      if (base.is_int) r.i = {base.i.at(ix - 1)};
      r.r = {base.r.at(ix - 1)};
      return r;
    }
    // Row of a 2-D array (col-major storage), spelled either X[i] or
    // X[i, :].
    if (base.dims.size() == 2 &&
        ((e.args.size() == 2 && e.args[1].name == "IndexSingle") ||
         (e.args.size() == 3 && e.args[1].name == "IndexSingle" &&
          e.args[2].name == "IndexAll"))) {
      const long i = as_int(e.args[1].args[0]);
      const int64_t R = base.dims[0], C = base.dims[1];
      r.is_int = base.is_int;
      r.dims = {C};
      for (int64_t j = 0; j < C; ++j) {
        r.r.push_back(base.r.at(j * R + (i - 1)));
        if (base.is_int) r.i.push_back(base.i.at(j * R + (i - 1)));
      }
      return r;
    }
    if (e.args.size() == 2 && e.args[1].name == "IndexAll") return base;
    // General all-Single N-D element access (col-major strides).
    if (e.args.size() == base.dims.size() + 1) {
      bool all_single = true;
      for (size_t k = 1; k < e.args.size(); ++k)
        if (e.args[k].name != "IndexSingle") all_single = false;
      if (all_single) {
        int64_t flatpos = 0, stride = 1;
        for (size_t d = 0; d < base.dims.size(); ++d) {
          flatpos += (as_int(e.args[1 + d].args[0]) - 1) * stride;
          stride *= base.dims[d];
        }
        r.is_int = base.is_int;
        if (base.is_int) r.i = {base.i.at(flatpos)};
        r.r = {base.r.at(flatpos)};
        return r;
      }
    }
    // Column slice X[:, j] on a matrix: contiguous in col-major.
    if (e.args.size() == 3 && e.args[1].name == "IndexAll" &&
        e.args[2].name == "IndexSingle" && base.dims.size() == 2) {
      const long j = as_int(e.args[2].args[0]);
      const int64_t R = base.dims[0];
      r.is_int = base.is_int;
      r.dims = {R};
      r.r.assign(base.r.begin() + (j - 1) * R, base.r.begin() + j * R);
      if (base.is_int)
        r.i.assign(base.i.begin() + (j - 1) * R, base.i.begin() + j * R);
      return r;
    }
    // Leading-Single slice of an N-D entry (k > 2): first index fixed.
    // Flat storage is Fortran (first index fastest), so the sub-tensor
    // elements sit at (i-1) + d0 * t.
    if (e.args.size() == 2 && e.args[1].name == "IndexSingle" &&
        base.dims.size() > 2) {
      const long i = as_int(e.args[1].args[0]);
      const int64_t d0 = base.dims[0];
      int64_t rest = 1;
      for (size_t d = 1; d < base.dims.size(); ++d) rest *= base.dims[d];
      r.is_int = base.is_int;
      r.dims.assign(base.dims.begin() + 1, base.dims.end());
      for (int64_t k = 0; k < rest; ++k) {
        r.r.push_back(base.r.at((i - 1) + d0 * k));
        if (base.is_int) r.i.push_back(base.i.at((i - 1) + d0 * k));
      }
      return r;
    }
    // Gather v[idx] by an int-array index on a 1-D value.
    if (e.args.size() == 2 && e.args[1].name == "IndexMulti" &&
        base.dims.size() <= 1) {
      Value ix = eval(e.args[1].args[0]);
      const size_t n = std::max(ix.i.size(), ix.r.size());
      r.is_int = base.is_int;
      r.dims = {(int64_t)n};
      for (size_t k = 0; k < n; ++k) {
        const long p = k < ix.i.size() ? ix.i[k] : (long)val(ix.r.at(k));
        r.r.push_back(base.r.at((size_t)(p - 1)));
        if (base.is_int) r.i.push_back(base.i.at((size_t)(p - 1)));
      }
      return r;
    }
    // Between subrange of a 1-D value: v[a:b].
    if (e.args.size() == 2 && e.args[1].name == "IndexBetween" &&
        base.dims.size() <= 1) {
      const long a = as_int(e.args[1].args[0]);
      const long b = as_int(e.args[1].args[1]);
      r.is_int = base.is_int;
      r.dims = {b - a + 1};
      for (long k = a; k <= b; ++k) {
        r.r.push_back(base.r.at(k - 1));
        if (base.is_int) r.i.push_back(base.i.at(k - 1));
      }
      return r;
    }
    // X[i, a:b] on a matrix / 2-D array: columns a..b of row i.
    if (e.args.size() == 3 && e.args[1].name == "IndexSingle" &&
        e.args[2].name == "IndexBetween" && base.dims.size() == 2) {
      const long i = as_int(e.args[1].args[0]);
      const long a = as_int(e.args[2].args[0]);
      const long b = as_int(e.args[2].args[1]);
      const int64_t R = base.dims[0];
      r.is_int = base.is_int;
      r.dims = {b - a + 1};
      for (long j = a; j <= b; ++j) {
        r.r.push_back(base.r.at((j - 1) * R + (i - 1)));
        if (base.is_int) r.i.push_back(base.i.at((j - 1) * R + (i - 1)));
      }
      return r;
    }
    // X[a:b, j] on a matrix / 2-D array: rows a..b of column j.
    if (e.args.size() == 3 && e.args[1].name == "IndexBetween" &&
        e.args[2].name == "IndexSingle" && base.dims.size() == 2) {
      const long a = as_int(e.args[1].args[0]);
      const long b = as_int(e.args[1].args[1]);
      const long j = as_int(e.args[2].args[0]);
      const int64_t R = base.dims[0];
      r.is_int = base.is_int;
      r.dims = {b - a + 1};
      for (long k = a; k <= b; ++k) {
        r.r.push_back(base.r.at((j - 1) * R + (k - 1)));
        if (base.is_int) r.i.push_back(base.i.at((j - 1) * R + (k - 1)));
      }
      return r;
    }
    {
      std::string what =
          "unsupported index: dims=" + std::to_string(base.dims.size());
      for (size_t k = 1; k < e.args.size(); ++k)
        what += " [" + e.args[k].name + "]";
      fail(what, e.raw);
    }
  }

  Value eval_fun(const mir::Expr& e) {
    Value r;
    if (e.fn_lib == mir::Expr::Lib::UserDefined) return call_udf(e);
    const auto is_scalar = [](const Value& v) {
      return v.dims.empty() && v.r.size() == 1;
    };
    const auto broadcast_dims = [](const Value& a, const Value& b) {
      if (a.dims.empty() != b.dims.empty())
        return a.dims.empty() ? b.dims : a.dims;
      if (a.r.size() > b.r.size()) return a.dims;
      if (b.r.size() > a.r.size()) return b.dims;
      return a.dims.empty() ? b.dims : a.dims;
    };
    const auto broadcast_size = [&](const Value& a, const Value& b) {
      if (is_scalar(a)) return b.r.size();
      if (is_scalar(b)) return a.r.size();
      return a.r.size();
    };
    const auto shapes_match = [&](const Value& a, const Value& b) {
      return is_scalar(a) || is_scalar(b) ||
             (a.dims == b.dims && a.r.size() == b.r.size());
    };
    // Elementwise with scalar broadcasting; results of real math are real
    // even on int inputs, and binary int results stay int only when both
    // sides are int scalars.
    auto bin = [&](auto f) {
      Value a = eval(e.args[0]), b = eval(e.args[1]);
      Value o;
      if (!shapes_match(a, b)) fail(e.name + ": incompatible shapes", e.raw);
      const size_t n = broadcast_size(a, b);
      o.r.resize(n);
      for (size_t i = 0; i < n; ++i)
        o.r[i] = f(a.r[a.r.size() == 1 ? 0 : i], b.r[b.r.size() == 1 ? 0 : i]);
      o.dims = broadcast_dims(a, b);
      if (a.is_int && b.is_int && a.i.size() == 1 && b.i.size() == 1) {
        o.is_int = true;
        o.i = {(int)val(f(T((double)a.i[0]), T((double)b.i[0])))};
      }
      return o;
    };
    // Two-argument scalar math with one int argument (bessel_first_kind
    // and friends): bin's broadcast and shape rules, but the int side
    // reaches stan-math as an int, which is what those overloads take.
    // `int_first` says which position holds it. No layout correction like
    // the graph kernel's -- every container here is column-major, arrays
    // included, so a matrix and an int array of the same dims already pair
    // element for element, which is the pairing stan-math makes.
    auto bin_int = [&](bool int_first, auto f) {
      Value a = eval(e.args[0]), b = eval(e.args[1]);
      if (!shapes_match(a, b)) fail(e.name + ": incompatible shapes", e.raw);
      const Value& re = int_first ? b : a;
      const Value& iv = int_first ? a : b;
      Value o;
      const size_t n = broadcast_size(a, b);
      o.r.resize(n);
      o.dims = broadcast_dims(a, b);
      const bool rs = re.r.size() == 1, is = iv.r.size() == 1;
      for (size_t i = 0; i < n; ++i) {
        const size_t k = is ? 0 : i;
        const int q = iv.is_int && k < iv.i.size()
                          ? iv.i[k]
                          : (int)std::llround(val(iv.r[k]));
        o.r[i] = f(re.r[rs ? 0 : i], q);
      }
      // Only falling_factorial and rising_factorial have an int,int
      // overload that answers int; everywhere else two int arguments still
      // make a real, so stanc's own result type decides rather than the
      // arguments.
      if (e.type_ == "UInt" && n == 1) {
        o.is_int = true;
        o.i = {(int)val(o.r[0])};
      }
      return o;
    };
    auto un = [&](auto f) {
      Value a = eval(e.args[0]);
      Value o;
      o.dims = a.dims;
      o.r.resize(a.r.size());
      for (size_t i = 0; i < a.r.size(); ++i) o.r[i] = f(a.r[i]);
      return o;
    };
    // The comparison operators: compare values, answer 1.0 or 0.0, and
    // otherwise take bin's broadcasting and int-scalar rules unchanged.
    auto cmp = [&](auto f) {
      return bin([f](const T& x, const T& y) {
        return T(f(val(x), val(y)) ? 1.0 : 0.0);
      });
    };
    // add/subtract/multiply/elt_multiply/divide/elt_divide are the named
    // spellings of the binary operators, and they are taught here beside
    // the operators rather than in a table of their own: the graph
    // lowering knows them too, and teaching only one side is what let a
    // vectorized log_sum_exp answer transformed data with the wrong value
    // and no exception.
    if (e.name == "Plus__" || e.name == "add")
      return bin([](const T& x, const T& y) { return x + y; });
    if (e.name == "Minus__" || e.name == "subtract")
      return bin([](const T& x, const T& y) { return x - y; });
    if (e.name == "Times__" || e.name == "multiply") {
      // Times on shaped operands is linear algebra, not elementwise; only
      // a scalar operand (either side) scales elementwise.
      Value a = eval(e.args[0]), b = eval(e.args[1]);
      const bool a_mat = a.dims.size() == 2, b_mat = b.dims.size() == 2;
      if (!is_scalar(a) && !is_scalar(b) && (a_mat || b_mat)) {
        const int64_t Ra = a_mat ? a.dims[0] : 1;
        const int64_t Ca = a_mat ? a.dims[1] : (int64_t)a.r.size();
        const int64_t Rb = b_mat ? b.dims[0] : (int64_t)b.r.size();
        const int64_t Cb = b_mat ? b.dims[1] : 1;
        if (Ca != Rb) fail(e.name + ": inner dimension mismatch", e.raw);
        // Col-major storage on both sides.
        r.r.assign((size_t)(Ra * Cb), T(0.0));
        for (int64_t j = 0; j < Cb; ++j)
          for (int64_t k = 0; k < Ca; ++k) {
            const T& bv = b.r.at((size_t)(j * Rb + k));
            for (int64_t i = 0; i < Ra; ++i)
              r.r[(size_t)(j * Ra + i)] +=
                  a.r.at((size_t)(a_mat ? k * Ra + i : k)) * bv;
          }
        if (a_mat && b_mat)
          r.dims = {Ra, Cb};
        else
          r.dims = {(int64_t)r.r.size()};
        return r;
      }
      if (!is_scalar(a) && !is_scalar(b) && !a_mat && !b_mat) {
        // vector * row_vector is an outer product when the result is a
        // matrix; row_vector * vector is a dot product when it is a scalar.
        if (e.type_ == "UMatrix") {
          const int64_t nr = (int64_t)a.r.size(), nc = (int64_t)b.r.size();
          r.dims = {nr, nc};
          for (int64_t j = 0; j < nc; ++j)
            for (int64_t i = 0; i < nr; ++i)
              r.r.push_back(a.r[(size_t)i] * b.r[(size_t)j]);
          return r;
        }
        if (e.type_ == "UReal" || e.type_ == "UInt") {
          if (a.r.size() != b.r.size())
            fail(e.name + ": dot product length mismatch", e.raw);
          T s = T(0.0);
          for (size_t i = 0; i < a.r.size(); ++i) s += a.r[i] * b.r[i];
          r.r = {s};
          return r;
        }
      }
      // Scalar scale, elementwise on the already-evaluated operands (an
      // argument may hold an RNG call; evaluating twice would draw twice).
      if (a.r.size() != b.r.size() && !is_scalar(a) && !is_scalar(b))
        fail(e.name + ": incompatible lengths", e.raw);
      const size_t n = broadcast_size(a, b);
      r.r.resize(n);
      for (size_t i = 0; i < n; ++i)
        r.r[i] = a.r[a.r.size() == 1 ? 0 : i] * b.r[b.r.size() == 1 ? 0 : i];
      r.dims = broadcast_dims(a, b);
      if (a.is_int && b.is_int && a.i.size() == 1 && b.i.size() == 1) {
        r.is_int = true;
        r.i = {a.i[0] * b.i[0]};
      }
      return r;
    }
    if (e.name == "EltTimes__" || e.name == "elt_multiply")
      return bin([](const T& x, const T& y) { return x * y; });
    // `A \ v` and `rv / A` are linear solves. stanc spells them with the
    // ordinary division operators, so the divisor's type is what tells a
    // solve from elementwise division by a scalar; `./` is never a solve.
    if (e.name == "LDivide__" ||
        (e.name == "Divide__" && e.args.at(1).type_ == "UMatrix")) {
      const bool left = e.name == "LDivide__";
      Value a = eval(e.args[0]), b = eval(e.args[1]);
      const Value& divisor = left ? a : b;
      const Value& dividend = left ? b : a;
      if (divisor.dims.size() != 2)
        fail(e.name + ": divisor is not a matrix", e.raw);
      using Mat = Eigen::Matrix<T, Eigen::Dynamic, Eigen::Dynamic>;
      // A non-matrix operand is the vector its side implies -- a column
      // under `\`, a row under `/` -- the same rule Times__ follows.
      const auto shaped = [](const Value& v, bool column) {
        const Eigen::Index rows = v.dims.size() == 2
                                      ? (Eigen::Index)v.dims[0]
                                      : (column ? (Eigen::Index)v.r.size() : 1);
        const Eigen::Index cols = v.dims.size() == 2
                                      ? (Eigen::Index)v.dims[1]
                                      : (column ? 1 : (Eigen::Index)v.r.size());
        Mat m(rows, cols);
        for (Eigen::Index j = 0; j < cols; ++j)
          for (Eigen::Index i = 0; i < rows; ++i)
            m(i, j) = v.r.at((size_t)(j * rows + i));
        return m;
      };
      const Mat d = shaped(divisor, true);
      // stan-math solves through whatever Eigen type it is handed, and a
      // vector passed as a one-column matrix takes the matrix code paths,
      // which reassociate -- 1 ULP against CmdStan, measured on `A \ v`. So
      // a vector goes in as a vector, which is also what the graph kernels
      // do (runtime/kernels/matrix_fns.cpp).
      //
      // stan-math checks squareness and the shared extent, and throws the
      // std::invalid_argument CmdStan would.
      Mat out;
      if (dividend.dims.size() == 2) {
        out = left ? stan::math::mdivide_left(d, shaped(dividend, left))
                   : stan::math::mdivide_right(shaped(dividend, left), d);
      } else if (left) {
        Eigen::Matrix<T, Eigen::Dynamic, 1> x(dividend.r.size());
        for (size_t i = 0; i < dividend.r.size(); ++i)
          x((Eigen::Index)i) = dividend.r[i];
        out = stan::math::mdivide_left(d, x);
      } else {
        Eigen::Matrix<T, 1, Eigen::Dynamic> x(dividend.r.size());
        for (size_t i = 0; i < dividend.r.size(); ++i)
          x((Eigen::Index)i) = dividend.r[i];
        out = stan::math::mdivide_right(x, d);
      }
      r.r.resize((size_t)(out.rows() * out.cols()));
      for (Eigen::Index j = 0; j < out.cols(); ++j)
        for (Eigen::Index i = 0; i < out.rows(); ++i)
          r.r[(size_t)(j * out.rows() + i)] = out(i, j);
      if (e.type_ == "UMatrix")
        r.dims = {(int64_t)out.rows(), (int64_t)out.cols()};
      else
        r.dims = {(int64_t)r.r.size()};
      return r;
    }
    // `divide` reaches here and never the solve above: stan::math::divide
    // divides by a scalar or divides a scalar elementwise, and has no
    // matrix-divisor overload at all.
    //
    // Its int,int overload is the exception to that elementwise reading:
    // it truncates and refuses a zero denominator, where the real
    // division below would answer 3.5 for `divide(7, 2)`. The value the
    // caller sees is `r`, not `i` -- lower.cpp's fold_const takes r[0]
    // for a UInt result -- so the truncation has to land in both.
    if ((e.name == "divide" || e.name == "elt_divide") && e.type_ == "UInt") {
      const int x = (int)as_int(e.args[0]), y = (int)as_int(e.args[1]);
      // divide is the one with the zero check; elt_divide is a bare `/`,
      // so a zero denominator there is undefined behavior and refused.
      if (e.name == "elt_divide" && y == 0)
        fail("integer division by zero", e.raw);
      const int q = e.name == "divide" ? stan::math::divide(x, y) : x / y;
      r.is_int = true;
      r.i = {q};
      r.r = {T((double)q)};
      return r;
    }
    if (e.name == "Divide__" || e.name == "EltDivide__" || e.name == "divide" ||
        e.name == "elt_divide")
      return bin([](const T& x, const T& y) { return x / y; });
    // `%` and `%/%`. Both operands are int by stanc's typing, so these are
    // C++ integer operators -- truncated toward zero -- and not fmod and
    // real division rounded afterwards, which disagree on negatives.
    // stan::math::modulus is what CmdStan calls, down to the exception it
    // throws on a zero divisor; `%/%` becomes a bare C++ `/`, where a zero
    // divisor is undefined behavior, so this refuses it instead.
    if (e.name == "Modulo__" || e.name == "IntDivide__") {
      const long x = as_int(e.args[0]), y = as_int(e.args[1]);
      long q;
      if (e.name == "Modulo__") {
        q = stan::math::modulus((int)x, (int)y);
      } else {
        if (y == 0) fail("integer division by zero", e.raw);
        q = x / y;
      }
      r.is_int = true;
      r.i = {(int)q};
      r.r = {T((double)q)};
      return r;
    }
    if (e.name == "Pow__" || e.name == "pow")
      return bin([](const T& x, const T& y) { return stan::math::pow(x, y); });
    if (e.name == "fmax")
      return bin([](const T& x, const T& y) { return stan::math::fmax(x, y); });
    if (e.name == "fmin")
      return bin([](const T& x, const T& y) { return stan::math::fmin(x, y); });
    if (e.name == "atan2")
      return bin(
          [](const T& x, const T& y) { return stan::math::atan2(x, y); });
    if (e.name == "beta" && e.args.size() == 2)
      return bin([](const T& x, const T& y) { return stan::math::beta(x, y); });
    if (e.name == "fdim")
      return bin([](const T& x, const T& y) { return stan::math::fdim(x, y); });
    if (e.name == "fmod")
      return bin([](const T& x, const T& y) { return stan::math::fmod(x, y); });
    if (e.name == "gamma_p")
      return bin(
          [](const T& x, const T& y) { return stan::math::gamma_p(x, y); });
    if (e.name == "gamma_q")
      return bin(
          [](const T& x, const T& y) { return stan::math::gamma_q(x, y); });
    if (e.name == "hypot")
      return bin(
          [](const T& x, const T& y) { return stan::math::hypot(x, y); });
    if (e.name == "lbeta")
      return bin(
          [](const T& x, const T& y) { return stan::math::lbeta(x, y); });
    if (e.name == "lchoose" || e.name == "binomial_coefficient_log")
      return bin([](const T& x, const T& y) {
        return stan::math::binomial_coefficient_log(x, y);
      });
    if (e.name == "log_falling_factorial")
      return bin([](const T& x, const T& y) {
        return stan::math::log_falling_factorial(x, y);
      });
    if (e.name == "log_inv_logit_diff")
      return bin([](const T& x, const T& y) {
        return stan::math::log_inv_logit_diff(x, y);
      });
    if (e.name == "log_modified_bessel_first_kind")
      return bin([](const T& x, const T& y) {
        return stan::math::log_modified_bessel_first_kind(x, y);
      });
    if (e.name == "log_rising_factorial")
      return bin([](const T& x, const T& y) {
        return stan::math::log_rising_factorial(x, y);
      });
    if (e.name == "owens_t")
      return bin(
          [](const T& x, const T& y) { return stan::math::owens_t(x, y); });
    if (e.name == "bessel_first_kind")
      return bin_int(true, [](const T& x, int k) {
        return stan::math::bessel_first_kind(k, x);
      });
    if (e.name == "bessel_second_kind")
      return bin_int(true, [](const T& x, int k) {
        return stan::math::bessel_second_kind(k, x);
      });
    if (e.name == "modified_bessel_first_kind")
      return bin_int(true, [](const T& x, int k) {
        return stan::math::modified_bessel_first_kind(k, x);
      });
    if (e.name == "modified_bessel_second_kind")
      return bin_int(true, [](const T& x, int k) {
        return stan::math::modified_bessel_second_kind(k, x);
      });
    if (e.name == "binary_log_loss")
      return bin_int(true, [](const T& x, int k) {
        return stan::math::binary_log_loss(k, x);
      });
    if (e.name == "lmgamma")
      return bin_int(
          true, [](const T& x, int k) { return stan::math::lmgamma(k, x); });
    if (e.name == "falling_factorial")
      return bin_int(false, [](const T& x, int k) {
        return stan::math::falling_factorial(x, k);
      });
    if (e.name == "rising_factorial")
      return bin_int(false, [](const T& x, int k) {
        return stan::math::rising_factorial(x, k);
      });
    if (e.name == "ldexp")
      return bin_int(false,
                     [](const T& x, int k) { return stan::math::ldexp(x, k); });
    // --O1 partial evaluation rewrites `x * log(y)` to lmultiply(x, y);
    // multiply_log computes exactly x * log(y), so the value is bitwise
    // what the unoptimized form produced.
    if (e.name == "lmultiply" || e.name == "multiply_log")
      return bin([](const T& x, const T& y) {
        return stan::math::multiply_log(x, y);
      });
    // fma from --O1 partial evaluation (`c + a*b`) or written explicitly:
    // fused like stan-math's, elementwise with scalar broadcast.
    if (e.name == "fma" && e.args.size() == 3) {
      Value a = eval(e.args[0]), b = eval(e.args[1]), c = eval(e.args[2]);
      Value o;
      size_t n = 1;
      bool have_container = false;
      for (const Value* x : {&a, &b, &c}) {
        if (is_scalar(*x)) continue;
        if (have_container && (x->dims != o.dims || x->r.size() != n))
          fail("fma: incompatible shapes", e.raw);
        n = x->r.size();
        o.dims = x->dims;
        have_container = true;
      }
      o.r.resize(n);
      for (size_t i = 0; i < n; ++i)
        o.r[i] = stan::math::fma(a.r[a.r.size() == 1 ? 0 : i],
                                 b.r[b.r.size() == 1 ? 0 : i],
                                 c.r[c.r.size() == 1 ? 0 : i]);
      return o;
    }
    // Stan's bound transforms, callable as functions rather than written on
    // a declaration: elementwise over every container shape, with each bound
    // either one value for the whole container or one value per element.
    // The interpreter serves transformed data and the interpreted
    // write_array, both of which the generated model instantiates with
    // `jacobian__ = false`, so the `_jacobian` direction is the constrained
    // value and nothing else -- there is no target here to increment.
    const size_t bound_arity = bound_transform_arity(e.name);
    if (bound_arity != 0 && bound_arity == e.args.size()) {
      std::vector<Value> a;
      a.reserve(e.args.size());
      for (const mir::Expr& arg : e.args) a.push_back(eval(arg));
      const auto at = [&](size_t k, size_t i) {
        return a[k].r[a[k].r.size() == 1 ? 0 : i];
      };
      Value o;
      o.dims = a[0].dims;
      o.r.resize(a[0].r.size());
      for (size_t i = 0; i < o.r.size(); ++i)
        o.r[i] = bound_transform(e.name, at(0, i), at(1, i),
                                 a.size() > 2 ? at(2, i) : T(0));
      return o;
    }
    // minus and plus are the named spellings of the unary operators, so
    // they are the same identity and the same negation over the same
    // shapes. The graph lowering knows them too; teaching only one side is
    // what let a vectorized log_sum_exp leave elements uninitialized here.
    if (e.name == "PMinus__" || (e.name == "minus" && e.args.size() == 1))
      return un([](const T& x) { return -x; });
    if (e.name == "PPlus__" || (e.name == "plus" && e.args.size() == 1))
      return un([](const T& x) { return x; });
    if (e.name == "exp")
      return un([](const T& x) { return stan::math::exp(x); });
    if (e.name == "log")
      return un([](const T& x) { return stan::math::log(x); });
    if (e.name == "sqrt")
      return un([](const T& x) { return stan::math::sqrt(x); });
    if (e.name == "square")
      return un([](const T& x) { return stan::math::square(x); });
    if (e.name == "inv_logit")
      return un([](const T& x) { return stan::math::inv_logit(x); });
    if (e.name == "logit")
      return un([](const T& x) { return stan::math::logit(x); });
    if (e.name == "log1m")
      return un([](const T& x) { return stan::math::log1m(x); });
    if (e.name == "tanh")
      return un([](const T& x) { return stan::math::tanh(x); });
    if (e.name == "cumulative_sum") {
      Value a = eval(e.args[0]);
      Value o;
      o.dims =
          a.dims.empty() ? std::vector<int64_t>{(int64_t)a.r.size()} : a.dims;
      T s = T(0.0);
      for (const T& x : a.r) {
        s += x;
        o.r.push_back(s);
      }
      return o;
    }
    if (e.name == "quad_form_diag" && e.args.size() == 2) {
      // diag(v) * M * diag(v): elementwise row and column scaling.
      Value m = eval(e.args[0]);
      Value v = eval(e.args[1]);
      if (m.dims.size() != 2) fail("quad_form_diag: needs a matrix", e.raw);
      const int64_t R = m.dims[0], C = m.dims[1];
      Value o;
      o.dims = {R, C};
      o.r.resize((size_t)(R * C));
      for (int64_t j = 0; j < C; ++j)
        for (int64_t i = 0; i < R; ++i)
          o.r[(size_t)(j * R + i)] = v.r.at((size_t)i) *
                                     m.r.at((size_t)(j * R + i)) *
                                     v.r.at((size_t)j);
      return o;
    }
    if (e.name == "gp_exp_quad_cov" && e.args.size() == 3) {
      // K(i,j) = alpha^2 exp(-|x_i - x_j|^2 / (2 rho^2)); x is array[N]
      // real or array[N] vector[D] in Fortran storage.
      Value x = eval(e.args[0]);
      const T alpha = eval(e.args[1]).r.at(0);
      const T rho = eval(e.args[2]).r.at(0);
      const int64_t N = x.dims.size() == 2 ? x.dims[0] : (int64_t)x.r.size();
      const int64_t D = x.dims.size() == 2 ? x.dims[1] : 1;
      const T a2 = alpha * alpha;
      const T inv2r2 = T(1.0) / (T(2.0) * rho * rho);
      Value o;
      o.dims = {N, N};
      o.r.resize((size_t)(N * N));
      for (int64_t j = 0; j < N; ++j)
        for (int64_t i = 0; i < N; ++i) {
          T sq = T(0.0);
          for (int64_t d = 0; d < D; ++d) {
            const T diff =
                x.r.at((size_t)(i + N * d)) - x.r.at((size_t)(j + N * d));
            sq += diff * diff;
          }
          o.r[(size_t)(j * N + i)] = a2 * stan::math::exp(-sq * inv2r2);
        }
      return o;
    }
    if (e.name == "fabs")
      return un([](const T& x) { return stan::math::fabs(x); });
    if (e.name == "mean") {
      Value a = eval(e.args[0]);
      T m = T(0.0);
      for (const T& v : a.r) m += v;
      r.r = {m / (double)a.r.size()};
      return r;
    }
    if (e.name == "sd") {
      Value a = eval(e.args[0]);
      T m = T(0.0);
      for (const T& v : a.r) m += v;
      m /= (double)a.r.size();
      T s2 = T(0.0);
      for (const T& v : a.r) s2 += (v - m) * (v - m);
      r.r = {stan::math::sqrt(s2 / (double)(a.r.size() - 1))};
      return r;
    }
    if (e.name == "sum") {
      Value a = eval(e.args[0]);
      T m = T(0.0);
      for (const T& v : a.r) m += v;
      r.r = {m};
      return r;
    }
    // The matrix slice family, all on col-major storage.
    if (e.name == "sub_col" && e.args.size() == 4) {
      Value m = eval(e.args[0]);
      if (m.dims.size() != 2) fail("sub_col: needs a matrix", e.raw);
      const long i = as_int(e.args[1]), j = as_int(e.args[2]),
                 n = as_int(e.args[3]);
      const int64_t R = m.dims[0];
      r.dims = {n};
      for (long k = 0; k < n; ++k)
        r.r.push_back(m.r.at((size_t)((j - 1) * R + (i - 1) + k)));
      return r;
    }
    if (e.name == "col" && e.args.size() == 2) {
      Value m = eval(e.args[0]);
      if (m.dims.size() != 2) fail("col: needs a matrix", e.raw);
      const long j = as_int(e.args[1]);
      const int64_t R = m.dims[0];
      r.dims = {R};
      r.r.assign(m.r.begin() + (j - 1) * R, m.r.begin() + j * R);
      return r;
    }
    if (e.name == "row" && e.args.size() == 2) {
      Value m = eval(e.args[0]);
      if (m.dims.size() != 2) fail("row: needs a matrix", e.raw);
      const long i = as_int(e.args[1]);
      const int64_t R = m.dims[0], C = m.dims[1];
      r.dims = {C};
      for (int64_t j = 0; j < C; ++j)
        r.r.push_back(m.r.at((size_t)(j * R + (i - 1))));
      return r;
    }
    if (e.name == "segment" && e.args.size() == 3) {
      Value a = eval(e.args[0]);
      const long from = as_int(e.args[1]), cnt = as_int(e.args[2]);
      r.is_int = a.is_int;
      r.dims = {cnt};
      for (long k = 0; k < cnt; ++k) {
        r.r.push_back(a.r.at((size_t)(from - 1 + k)));
        if (a.is_int) r.i.push_back(a.i.at((size_t)(from - 1 + k)));
      }
      return r;
    }
    if ((e.name == "head" || e.name == "tail") && e.args.size() == 2) {
      Value a = eval(e.args[0]);
      const long n = as_int(e.args[1]);
      const long off = e.name == "head" ? 0 : (long)a.r.size() - n;
      r.is_int = a.is_int;
      r.dims = {n};
      for (long k = 0; k < n; ++k) {
        r.r.push_back(a.r.at((size_t)(off + k)));
        if (a.is_int) r.i.push_back(a.i.at((size_t)(off + k)));
      }
      return r;
    }
    // squared_distance is dot_self of the difference, which is how the
    // graph lowers it too (lower.cpp); the two spellings agreeing keeps
    // transformed data and the log density on the same summation order.
    // A vector may be paired with a row_vector, and neither view carries
    // anything but a length, so there is nothing to reorder. No
    // broadcasting: the language has no scalar-against-container overload.
    if (e.name == "squared_distance" && e.args.size() == 2) {
      Value a = eval(e.args[0]), b = eval(e.args[1]);
      if (a.r.size() != b.r.size())
        fail("squared_distance: length mismatch", e.raw);
      T s = T(0.0);
      for (size_t i = 0; i < a.r.size(); ++i) {
        const T d = a.r[i] - b.r[i];
        s += d * d;
      }
      r.r = {s};
      return r;
    }
    if ((e.name == "dot_product" || e.name == "dot_self")) {
      Value a = eval(e.args[0]);
      Value b = e.name == "dot_self" ? a : eval(e.args[1]);
      if (a.r.size() != b.r.size()) fail("dot_product: length mismatch", e.raw);
      T s = T(0.0);
      for (size_t i = 0; i < a.r.size(); ++i) s += a.r[i] * b.r[i];
      r.r = {s};
      return r;
    }
    if (e.name == "cholesky_decompose" && e.args.size() == 1) {
      // Standard column-oriented Cholesky on the templated scalar.
      Value a = eval(e.args[0]);
      if (a.dims.size() != 2 || a.dims[0] != a.dims[1])
        fail("cholesky_decompose: needs a square matrix", e.raw);
      const int64_t K = a.dims[0];
      Value o;
      o.dims = {K, K};
      o.r.assign((size_t)(K * K), T(0.0));
      for (int64_t j = 0; j < K; ++j) {
        T d = a.r.at((size_t)(j * K + j));
        for (int64_t k = 0; k < j; ++k) {
          const T& l = o.r[(size_t)(k * K + j)];
          d -= l * l;
        }
        if (!(val(d) > 0.0))
          fail("cholesky_decompose: matrix is not positive definite", e.raw);
        const T dj = stan::math::sqrt(d);
        o.r[(size_t)(j * K + j)] = dj;
        for (int64_t i = j + 1; i < K; ++i) {
          T s = a.r.at((size_t)(j * K + i));
          for (int64_t k = 0; k < j; ++k)
            s -= o.r[(size_t)(k * K + i)] * o.r[(size_t)(k * K + j)];
          o.r[(size_t)(j * K + i)] = s / dj;
        }
      }
      return o;
    }
    if (e.name == "prod") {
      Value a = eval(e.args[0]);
      T m = T(1.0);
      for (const T& v : a.r) m *= v;
      r.r = {m};
      return r;
    }
    // Two arguments is the elementwise form, which Stan vectorizes over
    // every container shape with scalar broadcast, so it belongs with the
    // binaries above; only one-argument log_sum_exp is a reduction, and
    // log_diff_exp has no reduction form at all.
    if (e.name == "log_sum_exp" && e.args.size() == 2)
      return bin(
          [](const T& x, const T& y) { return stan::math::log_sum_exp(x, y); });
    if (e.name == "log_diff_exp" && e.args.size() == 2)
      return bin([](const T& x, const T& y) {
        return stan::math::log_diff_exp(x, y);
      });
    if (e.name == "log_sum_exp") {
      Value a = eval(e.args[0]);
      r.r = {lse(a.r)};
      return r;
    }
    if (e.name == "softmax" || e.name == "log_softmax") {
      Value a = eval(e.args[0]);
      const T m = lse(a.r);
      Value o;
      o.dims =
          a.dims.empty() ? std::vector<int64_t>{(int64_t)a.r.size()} : a.dims;
      for (const T& x : a.r)
        o.r.push_back(e.name == "softmax" ? stan::math::exp(x - m) : x - m);
      return o;
    }
    if ((e.name == "diag_pre_multiply" || e.name == "diag_post_multiply") &&
        e.args.size() == 2) {
      const bool pre = e.name.find("_pre_") != std::string::npos;
      Value v = eval(e.args[pre ? 0 : 1]);
      Value m = eval(e.args[pre ? 1 : 0]);
      if (m.dims.size() != 2) fail(e.name + ": needs a matrix argument", e.raw);
      const int64_t R = m.dims[0], C = m.dims[1];
      Value o;
      o.dims = {R, C};
      o.r.resize((size_t)(R * C));
      for (int64_t j = 0; j < C; ++j)
        for (int64_t i = 0; i < R; ++i)
          o.r[(size_t)(j * R + i)] =
              m.r.at((size_t)(j * R + i)) * v.r.at((size_t)(pre ? i : j));
      return o;
    }
    if (e.name == "rep_vector" || e.name == "rep_row_vector") {
      Value a = eval(e.args[0]);
      const long n = as_int(e.args[1]);
      r.r.assign(n, a.r.at(0));
      r.dims = {n};
      return r;
    }
    if (e.name == "Equals__")
      return cmp([](double x, double y) { return x == y; });
    if (e.name == "NEquals__")
      return cmp([](double x, double y) { return x != y; });
    if (e.name == "Greater__")
      return cmp([](double x, double y) { return x > y; });
    if (e.name == "Geq__")
      return cmp([](double x, double y) { return x >= y; });
    if (e.name == "Less__")
      return cmp([](double x, double y) { return x < y; });
    if (e.name == "Leq__")
      return cmp([](double x, double y) { return x <= y; });
    if (e.name == "PNot__")
      return un([](const T& x) { return T(val(x) == 0.0 ? 1.0 : 0.0); });
    if (e.name == "max" || e.name == "min") {
      // std::max/min semantics on values, not fmax/fmin: NaN handling and
      // tie selection must not change under the template.
      const auto vmax = [](const T& x, const T& y) {
        return val(x) < val(y) ? y : x;
      };
      const auto vmin = [](const T& x, const T& y) {
        return val(y) < val(x) ? y : x;
      };
      Value a = eval(e.args[0]);
      if (e.args.size() == 2) {
        Value b = eval(e.args[1]);
        const T m = e.name == "max" ? vmax(a.r.at(0), b.r.at(0))
                                    : vmin(a.r.at(0), b.r.at(0));
        r.is_int = a.is_int && b.is_int;
        if (r.is_int) r.i = {(int)val(m)};
        r.r = {m};
        return r;
      }
      T m = a.r.at(0);
      for (const T& x : a.r) m = e.name == "max" ? vmax(m, x) : vmin(m, x);
      r.is_int = a.is_int;
      if (r.is_int) r.i = {(int)val(m)};
      r.r = {m};
      return r;
    }
    if (e.name == "FnMakeArray" || e.name == "FnMakeRowVec") {
      Value o;
      o.is_int = true;
      bool rows_mode = false;
      int64_t row_len = 0;
      std::vector<int64_t> elem_dims;  // shape of one element, once known
      for (const auto& a : e.args) {
        Value v2 = eval(a);
        if (v2.r.size() > 1 || rows_mode) {
          // Row-vector elements: build a matrix, row-major.
          rows_mode = true;
          row_len = (int64_t)v2.r.size();
          elem_dims = v2.dims;
          o.is_int = false;
          o.r.insert(o.r.end(), v2.r.begin(), v2.r.end());
          continue;
        }
        o.r.push_back(v2.r.at(0));
        if (v2.is_int && !v2.i.empty())
          o.i.push_back(v2.i[0]);
        else
          o.is_int = false;
      }
      if (!o.is_int) o.i.clear();
      if (rows_mode) {
        // Rows arrived row-by-row; store column-major. Each element is
        // already first-index-fastest in itself, so sending its cell j to
        // j*R+i makes the whole result first-index-fastest over {R} ++ the
        // element's extents, at any rank.
        const int64_t R = (int64_t)e.args.size(), C = row_len;
        std::vector<T> cm(R * C);
        for (int64_t i = 0; i < R; ++i)
          for (int64_t j = 0; j < C; ++j) cm[j * R + i] = o.r[i * C + j];
        o.r = std::move(cm);
      }
      if (rows_mode) {
        // Keep the element's own extents instead of collapsing them into
        // one. The placement above does not pin them down -- Fortran order
        // over {2,4} is byte-for-byte Fortran order over {2,2,2} -- but
        // graph_order permutes by exactly these extents on the way to a
        // slot, and eval_indexed strides by them, so a collapsed shape
        // silently transposes the trailing two axes of every array literal
        // of rank 3 or deeper.
        // The extents still have to multiply out to the storage, because
        // graph_order walks them to place every cell; one axis is the
        // honest answer for anything that cannot say more.
        o.dims = {(int64_t)e.args.size()};
        int64_t elem_n = 1;
        for (int64_t d : elem_dims) elem_n *= d;
        if (elem_dims.empty() || elem_n != row_len)
          o.dims.push_back(row_len);
        else
          o.dims.insert(o.dims.end(), elem_dims.begin(), elem_dims.end());
      } else {
        o.dims = {(int64_t)o.r.size()};
      }
      return o;
    }
    if (e.name == "Transpose__") {
      Value a = eval(e.args[0]);
      if (a.dims.size() < 2) return a;  // vector transpose: same storage
      Value o;
      o.dims = {a.dims[1], a.dims[0]};
      o.r.resize(a.r.size());
      // col-major: o(j,i) = a(i,j)
      for (int64_t i = 0; i < a.dims[0]; ++i)
        for (int64_t j = 0; j < a.dims[1]; ++j)
          o.r[i * a.dims[1] + j] = a.r[j * a.dims[0] + i];
      return o;
    }
    if (e.name == "to_vector" || e.name == "to_row_vector") {
      Value a = eval(e.args[0]);
      a.dims = {(int64_t)a.r.size()};
      a.is_int = false;
      return a;
    }
    if (e.name == "to_array_1d") {
      // Flattening is the identity on this storage.
      Value a = eval(e.args[0]);
      a.dims = {(int64_t)std::max(a.r.size(), a.i.size())};
      return a;
    }
    // FnLength is the compiler-internal stanc3 emits for the observation
    // count in a vectorized `T[,]` normalizer. Its backend spelling is
    // stan::math::size, which counts every element, so it answers as
    // num_elements does rather than as size does.
    if (e.name == "rows" || e.name == "cols" || e.name == "size" ||
        e.name == "num_elements" || e.name == "FnLength") {
      Value a = eval(e.args[0]);
      long v = 0;
      if (e.name == "rows")
        v = a.dims.size() == 2 ? a.dims[0] : (long)a.r.size();
      else if (e.name == "cols")
        v = a.dims.size() == 2 ? a.dims[1] : 1;
      else
        v = a.dims.empty() ? (long)std::max(a.r.size(), a.i.size())
                           : (long)a.dims[0];
      if (e.name == "num_elements" || e.name == "FnLength")
        v = (long)std::max(a.r.size(), a.i.size());
      r.is_int = true;
      r.i = {(int)v};
      r.r = {T((double)v)};
      return r;
    }
    if (e.name == "dims" && e.args.size() == 1) {
      Value a = eval(e.args[0]);
      r.is_int = true;
      std::vector<int64_t> ds = a.dims;
      if (ds.empty()) ds = {(int64_t)std::max(a.r.size(), a.i.size())};
      r.dims = {(int64_t)ds.size()};
      for (int64_t d : ds) {
        r.i.push_back((int)d);
        r.r.push_back(T((double)d));
      }
      return r;
    }
    if (e.name == "pi" && e.args.empty()) {
      r.r = {T(stan::math::pi())};
      return r;
    }
    if (e.name == "e" && e.args.empty()) {
      r.r = {T(stan::math::e())};
      return r;
    }
    if (e.name == "machine_precision" && e.args.empty()) {
      r.r = {T(std::numeric_limits<double>::epsilon())};
      return r;
    }
    if (e.name == "negative_infinity") {
      r.r = {T(-std::numeric_limits<double>::infinity())};
      return r;
    }
    if (e.name == "positive_infinity") {
      r.r = {T(std::numeric_limits<double>::infinity())};
      return r;
    }
    if (e.name == "student_t_lccdf" && e.args.size() == 4) {
      r.r = {stan::math::student_t_lccdf(
          eval(e.args[0]).r.at(0), eval(e.args[1]).r.at(0),
          eval(e.args[2]).r.at(0), eval(e.args[3]).r.at(0))};
      return r;
    }
    // Densities as plain functions (generated quantities use them freely:
    // the hmm Viterbi recursions, log-likelihood columns). Elementwise
    // with scalar broadcasting, summed, which is stan-math's vectorized
    // semantics.
    // Scalar unaries from the shared list, so transformed data and
    // generated quantities accept exactly what the log-density path does.
    // Evaluate the table's value formula on doubles on every route. An ODE
    // fallback instantiates this on var; one arena callback node applies the
    // same ordered pullback without selecting a second implementation. Keep
    // `x`, `y`, and the callback's `seed` visible to that shared expression.
#define STANLI_INTERP_UNARY(code, ufn, VAL, DELTA, TOPOLOGY)              \
  if (e.name == #ufn && e.args.size() == 1) {                             \
    return un([](const T& arg) {                                          \
      const double x = val(arg);                                          \
      const double y = VAL;                                               \
      if constexpr (std::is_same_v<T, double>) {                          \
        return y;                                                         \
      } else {                                                            \
        if (!unary_has_pullback(TOPOLOGY, x)) return T(y);                \
        return stan::math::make_callback_var(y, [arg](auto& vi) mutable { \
          const double x = stan::math::value_of(arg);                     \
          const double y = vi.val();                                      \
          const double seed = vi.adj();                                   \
          arg.adj() += (DELTA);                                           \
        });                                                               \
      }                                                                   \
    });                                                                   \
  }
    STANLI_SCALAR_UNARY_LIST(STANLI_INTERP_UNARY)
#undef STANLI_INTERP_UNARY

    // The two unaries the shared list cannot generate. std_normal_qf is
    // stanc3's alias for inv_Phi (Lower_expr.ml maps it onto
    // stan::math::inv_Phi), and trigamma's derivative is the derivative of
    // AS121's recurrence rather than a formula, so both take Math's own
    // overload: on doubles that is the prim call, on var it is the tape the
    // graph kernel replays.
    if (e.name == "std_normal_qf" && e.args.size() == 1)
      return un([](const T& x) { return stan::math::inv_Phi(x); });
    if (e.name == "trigamma" && e.args.size() == 1)
      return un([](const T& x) { return stan::math::trigamma(x); });

    // Categorical arguments are containers as a whole, not elementwise
    // broadcasts. Preserve scalar-vs-array outcome overloads and the
    // caller's propto instantiation exactly as the graph kernel does.
    if ((e.name == "categorical_lpmf" || e.name == "categorical_logit_lpmf") &&
        e.args.size() == 2) {
      Value outcome = eval(e.args[0]);
      Value value = eval(e.args[1]);
      std::vector<int> outcomes = outcome.i;
      if (outcomes.empty() && !outcome.r.empty()) {
        outcomes.reserve(outcome.r.size());
        for (const T& x : outcome.r) {
          const double v = val(x);
          if (!std::isfinite(v) || std::trunc(v) != v ||
              v < std::numeric_limits<int>::min() ||
              v > std::numeric_limits<int>::max())
            fail("malformed integer categorical outcome", e.raw);
          outcomes.push_back(static_cast<int>(v));
        }
      }
      const bool scalar = e.args[0].unsized.depth == 0;
      if (scalar && outcomes.size() != 1)
        fail("categorical scalar outcome has wrong width", e.raw);
      if (value.dims.size() != 1)
        fail("categorical probability argument is not a vector", e.raw);
      Eigen::Matrix<T, Eigen::Dynamic, 1> arg(value.r.size());
      for (size_t k = 0; k < value.r.size(); ++k)
        arg((Eigen::Index)k) = value.r[k];
      const bool propto = e.fn_propto && propto_ctx_;
      const auto density = [&](const auto& n) -> T {
        if (e.name == "categorical_logit_lpmf")
          return propto ? stan::math::categorical_logit_lpmf<true>(n, arg)
                        : stan::math::categorical_logit_lpmf<false>(n, arg);
        return propto ? stan::math::categorical_lpmf<true>(n, arg)
                      : stan::math::categorical_lpmf<false>(n, arg);
      };
      r.r = {scalar ? density(outcomes[0]) : density(outcomes)};
      return r;
    }

    // The continuous scalar ones come from the shared list, so this can
    // never be narrower than what the register-machine compiler accepts
    // -- the compiled path falls back here when compilation fails, and a
    // narrower fallback turns a slow path into an error. The discrete
    // ones are the interpreter's alone: the register file has nowhere to
    // put an integer outcome.
    const int shared_id = program_density_id_by_name(e.name);
    if (shared_id >= 0 || e.name == "bernoulli_lpmf" ||
        e.name == "binomial_lpmf" || e.name == "poisson_lpmf" ||
        e.name == "poisson_log_lpmf" || e.name == "bernoulli_logit_lpmf" ||
        e.name == "binomial_logit_lpmf" || e.name == "hypergeometric_lpmf" ||
        e.name == "discrete_range_lpmf") {
      std::vector<Value> av;
      for (const auto& a : e.args) av.push_back(eval(a));
      size_t n = 1;
      for (const auto& a : av) n = std::max(n, a.r.size());
      const auto sc = [&](size_t k, size_t i) -> const T& {
        const auto& rr = av[k].r;
        return rr.size() == 1 ? rr[0] : rr.at(i);
      };
      const auto ic = [&](size_t k, size_t i) -> int {
        const auto& a = av[k];
        if (!a.i.empty()) return a.i.size() == 1 ? a.i[0] : a.i.at(i);
        return (int)val(sc(k, i));
      };
      T acc = T(0.0);
      for (size_t i = 0; i < n; ++i) {
        // The continuous ones go through the shared dispatch, so this
        // cannot be narrower than what the register machine accepts and
        // costs one instantiation of 27 densities rather than one per
        // translation unit that interprets MIR.
        if (shared_id >= 0) {
          T argbuf[kMaxDensityArgs];
          const int arity = program_density_arity(shared_id);
          for (int k = 0; k < arity; ++k) argbuf[k] = sc((size_t)k, i);
          acc += program_density<T>(shared_id, argbuf);
          continue;
        }
        if (e.name == "bernoulli_lpmf")
          acc += stan::math::bernoulli_lpmf(ic(0, i), sc(1, i));
        else if (e.name == "bernoulli_logit_lpmf")
          acc += stan::math::bernoulli_logit_lpmf(ic(0, i), sc(1, i));
        else if (e.name == "binomial_lpmf")
          acc += stan::math::binomial_lpmf(ic(0, i), ic(1, i), sc(2, i));
        else if (e.name == "binomial_logit_lpmf")
          acc += stan::math::binomial_logit_lpmf(ic(0, i), ic(1, i), sc(2, i));
        else if (e.name == "poisson_lpmf")
          acc += stan::math::poisson_lpmf(ic(0, i), sc(1, i));
        else if (e.name == "poisson_log_lpmf")
          acc += stan::math::poisson_log_lpmf(ic(0, i), sc(1, i));
        else if (e.name == "hypergeometric_lpmf")
          acc += stan::math::hypergeometric_lpmf(ic(0, i), ic(1, i), ic(2, i),
                                                 ic(3, i));
        else if (e.name == "discrete_range_lpmf")
          acc += stan::math::discrete_range_lpmf(ic(0, i), ic(1, i), ic(2, i));
        else if (e.name == "student_t_lpdf")
          acc += stan::math::student_t_lpdf(sc(0, i), sc(1, i), sc(2, i),
                                            sc(3, i));
        else
          fail("unsupported density " + e.name, e.raw);
      }
      r.r = {acc};
      return r;
    }
    if (e.name == "rep_array" && e.args.size() == 2) {
      Value v = eval(e.args[0]);
      const long n = as_int(e.args[1]);
      r.is_int = v.is_int;
      r.dims = {n};
      r.r.assign(n, v.r.at(0));
      if (v.is_int) r.i.assign(n, v.i.at(0));
      return r;
    }
    if (e.name == "rep_matrix" && e.args.size() == 3) {
      Value v = eval(e.args[0]);
      const long R = as_int(e.args[1]), C = as_int(e.args[2]);
      r.dims = {R, C};
      r.r.assign(R * C, v.r.at(0));
      return r;
    }
    if (e.name == "rep_matrix" && e.args.size() == 2) {
      // rep_matrix(vector, C) tiles columns; rep_matrix(row_vector, R)
      // tiles rows. Col-major storage either way.
      Value v = eval(e.args[0]);
      const long n = as_int(e.args[1]);
      const bool rowvec = e.args[0].type_ == "URowVector";
      const int64_t len = (int64_t)v.r.size();
      if (rowvec) {
        r.dims = {n, len};
        for (int64_t j = 0; j < len; ++j)
          for (int64_t i = 0; i < n; ++i) r.r.push_back(v.r[(size_t)j]);
      } else {
        r.dims = {len, n};
        for (int64_t j = 0; j < n; ++j)
          r.r.insert(r.r.end(), v.r.begin(), v.r.end());
      }
      return r;
    }
    if (e.name == "append_row" && e.args.size() == 2) {
      Value a = eval(e.args[0]), b = eval(e.args[1]);
      if (a.dims.size() <= 1 && b.dims.size() <= 1) {
        // Vectors/scalars: vertical concatenation.
        r.dims = {(int64_t)(a.r.size() + b.r.size())};
        r.r = a.r;
        r.r.insert(r.r.end(), b.r.begin(), b.r.end());
        return r;
      }
      if (a.dims.size() == 2 && b.dims.size() == 2 && a.dims[1] == b.dims[1]) {
        // Matrices: stack rows, col-major storage interleaves columns.
        const int64_t Ra = a.dims[0], Rb = b.dims[0], C = a.dims[1];
        r.dims = {Ra + Rb, C};
        r.r.reserve((Ra + Rb) * C);
        for (int64_t j = 0; j < C; ++j) {
          r.r.insert(r.r.end(), a.r.begin() + j * Ra,
                     a.r.begin() + (j + 1) * Ra);
          r.r.insert(r.r.end(), b.r.begin() + j * Rb,
                     b.r.begin() + (j + 1) * Rb);
        }
        return r;
      }
      fail("append_row shape mismatch", e.raw);
    }
    if (e.name == "append_col" && e.args.size() == 2) {
      Value a = eval(e.args[0]), b = eval(e.args[1]);
      // Column-major storage makes column appends a concatenation.
      // A vector argument is a one-column block.
      const int64_t Ra = a.dims.size() == 2 ? a.dims[0] : (int64_t)a.r.size();
      const int64_t Rb = b.dims.size() == 2 ? b.dims[0] : (int64_t)b.r.size();
      const int64_t Ca = a.dims.size() == 2 ? a.dims[1] : 1;
      const int64_t Cb = b.dims.size() == 2 ? b.dims[1] : 1;
      if (Ra != Rb) fail("append_col row mismatch", e.raw);
      r.dims = {Ra, Ca + Cb};
      r.r = a.r;
      r.r.insert(r.r.end(), b.r.begin(), b.r.end());
      r.is_int = false;
      return r;
    }
    if constexpr (std::is_same_v<T, double>) {
      if (hooks_.fun && hooks_.fun(e, &r)) return r;
    }
    fail("unsupported function " + e.name, e.raw);
  }
};

}  // namespace stanli

#endif

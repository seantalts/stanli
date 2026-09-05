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
#include <stanli/container_shape.hpp>
#include <stanli/data.hpp>
#include <stanli/density_registry.hpp>
#include <stanli/builtin_registry.hpp>
#include <stanli/function_registry.hpp>
#include <stanli/extrema_grouping.hpp>
#include <stanli/mir_message.hpp>
#include <stanli/mir.hpp>
#include <stanli/optable.hpp>
#include <stanli/program.hpp>
#include <stanli/structured_check.hpp>

#include <stan/math.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
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

  // A host may bind a typed value directly and skip stanc's generated input
  // declaration/rebuild statements. Keep the declaration-owned geometry
  // that FnCheck needs separately: an empty JSON array cannot encode trailing
  // extents such as array[0] vector[3].
  void set_declared_dims(const std::string& name, std::vector<int64_t> dims) {
    decl_dims_[name] = std::move(dims);
  }

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

  // Bind arguments positionally by their declared logical views and evaluate
  // the body to its return value. This is the ODE entry point: real-typed
  // parameters consume `args` in order, int-typed ones consume `int_args`.
  std::vector<T> call(const mir::FunDef& f,
                      const std::vector<std::vector<T>>& args,
                      const std::vector<std::vector<int>>& int_args) {
    if (udf_depth_ > 64) fail("UDF recursion too deep");
    MirInterp sub(funs_, where_, hooks_);
    // Positional calls are used by the ODE/RHS fallback, but their formals
    // have the same unknown Eigen-view provenance as an ordinary UDF's.
    // Keeping them below top-level depth prevents product lowering from
    // assuming a formal is an owning, address-zero vector.
    sub.udf_depth_ = udf_depth_ + 1;
    sub.propto_ctx_ = propto_ctx_;
    if (f.arg_views.size() != f.arg_names.size())
      fail("function has incomplete unsized argument metadata: " + f.name);
    size_t ai = 0, ii = 0;
    for (size_t k = 0; k < f.arg_names.size(); ++k) {
      const mir::UnsizedView& view = f.arg_views[k];
      const bool is_int = view.leaf == mir::UnsizedLeaf::Int;
      Value v;
      if (is_int && ii < int_args.size()) {
        v.is_int = true;
        v.i = int_args[ii++];
        v.r.assign(v.i.begin(), v.i.end());
        if (view.depth != 0) v.dims = {(int64_t)v.i.size()};
      } else if (ai < args.size()) {
        v.r = args[ai++];
        if (view.depth != 0 || view.leaf != mir::UnsizedLeaf::Real)
          v.dims = {(int64_t)v.r.size()};
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

  // Bind already-typed values positionally and preserve the complete return
  // value, including integer identity and logical dimensions.  Function's
  // public C++ entry point uses this path: DataMap::Entry is Value itself for
  // the double instantiation, so no host/container conversion is needed.
  Value call(const mir::FunDef& f, const std::vector<Value>& args) {
    if (udf_depth_ > 64) fail("UDF recursion too deep");
    if (f.arg_names.size() != args.size())
      fail("function argument count mismatch: " + f.name);
    MirInterp sub(funs_, where_, hooks_);
    sub.udf_depth_ = udf_depth_ + 1;
    sub.propto_ctx_ = propto_ctx_;
    for (size_t k = 0; k < args.size(); ++k) {
      sub.env_[f.arg_names[k]] = args[k];
      sub.set_declared_dims(f.arg_names[k], args[k].dims);
    }
    try {
      for (const auto& s : f.body) sub.exec(s);
    } catch (ReturnV& r) {
      return std::move(r.v);
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
          if (e.is_int) e.i = {std::numeric_limits<int>::min()};
          e.r = {e.is_int
                     ? T(static_cast<double>(std::numeric_limits<int>::min()))
                     : T(std::numeric_limits<double>::quiet_NaN())};
        } else if (!st.decl_type.base.empty()) {
          // Bare sized decl: allocate so element writes work; real elements
          // are NaN until written (see the scalar case above).
          int64_t n = 1;
          for (int64_t d : declared_dims) n *= d;
          e.r.assign(
              n, e.is_int
                     ? T(static_cast<double>(std::numeric_limits<int>::min()))
                     : T(std::numeric_limits<double>::quiet_NaN()));
          if (e.is_int) e.i.assign(n, std::numeric_limits<int>::min());
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
          Value v = eval(st.rhs);
          // A declared numeric type stays fixed for its whole lifetime. The
          // RHS's own representation can differ: MirInterp's binary ops use
          // real registers, while an all-integer array literal is tagged int.
          // Preserve int destinations as before, and apply Stan's implicit
          // int-to-real promotion when assigning into a real destination.
          const Value* existing = find(st.lhs);
          if (existing && existing->is_int && !v.is_int) {
            v.is_int = true;
            v.i.clear();
            for (const T& x : v.r) v.i.push_back((int)val(x));
          } else if (existing && !existing->is_int && v.is_int) {
            v.is_int = false;
            v.i.clear();
          }
          env_[st.lhs] = std::move(v);
          return;
        }
        Value* en = find(st.lhs);
        if (!en) fail("assignment to unknown " + st.lhs);
        Value v = eval(st.rhs);
        // A lone All replaces the complete container. Retain the declared
        // geometry and integer specialization owned by the destination:
        // replacing the Value object wholesale would let malformed MIR
        // resize or retag the declaration. The two-index matrix column form
        // remains on its existing path below.
        if (st.lhs_idx.size() == 1 && st.lhs_idx[0].name == "IndexAll") {
          if (en->dims.empty())
            fail("full-span assignment needs a container", st.raw);
          if (v.r.size() != en->r.size())
            fail("full-span assignment size mismatch", st.raw);
          if (v.dims != en->dims)
            fail("full-span assignment shape mismatch", st.raw);
          const bool target_is_int = en->is_int;
          en->r = std::move(v.r);
          en->i.clear();
          en->is_int = target_is_int;
          if (target_is_int) {
            en->i.reserve(en->r.size());
            if (v.is_int && v.i.size() == en->r.size()) {
              en->i = std::move(v.i);
            } else {
              for (const T& value : en->r)
                en->i.push_back(static_cast<int>(val(value)));
            }
          }
          return;
        }
        // Fix one or more leading dimensions of an N-D array and replace
        // the complete remaining container. For array[3] matrix[2,4] x,
        // x[i,j] is a row_vector[4] (implicit rest); x[i, :, :] spells the
        // same write with an explicit `:` for every remaining dimension.
        // First-index-fast storage makes those trailing values a strided
        // sequence at offset + prefix_stride*k.
        if (en->dims.size() > 2 && !st.lhs_idx.empty()) {
          size_t prefix_len = 0;
          while (prefix_len < st.lhs_idx.size() &&
                 st.lhs_idx[prefix_len].name == "IndexSingle")
            ++prefix_len;
          bool trailing_all = true;
          for (size_t d = prefix_len; d < st.lhs_idx.size(); ++d)
            if (st.lhs_idx[d].name != "IndexAll") trailing_all = false;
          if (prefix_len > 0 && trailing_all && prefix_len < en->dims.size() &&
              (st.lhs_idx.size() == prefix_len ||
               st.lhs_idx.size() == en->dims.size())) {
            int64_t offset = 0, prefix_stride = 1;
            for (size_t d = 0; d < prefix_len; ++d) {
              const long i = as_int(st.lhs_idx[d].args[0]);
              if (i < 1 || i > en->dims[d])
                fail("indexed assignment index out of bounds", st.raw);
              offset += (i - 1) * prefix_stride;
              prefix_stride *= en->dims[d];
            }
            int64_t rest = 1;
            for (size_t d = prefix_len; d < en->dims.size(); ++d)
              rest *= en->dims[d];
            if (static_cast<int64_t>(v.r.size()) != rest)
              fail("indexed assignment size mismatch", st.raw);
            for (int64_t k = 0; k < rest; ++k) {
              const size_t dst =
                  static_cast<size_t>(offset + prefix_stride * k);
              en->r.at(dst) = v.r.at(static_cast<size_t>(k));
              if (en->is_int)
                en->i.at(dst) =
                    v.is_int && static_cast<size_t>(k) < v.i.size()
                        ? v.i[static_cast<size_t>(k)]
                        : static_cast<int>(val(v.r.at(static_cast<size_t>(k))));
            }
            return;
          }
        }
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
        // Contiguous subrange write into a 1-D value: x[a:b] = rhs.
        if (st.lhs_idx.size() == 1 &&
            (st.lhs_idx[0].name == "IndexBetween" ||
             st.lhs_idx[0].name == "IndexUpfrom") &&
            en->dims.size() == 1) {
          const long a = as_int(st.lhs_idx[0].args[0]);
          const long b = st.lhs_idx[0].name == "IndexBetween"
                             ? as_int(st.lhs_idx[0].args[1])
                             : en->dims[0];
          const int64_t n = b >= a ? b - a + 1 : 0;
          if (n > 0 && (a < 1 || b > en->dims[0] || b > (long)en->r.size()))
            fail("range assignment index out of bounds", st.raw);
          if ((int64_t)v.r.size() != n)
            fail("range assignment size mismatch", st.raw);
          for (int64_t k = 0; k < n; ++k) {
            const size_t dst = static_cast<size_t>(a - 1 + k);
            en->r.at(dst) = v.r.at(static_cast<size_t>(k));
            if (en->is_int)
              en->i.at(dst) =
                  v.is_int && static_cast<size_t>(k) < v.i.size()
                      ? v.i[static_cast<size_t>(k)]
                      : static_cast<int>(val(v.r.at(static_cast<size_t>(k))));
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
        // Explicit row write X[i, :] = row_vector / array.
        if (st.lhs_idx.size() == 2 && st.lhs_idx[0].name == "IndexSingle" &&
            st.lhs_idx[1].name == "IndexAll" && en->dims.size() == 2) {
          const long i = as_int(st.lhs_idx[0].args[0]);
          const int64_t R = en->dims[0], C = en->dims[1];
          if (i < 1 || i > R)
            fail("matrix row assignment index out of bounds", st.raw);
          if ((int64_t)v.r.size() != C)
            fail("matrix row assignment size mismatch", st.raw);
          for (int64_t j = 0; j < C; ++j) {
            const size_t dst = (size_t)(j * R + i - 1);
            en->r.at(dst) = v.r.at((size_t)j);
            if (en->is_int)
              en->i.at(dst) = v.is_int && (size_t)j < v.i.size()
                                  ? v.i[(size_t)j]
                                  : (int)val(v.r.at((size_t)j));
          }
          return;
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
          const int64_t n = b >= a ? b - a + 1 : 0;
          if (j < 1 || j > en->dims[1] || (n > 0 && (a < 1 || b > R)))
            fail("matrix range assignment index out of bounds", st.raw);
          if ((int64_t)v.r.size() != n)
            fail("matrix range assignment size mismatch", st.raw);
          for (int64_t k = 0; k < n; ++k) {
            const size_t dst = static_cast<size_t>((j - 1) * R + (a - 1) + k);
            en->r.at(dst) = v.r.at(static_cast<size_t>(k));
            if (en->is_int)
              en->i.at(dst) =
                  v.is_int && static_cast<size_t>(k) < v.i.size()
                      ? v.i[static_cast<size_t>(k)]
                      : static_cast<int>(val(v.r.at(static_cast<size_t>(k))));
          }
          return;
        }
        // Row-segment write X[i, a:b] = row_vector / array.  The selected
        // elements are strided in first-index-fast storage.
        if (st.lhs_idx.size() == 2 && st.lhs_idx[0].name == "IndexSingle" &&
            st.lhs_idx[1].name == "IndexBetween" && en->dims.size() == 2) {
          const long i = as_int(st.lhs_idx[0].args[0]);
          const long a = as_int(st.lhs_idx[1].args[0]);
          const long b = as_int(st.lhs_idx[1].args[1]);
          const int64_t R = en->dims[0];
          const int64_t n = b >= a ? b - a + 1 : 0;
          if (i < 1 || i > R || (n > 0 && (a < 1 || b > en->dims[1])))
            fail("matrix range assignment index out of bounds", st.raw);
          if ((int64_t)v.r.size() != n)
            fail("matrix range assignment size mismatch", st.raw);
          for (int64_t k = 0; k < n; ++k) {
            const size_t dst = static_cast<size_t>((a - 1 + k) * R + (i - 1));
            en->r.at(dst) = v.r.at(static_cast<size_t>(k));
            if (en->is_int)
              en->i.at(dst) =
                  v.is_int && static_cast<size_t>(k) < v.i.size()
                      ? v.i[static_cast<size_t>(k)]
                      : static_cast<int>(val(v.r.at(static_cast<size_t>(k))));
          }
          return;
        }
        // General mixed-selection write through the shared index geometry:
        // any static combination of Single, All, Between, Upfrom, and Multi
        // selectors. The map enumerates destination cells in the
        // interpreter's first-index-fast storage, matching eval_indexed and
        // the RHS's logical order, so repeated indices deliberately retain
        // Stan's last-write-wins behavior. Validate the complete selection
        // before mutating the destination so a malformed index remains
        // atomic.
        if (!en->dims.empty() && st.lhs_idx.size() <= en->dims.size()) {
          std::vector<std::vector<int64_t>> selected(st.lhs_idx.size());
          std::vector<bool> drops(st.lhs_idx.size(), false);
          bool supported = true;
          for (size_t d = 0; supported && d < st.lhs_idx.size(); ++d) {
            const int64_t extent = en->dims[d];
            const mir::Expr& index = st.lhs_idx[d];
            if (index.name == "IndexAll") {
              selected[d].reserve((size_t)extent);
              for (int64_t k = 0; k < extent; ++k) selected[d].push_back(k);
            } else if (index.name == "IndexSingle") {
              const long one = as_int(index.args[0]);
              if (one < 1 || one > extent)
                fail("multi-index assignment index out of bounds", st.raw);
              selected[d].push_back(one - 1);
              drops[d] = true;
            } else if (index.name == "IndexBetween" ||
                       index.name == "IndexUpfrom") {
              const long lo = as_int(index.args[0]);
              const long hi =
                  index.name == "IndexBetween" ? as_int(index.args[1]) : extent;
              if (hi >= lo && (lo < 1 || hi > extent))
                fail("multi-index assignment range out of bounds", st.raw);
              for (long k = lo; k <= hi; ++k) selected[d].push_back(k - 1);
            } else if (index.name == "IndexMulti") {
              const Value positions = eval(index.args[0]);
              if (!positions.is_int)
                fail("multi-index assignment needs an int index array", st.raw);
              selected[d].reserve(positions.i.size());
              for (int one : positions.i) {
                if (one < 1 || one > extent)
                  fail("multi-index assignment index out of bounds", st.raw);
                selected[d].push_back(one - 1);
              }
            } else {
              supported = false;
            }
          }
          if (supported) {
            BuiltinIndexMap map;
            try {
              map = builtin_index_map(en->dims, 0, selected, drops,
                                      SliceStorageOrder::FirstIndexFast);
            } catch (const std::invalid_argument& error) {
              fail(std::string("multi-index assignment: ") + error.what(),
                   st.raw);
            }
            if ((int64_t)v.r.size() != map.count ||
                (!map.dimensions.empty() && v.dims != map.dimensions))
              fail("multi-index assignment size mismatch", st.raw);
            for (int64_t k = 0; k < map.count; ++k) {
              const size_t dst = static_cast<size_t>(
                  map.kind == BuiltinSliceMap::Kind::Contiguous ? map.offset + k
                  : map.kind == BuiltinSliceMap::Kind::Strided
                      ? map.offset + k * map.stride
                      : map.gather[(size_t)k]);
              en->r.at(dst) = v.r[(size_t)k];
              if (en->is_int)
                en->i.at(dst) = v.is_int && (size_t)k < v.i.size()
                                    ? v.i[(size_t)k]
                                    : (int)val(v.r[(size_t)k]);
            }
            return;
          }
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
          try {
            for (const auto& k : st.body) exec(k);
          } catch (ContinueV&) {
            continue;
          } catch (BreakV&) {
            break;
          }
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
          try {
            for (const auto& k : st.body) exec(k);
          } catch (ContinueV&) {
            continue;
          } catch (BreakV&) {
            break;
          }
        }
        return;
      }
      case mir::Stmt::Return:
        throw ReturnV{st.has_init ? eval(st.rhs) : Value{}};
      case mir::Stmt::Break:
        throw BreakV{};
      case mir::Stmt::Continue:
        throw ContinueV{};
      case mir::Stmt::NRFunApp:
        // Constraint checks are not executed here, but reject() and
        // print() are: a `reject` in transformed data is how a model
        // validates its data, and CmdStan fails to construct the model
        // there rather than sampling from a model whose data is wrong.
        // Skipping it would mean stanli happily sampled a model CmdStan
        // refuses to build.
        if (const auto action = message_action(st.fn_name)) {
          std::vector<Value> values;
          const MessageSpec spec = lower_message_arguments(
              st.fn_args, [&](const mir::Expr& argument) {
                values.push_back(eval(argument));
              });
          execute_message(*action,
                          render_message(
                              spec, values.size(),
                              [&](size_t k) {
                                return static_cast<int64_t>(values[k].r.size());
                              },
                              [&](size_t k, int64_t i) {
                                return val(values[k].r[static_cast<size_t>(i)]);
                              }));
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
        if (st.fn_name == "FnValidateSizeUnitVector") {
          if (st.fn_args.size() != 3 ||
              st.fn_args[0].kind != mir::Expr::LitStr ||
              st.fn_args[1].kind != mir::Expr::LitStr)
            fail("malformed FnValidateSizeUnitVector", st.raw);
          stan::math::validate_unit_vector_index(
              st.fn_args[0].lit_s.c_str(), st.fn_args[1].lit_s.c_str(),
              static_cast<int>(as_int(st.fn_args[2])));
          return;
        }
        fail("unsupported statement function " + st.fn_name, st.raw);
      case mir::Stmt::Skip:
        return;  // constraint checks are not executed here
      default:
        fail("unsupported statement", st.raw);
    }
  }

 private:
  FunctionArgumentShape function_argument_shape(const mir::Expr& source,
                                                const Value& value) const {
    const FunctionArgumentKind kind =
        source.unsized.leaf == mir::UnsizedLeaf::Int || value.is_int
            ? FunctionArgumentKind::Integer
            : FunctionArgumentKind::Real;
    FunctionContainerKind container = FunctionContainerKind::Scalar;
    FunctionContainerKind leaf = FunctionContainerKind::Scalar;
    std::vector<int64_t> dimensions = value.dims;
    if (source.unsized.depth != 0) {
      container = FunctionContainerKind::Array;
      if (source.unsized.leaf == mir::UnsizedLeaf::Vector)
        leaf = FunctionContainerKind::Vector;
      else if (source.unsized.leaf == mir::UnsizedLeaf::RowVector)
        leaf = FunctionContainerKind::RowVector;
      else if (source.unsized.leaf == mir::UnsizedLeaf::Matrix)
        leaf = FunctionContainerKind::Matrix;
    } else if (source.unsized.leaf == mir::UnsizedLeaf::Vector) {
      container = FunctionContainerKind::Vector;
    } else if (source.unsized.leaf == mir::UnsizedLeaf::RowVector) {
      container = FunctionContainerKind::RowVector;
    } else if (source.unsized.leaf == mir::UnsizedLeaf::Matrix) {
      container = FunctionContainerKind::Matrix;
    } else {
      // Legacy hand-built MIR may only carry the old string type; kind the
      // container from the evaluated value where even that is missing, the
      // way the metadata-free dispatch tail kinds integers.
      if (source.type_ == "UVector")
        container = FunctionContainerKind::Vector;
      else if (source.type_ == "URowVector")
        container = FunctionContainerKind::RowVector;
      else if (source.type_ == "UMatrix" || value.dims.size() == 2)
        container = FunctionContainerKind::Matrix;
      else
        container = value.dims.empty() && value.r.size() == 1
                        ? FunctionContainerKind::Scalar
                        : FunctionContainerKind::Vector;
      if (container == FunctionContainerKind::Matrix && dimensions.size() != 2)
        dimensions = value.dims;
      if (container == FunctionContainerKind::Vector && dimensions.empty())
        dimensions = {static_cast<int64_t>(value.r.size())};
    }
    return make_function_shape(kind, container, leaf, std::move(dimensions),
                               static_cast<int64_t>(value.r.size()));
  }

  // Invoke a probability function through its registered policy. Most use
  // the same graph-kernel CALL bridge as runtime-control programs; the
  // all-integer policy instead calls the shared evaluator because there is no
  // differentiable input on which to hang a graph operation.
  Value density_eval(const mir::Expr& e, const DensitySpec& spec) {
    if ((int)e.args.size() != spec.arity)
      fail(e.name + ": wrong number of arguments", e.raw);
    std::vector<Value> values;
    values.reserve(e.args.size());
    for (const mir::Expr& arg : e.args) values.push_back(eval(arg));
    const auto ints = [&](size_t k) {
      if (!values[k].is_int || values[k].i.empty())
        fail(e.name + ": integer argument is not integer-valued", e.raw);
      return values[k].i;
    };
    std::vector<DensityCallArgument> plan_arguments;
    plan_arguments.reserve(values.size());
    try {
      for (size_t k = 0; k < values.size(); ++k) {
        DensityCallArgument argument;
        argument.shape = function_argument_shape(e.args[k], values[k]);
        argument.scalar = e.args[k].unsized.depth == 0;
        argument.data_only = e.args[k].data_only;
        if constexpr (!std::is_same_v<T, double>)
          argument.active = !e.args[k].data_only;
        if (spec.evaluation == DensityEvaluationPolicy::AllInteger ||
            k < static_cast<size_t>(spec.integer_args)) {
          argument.integers = ints(k);
          // Metadata-free MIR claims depth 0 everywhere; a multi-value
          // integer group is a container whatever the source says.
          if (argument.integers.size() > 1) argument.scalar = false;
        }
        plan_arguments.push_back(std::move(argument));
      }
    } catch (const std::exception& error) {
      fail(e.name + ": " + error.what(), e.raw);
    }
    DensityCallPlan plan;
    try {
      plan =
          density_call_plan(spec, plan_arguments, e.fn_propto && propto_ctx_);
    } catch (const std::exception& error) {
      fail(e.name + ": " + error.what(), e.raw);
    }
    if (spec.evaluation == DensityEvaluationPolicy::AllInteger) {
      Value result;
      result.r = {T(evaluate_packed_all_integer_density(
          spec.all_integer, plan.idata.data(),
          static_cast<int64_t>(plan.idata.size()),
          (plan.variant & 0x80u) != 0))};
      return result;
    }
    if (plan.empty_result) {
      Value result;
      result.r = {T(0.0)};
      return result;
    }

    std::vector<std::vector<T>> inputs;
    inputs.reserve(e.args.size() - (size_t)spec.integer_args);
    for (size_t k = (size_t)spec.integer_args; k < e.args.size(); ++k) {
      std::vector<T> input;
      if (spec.shape == DensityShape::Categorical && k == 0) {
        input.reserve(values[k].i.size());
        for (int value : values[k].i) input.push_back(T((double)value));
      } else {
        input = values[k].r;
      }
      if (e.args[k].unsized.depth != 0 && values[k].dims.size() > 1)
        input = graph_container_order(input, values[k].dims,
                                      e.args[k].unsized.depth);
      inputs.push_back(std::move(input));
    }

    Value result;
    result.r = {run_kernel_call(e, spec.opcode, plan.variant,
                                plan.activity_mask, std::move(plan.idata),
                                std::move(inputs), 1)[0]};
    return result;
  }

  // Bind and execute one graph-kernel CALL over materialized inputs. Shared
  // by density_eval and builtin_kernel_eval: the double interpreter runs the
  // kernel forward directly and the var interpreter replays it through the
  // same nested-tape adapter the register machine uses.
  std::vector<T> run_kernel_call(const mir::Expr& e, uint16_t opcode,
                                 uint8_t variant, uint8_t adjoint_mask,
                                 std::vector<int> idata,
                                 std::vector<std::vector<T>> inputs,
                                 int64_t out_len) {
    Program::Call call;
    call.opcode = opcode;
    call.variant = variant;
    call.input_adjoint_mask = adjoint_mask;
    call.n_in = (int8_t)inputs.size();
    int32_t next = 0;
    for (size_t k = 0; k < inputs.size(); ++k) {
      call.in[k] = next;
      call.in_len[k] = (int32_t)inputs[k].size();
      next += call.in_len[k];
    }
    call.out = next;
    call.out_len = (int32_t)out_len;
    next += (int32_t)out_len;
    call.idata = std::move(idata);

    const Kernel* kernel = find_kernel(opcode);
    if (kernel == nullptr || !bind_call(call))
      fail(e.name + ": graph kernel is unavailable", e.raw);
    call.scratch_len = (int32_t)kernel_call_scratch(
        kernel->scratch_size, opcode, call.variant, call.n_in, call.in_len,
        call.out_len, call.idata.data(), (int64_t)call.idata.size(), nullptr);
    call.scratch = next;
    next += call.scratch_len;
    std::vector<T> registers((size_t)next, T(0.0));
    for (size_t k = 0; k < inputs.size(); ++k)
      std::copy(inputs[k].begin(), inputs[k].end(),
                registers.begin() + call.in[k]);
    if constexpr (std::is_same_v<T, double>) {
      run_call(call, registers.data());
    } else {
      run_call_var(call, registers.data());
    }
    return {registers.begin() + call.out,
            registers.begin() + call.out + out_len};
  }

  // A registered real-result builtin executes through the same graph kernel
  // every lowering backend dispatches to, so a registry entry plus a kernel
  // is complete interpreter support; the named branches in eval_fun remain
  // only as allocation-free fast paths. The interpreter stores every
  // container column-major, arrays included, so unlike the graph layout
  // there is no storage-order idata: an integer array and a real matrix
  // already pair element for element.
  Value builtin_kernel_eval(const mir::Expr& e, const BuiltinSpec& spec,
                            std::vector<Value> values) {
    BuiltinLayout layout;
    try {
      std::vector<BuiltinArgumentShape> shapes;
      shapes.reserve(values.size());
      for (size_t k = 0; k < values.size(); ++k)
        shapes.push_back(function_argument_shape(e.args[k], values[k]));
      layout = builtin_layout(spec, shapes);
    } catch (const std::invalid_argument& error) {
      fail(e.name + ": " + error.what(), e.raw);
    }
    std::vector<std::vector<T>> inputs;
    inputs.reserve(values.size());
    uint8_t activity = 0;
    for (size_t k = 0; k < values.size(); ++k) {
      Value& v = values[k];
      if (v.is_int && v.r.size() != v.i.size()) {
        v.r.clear();
        v.r.reserve(v.i.size());
        for (int value : v.i) v.r.push_back(T((double)value));
      }
      inputs.push_back(std::move(v.r));
      if constexpr (!std::is_same_v<T, double>) {
        if (!e.args[k].data_only) activity |= (uint8_t)(1u << k);
      }
    }
    activity &= spec.activity_mask;
    Value o;
    o.r = run_kernel_call(e, spec.opcode, 0, activity, {}, std::move(inputs),
                          layout.lanes);
    if (spec.shape == BuiltinShapePolicy::Reduction) return o;
    o.dims = values[layout.result_argument].dims;
    if (spec.shape == BuiltinShapePolicy::WholeValue && o.dims.empty())
      o.dims = {(int64_t)o.r.size()};
    if (spec.arity == 2) {
      if (builtin_argument_is_integer(spec, 0) !=
          builtin_argument_is_integer(spec, 1)) {
        // Only falling_factorial and rising_factorial have an int,int
        // overload that answers int; everywhere else two int arguments
        // still make a real, so stanc's own result type decides.
        if (e.type_ == "UInt" && o.r.size() == 1) {
          o.is_int = true;
          o.i = {(int)val(o.r[0])};
        }
      } else if (values[0].is_int && values[1].is_int &&
                 values[0].i.size() == 1 && values[1].i.size() == 1) {
        // Two int scalars keep an int mirror, the rule the operator
        // branches apply.
        o.is_int = true;
        o.i = {(int)val(o.r[0])};
      }
    }
    return o;
  }

  // A registered constructor folds its data scalar arguments through the
  // shared Stan Math evaluator, so extents, spacing rules, and domain errors
  // are CmdStan's own. Arguments read as values: the underlying Stan Math
  // builders take doubles, so there is no gradient to carry.
  Value constructor_eval(const mir::Expr& e, const BuiltinSpec& spec) {
    std::vector<double> arguments;
    arguments.reserve(e.args.size());
    for (const mir::Expr& argument : e.args) {
      const Value v = eval(argument);
      if (v.r.size() != 1) fail(e.name + ": argument must be a scalar", e.raw);
      arguments.push_back(val(v.r[0]));
    }
    ConstructorValue built;
    try {
      built = evaluate_constructor_builtin(spec, arguments);
    } catch (const std::domain_error&) {
      // Stan Math's own validation (a negative extent, an out-of-range
      // index): exactly the rejection CmdStan throws, so pass it through.
      throw;
    } catch (const std::invalid_argument& error) {
      fail(e.name + ": " + error.what(), e.raw);
    }
    Value o;
    o.dims = built.dimensions;
    o.r.reserve(built.values.size());
    for (const double value : built.values) o.r.push_back(T(value));
    if (spec.result == FunctionArgumentKind::Integer) {
      o.is_int = true;
      o.i = std::move(built.integers);
    }
    return o;
  }

  // A registered paired reduction folds two equal-length containers (or one,
  // paired with itself) to a scalar through the dot kernel; squared_distance
  // subtracts first, the same two kernels the graph emits.
  Value paired_reduction_eval(const mir::Expr& e, const BuiltinSpec& spec) {
    Value a = eval(e.args[0]);
    Value b = spec.arity == 2 ? eval(e.args[1]) : a;
    {
      std::vector<BuiltinArgumentShape> shapes;
      try {
        shapes.push_back(function_argument_shape(e.args[0], a));
        if (spec.arity == 2)
          shapes.push_back(function_argument_shape(e.args[1], b));
        (void)builtin_layout(spec, shapes);
      } catch (const std::invalid_argument& error) {
        fail(e.name + ": " + error.what(), e.raw);
      }
    }
    uint8_t activity = 0;
    if constexpr (!std::is_same_v<T, double>) {
      if (spec.arity == 2 && !spec.difference) {
        if (!e.args[0].data_only) activity |= 0x1;
        if (!e.args[1].data_only) activity |= 0x2;
      } else {
        for (const mir::Expr& argument : e.args)
          if (!argument.data_only) activity = 0x3;
      }
    }
    std::vector<T> lhs = a.r;
    std::vector<T> rhs = spec.arity == 2 ? b.r : a.r;
    if (spec.difference) {
      for (size_t i = 0; i < lhs.size(); ++i) lhs[i] = lhs[i] - rhs[i];
      rhs = lhs;
    }
    std::vector<std::vector<T>> inputs;
    inputs.push_back(std::move(lhs));
    inputs.push_back(std::move(rhs));
    Value o;
    o.r = run_kernel_call(e, OP_DOT, 0, activity, {}, std::move(inputs), 1);
    return o;
  }

  // A registered grouped reduction folds each column or row through the
  // shared grouped dot kernel: the same in-order accumulation the AoS
  // reverse-mode overloads perform, and the same kernel every other backend
  // dispatches to, so all three agree bitwise by construction.
  Value grouped_dot_eval(const mir::Expr& e, const BuiltinSpec& spec) {
    Value a = eval(e.args[0]);
    Value b = spec.arity == 2 ? eval(e.args[1]) : a;
    BuiltinGroupedDotMap map;
    try {
      map = builtin_grouped_dot_map(
          spec, function_argument_shape(e.args[0], a),
          function_argument_shape(e.args[spec.arity == 2 ? 1 : 0], b));
    } catch (const std::invalid_argument& error) {
      fail(e.name + ": " + error.what(), e.raw);
    }
    uint8_t activity = 0;
    if constexpr (!std::is_same_v<T, double>) {
      if (spec.arity == 2) {
        if (!e.args[0].data_only) activity |= 0x1;
        if (!e.args[1].data_only) activity |= 0x2;
      } else if (!e.args[0].data_only) {
        activity = 0x3;
      }
    }
    std::vector<std::vector<T>> inputs;
    inputs.push_back(a.r);
    inputs.push_back(spec.arity == 2 ? std::move(b.r) : std::move(a.r));
    Value o;
    o.r = run_kernel_call(e, spec.opcode, 0, activity,
                          {(int)map.groups, (int)map.width,
                           (int)map.group_stride, (int)map.cell_stride},
                          std::move(inputs), map.groups);
    o.dims = {map.groups};
    return o;
  }

  // A registered matrix operation executes the same dedicated kernel every
  // other backend dispatches to. The active variant bit is per context: the
  // double interpreter serves the prim instantiations (write_array,
  // prepare_data) and the var interpreter the reverse-mode ones, which is
  // the distinction the kernels' variant conventions encode.
  Value matrix_op_eval(const mir::Expr& e, const BuiltinSpec& spec) {
    std::vector<Value> values;
    values.reserve(e.args.size());
    for (const mir::Expr& argument : e.args) values.push_back(eval(argument));
    BuiltinMatrixMap map;
    try {
      std::vector<BuiltinArgumentShape> shapes;
      shapes.reserve(values.size());
      for (size_t k = 0; k < values.size(); ++k)
        shapes.push_back(function_argument_shape(e.args[k], values[k]));
      map = builtin_matrix_map(spec, shapes);
    } catch (const std::invalid_argument& error) {
      fail(e.name + ": " + error.what(), e.raw);
    }
    uint8_t activity = 0;
    bool active = false;
    if constexpr (!std::is_same_v<T, double>) {
      for (size_t k = 0; k < e.args.size(); ++k)
        if (!e.args[k].data_only) {
          activity |= (uint8_t)(1u << k);
          active = true;
        }
    }
    activity &= spec.activity_mask;
    std::vector<std::vector<T>> inputs;
    inputs.reserve(values.size());
    for (Value& value : values) inputs.push_back(std::move(value.r));
    std::vector<int> idata;
    idata.reserve(map.idata.size());
    for (const int64_t value : map.idata) idata.push_back((int)value);
    Value o;
    o.r = run_kernel_call(
        e, spec.opcode,
        (uint8_t)(map.variant | (active ? map.active_variant : 0u)), activity,
        std::move(idata), std::move(inputs), map.result.storage_size);
    if (map.result.container != FunctionContainerKind::Scalar)
      o.dims = map.result.dimensions;
    return o;
  }

  // Thrown by a Return statement inside an interpreted function body.
  struct ReturnV {
    Value v;
  };
  struct BreakV {};
  struct ContinueV {};

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

  // The .at() reads in eval_indexed would throw a bare
  // std::out_of_range("vector"); check first so an out-of-range index
  // names the index and the extent instead.
  void bounds(long i, int64_t n, const mir::Expr& e) const {
    if (i < 1 || i > n)
      fail("index " + std::to_string(i) + " out of bounds for size " +
               std::to_string(n),
           e.raw);
  }

  static double val(const T& x) { return stan::math::value_of(x); }

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

  // reduce_sum(f, sliced, grainsize, shared...) sums f over the terms of a
  // partition of `sliced`, and its contract is that the partition is
  // unobservable. Stan Math without STAN_THREADS takes that freedom to its
  // limit and makes exactly one call over the whole slice, returning zero
  // for an empty one (prim/functor/reduce_sum.hpp); stanli has no threading,
  // so both engines make that same single call. lower.cpp's
  // lower_reduce_sum is the graph-side half of this and carries the full
  // account.
  Value call_reduce_sum(const mir::Expr& e) {
    if (e.args.size() < 3)
      fail(
          "reduce_sum: expected a partial-sum function, a sliced argument, "
          "and a grainsize",
          e.raw);
    if (e.args[0].kind != mir::Expr::Var)
      fail("reduce_sum: the partial-sum argument is not a function name",
           e.raw);
    Value slice = eval(e.args[1]);
    const long grainsize = as_int(e.args[2]);
    std::vector<Value> shared;
    shared.reserve(e.args.size() - 3);
    for (size_t i = 3; i < e.args.size(); ++i)
      shared.push_back(eval(e.args[i]));
    // Even an empty slice evaluates every actual argument. Validation is
    // inside the Stan Math call, after those evaluations and their effects.
    stan::math::check_positive("reduce_sum", "grainsize", grainsize);
    const int64_t n =
        slice.dims.empty() ? (int64_t)slice.r.size() : slice.dims.front();
    Value sum;
    sum.r = {0.0};
    if (n == 0) return sum;

    bool propto = false;
    const std::string base =
        mir::reduce_sum_partial_name(e.args[0].name, &propto);
    const std::vector<mir::UnsizedView> views =
        mir::reduce_sum_partial_views(e);
    const mir::FunDef* f = mir::resolve_callback(funs_, base, views);
    if (f == nullptr)
      fail("reduce_sum: unknown partial-sum function " + base, e.raw);
    if (f->arg_names.size() != views.size())
      fail("reduce_sum: " + base + " arity does not match the call", e.raw);
    if (udf_depth_ > 64) fail("UDF recursion too deep");

    MirInterp sub(funs_, where_, hooks_);
    sub.udf_depth_ = udf_depth_ + 1;
    // As in CmdStan, an `_lupdf` functor inherits the caller's normalization
    // and an `_lpdf` one forces the normalized density.
    sub.propto_ctx_ = propto_ctx_ && propto;
    sub.env_[f->arg_names[0]] = std::move(slice);
    Value start;
    start.is_int = true;
    start.i = {1};
    start.r = {1.0};
    if (n > std::numeric_limits<int32_t>::max())
      fail("reduce_sum: slice bound exceeds the Stan integer range", e.raw);
    Value end;
    end.is_int = true;
    end.i = {static_cast<int>(n)};
    end.r = {(double)n};
    sub.env_[f->arg_names[1]] = std::move(start);
    sub.env_[f->arg_names[2]] = std::move(end);
    for (size_t i = 3; i < e.args.size(); ++i)
      sub.env_[f->arg_names[i]] = std::move(shared[i - 3]);
    try {
      for (const auto& st : f->body) sub.exec(st);
    } catch (ReturnV& r) {
      return std::move(r.v);
    }
    fail("reduce_sum: " + base + " returned no value", e.raw);
  }

  // Serial map_rect is structural, just like reduce_sum: each job is one
  // ordinary callback invocation and the vector results are concatenated.
  // Keeping it here gives transformed data, callback fallback, and
  // interpreted write_array the same definition.
  Value call_map_rect(const mir::Expr& e) {
    if (e.args.size() != 5 || e.args[0].kind != mir::Expr::Var)
      fail("map_rect: malformed call", e.raw);
    Value shared = eval(e.args[1]);
    Value jobs = eval(e.args[2]);
    Value real_data = eval(e.args[3]);
    Value int_data = eval(e.args[4]);
    if (jobs.dims.empty()) fail("map_rect: jobs are not an array", e.raw);
    const int64_t n = jobs.dims[0];
    if (n < 0 || (n && jobs.r.size() % (size_t)n != 0) ||
        (n && real_data.r.size() % (size_t)n != 0) ||
        (n && int_data.i.size() % (size_t)n != 0))
      fail("map_rect: job shapes disagree", e.raw);
    const int64_t job_width = n ? (int64_t)jobs.r.size() / n : 0;
    const int64_t real_width = n ? (int64_t)real_data.r.size() / n : 0;
    const int64_t int_width = n ? (int64_t)int_data.i.size() / n : 0;
    const std::vector<mir::UnsizedView> views{{0, mir::UnsizedLeaf::Vector},
                                              {0, mir::UnsizedLeaf::Vector},
                                              {1, mir::UnsizedLeaf::Real},
                                              {1, mir::UnsizedLeaf::Int}};
    const mir::FunDef* f = mir::resolve_callback(funs_, e.args[0].name, views);
    if (!f) fail("map_rect: unknown callback " + e.args[0].name, e.raw);
    Value result;
    for (int64_t job = 0; job < n; ++job) {
      Value job_arg, xr_arg, xi_arg;
      job_arg.dims = {job_width};
      xr_arg.dims = {real_width};
      xi_arg.dims = {int_width};
      xi_arg.is_int = true;
      for (int64_t k = 0; k < job_width; ++k)
        job_arg.r.push_back(jobs.r[(size_t)(job + k * n)]);
      for (int64_t k = 0; k < real_width; ++k)
        xr_arg.r.push_back(real_data.r[(size_t)(job + k * n)]);
      for (int64_t k = 0; k < int_width; ++k) {
        const int value = int_data.i[(size_t)(job + k * n)];
        xi_arg.i.push_back(value);
        xi_arg.r.push_back(T(value));
      }
      Value one = call(*f, {shared, job_arg, xr_arg, xi_arg});
      result.r.insert(result.r.end(), one.r.begin(), one.r.end());
    }
    result.dims = {(int64_t)result.r.size()};
    return result;
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
      bounds(ix, (int64_t)base.r.size(), e);
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
      bounds(i, R, e);
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
          const long ixd = as_int(e.args[1 + d].args[0]);
          bounds(ixd, base.dims[d], e);
          flatpos += (ixd - 1) * stride;
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
      bounds(j, base.dims[1], e);
      r.is_int = base.is_int;
      r.dims = {R};
      r.r.assign(base.r.begin() + (j - 1) * R, base.r.begin() + j * R);
      if (base.is_int)
        r.i.assign(base.i.begin() + (j - 1) * R, base.i.begin() + j * R);
      return r;
    }
    // Leading-Single slice of an N-D entry: fix one or more leading array
    // dimensions and return the remaining container.  For example,
    // array[4, 5] matrix[2, 3] indexed as x[i, j] returns a 2x3 matrix.
    // Flat storage is Fortran (first index fastest), so after fixing q
    // dimensions the selected elements sit at offset + prefix_stride * t.
    const size_t n_indices = e.args.size() - 1;
    if (n_indices > 0 && n_indices < base.dims.size()) {
      bool all_single = true;
      for (size_t k = 1; k < e.args.size(); ++k)
        if (e.args[k].name != "IndexSingle") all_single = false;
      if (all_single) {
        int64_t offset = 0, prefix_stride = 1;
        for (size_t d = 0; d < n_indices; ++d) {
          const long i = as_int(e.args[1 + d].args[0]);
          bounds(i, base.dims[d], e);
          offset += (i - 1) * prefix_stride;
          prefix_stride *= base.dims[d];
        }
        int64_t rest = 1;
        for (size_t d = n_indices; d < base.dims.size(); ++d)
          rest *= base.dims[d];
        r.is_int = base.is_int;
        r.dims.assign(base.dims.begin() + n_indices, base.dims.end());
        for (int64_t k = 0; k < rest; ++k) {
          r.r.push_back(base.r.at(offset + prefix_stride * k));
          if (base.is_int) r.i.push_back(base.i.at(offset + prefix_stride * k));
        }
        return r;
      }
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
        bounds(p, (int64_t)base.r.size(), e);
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
      if (b >= a) {
        bounds(a, (int64_t)base.r.size(), e);
        bounds(b, (int64_t)base.r.size(), e);
      }
      r.is_int = base.is_int;
      r.dims = {b >= a ? b - a + 1 : 0};  // b < a is an empty range
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
      bounds(i, R, e);
      if (b >= a) {
        bounds(a, base.dims[1], e);
        bounds(b, base.dims[1], e);
      }
      r.is_int = base.is_int;
      r.dims = {b >= a ? b - a + 1 : 0};  // b < a is an empty range
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
      bounds(j, base.dims[1], e);
      if (b >= a) {
        bounds(a, R, e);
        bounds(b, R, e);
      }
      r.is_int = base.is_int;
      r.dims = {b >= a ? b - a + 1 : 0};  // b < a is an empty range
      for (long k = a; k <= b; ++k) {
        r.r.push_back(base.r.at((j - 1) * R + (k - 1)));
        if (base.is_int) r.i.push_back(base.i.at((j - 1) * R + (k - 1)));
      }
      return r;
    }
    // General mixed N-D read. DataMap uses one first-index-fast layout for
    // arrays and Eigen leaves alike, so each index selects positions along
    // its corresponding physical dimension. Single drops that dimension;
    // the other index kinds preserve it, and omitted trailing indices keep
    // the complete suffix. Iterating the last source dimension outermost
    // emits the selected value in the same first-index-fast layout.
    if (n_indices > 0 && n_indices <= base.dims.size()) {
      std::vector<std::vector<int64_t>> selected(n_indices);
      std::vector<bool> drops(n_indices, false);
      bool supported = true;
      for (size_t d = 0; d < n_indices; ++d) {
        const int64_t extent = base.dims[d];
        const mir::Expr& index = e.args[1 + d];
        if (index.name == "IndexSingle") {
          const long i = as_int(index.args[0]);
          bounds(i, extent, e);
          selected[d].push_back(i - 1);
          drops[d] = true;
        } else if (index.name == "IndexAll") {
          selected[d].reserve(static_cast<size_t>(extent));
          for (int64_t i = 0; i < extent; ++i) selected[d].push_back(i);
        } else if (index.name == "IndexMulti") {
          const Value positions = eval(index.args[0]);
          const size_t n = std::max(positions.i.size(), positions.r.size());
          selected[d].reserve(n);
          for (size_t k = 0; k < n; ++k) {
            const long i = k < positions.i.size()
                               ? positions.i[k]
                               : static_cast<long>(val(positions.r.at(k)));
            bounds(i, extent, e);
            selected[d].push_back(i - 1);
          }
        } else if (index.name == "IndexBetween") {
          const long lo = as_int(index.args[0]);
          const long hi = as_int(index.args[1]);
          if (hi >= lo) {
            bounds(lo, extent, e);
            bounds(hi, extent, e);
          }
          selected[d].reserve(static_cast<size_t>(hi >= lo ? hi - lo + 1 : 0));
          for (long i = lo; i <= hi; ++i) selected[d].push_back(i - 1);
        } else if (index.name == "IndexUpfrom") {
          const long lo = as_int(index.args[0]);
          bounds(lo, extent, e);
          selected[d].reserve(static_cast<size_t>(extent - lo + 1));
          for (long i = lo; i <= extent; ++i) selected[d].push_back(i - 1);
        } else {
          supported = false;
          break;
        }
      }
      if (supported) {
        // The shared index geometry over the interpreter's first-index-fast
        // storage; trailing axes keep their full extent inside the resolver.
        const BuiltinIndexMap map = builtin_index_map(
            base.dims, 0, selected, drops, SliceStorageOrder::FirstIndexFast);
        r.is_int = base.is_int;
        r.dims = map.dimensions;
        r.r.reserve(static_cast<size_t>(map.count));
        if (base.is_int) r.i.reserve(static_cast<size_t>(map.count));
        for (int64_t k = 0; k < map.count; ++k) {
          const int64_t cell = map.kind == BuiltinSliceMap::Kind::Contiguous
                                   ? map.offset + k
                               : map.kind == BuiltinSliceMap::Kind::Strided
                                   ? map.offset + k * map.stride
                                   : map.gather[static_cast<size_t>(k)];
          r.r.push_back(base.r.at(static_cast<size_t>(cell)));
          if (base.is_int) r.i.push_back(base.i.at(static_cast<size_t>(cell)));
        }
        return r;
      }
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
    const FunctionSpec* registered = function_spec(e);
    const BuiltinSpec* builtin =
        registered != nullptr && registered->builtin() != nullptr
            ? registered->builtin()
            : nullptr;
    if (mir::stateful_intrinsic_kind(e))
      fail("target() is unavailable in this context", e.raw);
    if (const auto value = mir::nullary_constant(e)) {
      r.r = {T(*value)};
      return r;
    }
    if (e.fn_lib == mir::Expr::Lib::UserDefined) return call_udf(e);
    if (mir::is_reduce_sum(e)) return call_reduce_sum(e);
    if (e.name == "map_rect") return call_map_rect(e);
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
    const auto resolve_registered = [&](const std::vector<Value>& values) {
      if (builtin == nullptr)
        fail(e.name + ": missing builtin descriptor", e.raw);
      std::vector<BuiltinArgumentShape> shapes;
      shapes.reserve(values.size());
      try {
        for (size_t k = 0; k < values.size(); ++k)
          shapes.push_back(function_argument_shape(e.args[k], values[k]));
        return builtin_layout(*builtin, shapes);
      } catch (const std::invalid_argument& error) {
        fail(e.name + ": " + error.what(), e.raw);
      }
    };
    // Elementwise with scalar broadcasting; results of real math are real
    // even on int inputs, and binary int results stay int only when both
    // sides are int scalars.
    auto bin = [&](auto f) {
      Value a = eval(e.args[0]), b = eval(e.args[1]);
      Value o;
      size_t n;
      size_t result_argument;
      if (builtin != nullptr) {
        const BuiltinLayout layout = resolve_registered({a, b});
        n = (size_t)layout.lanes;
        result_argument = layout.result_argument;
      } else {
        if (!shapes_match(a, b)) fail(e.name + ": incompatible shapes", e.raw);
        n = broadcast_size(a, b);
        result_argument = is_scalar(a) && !is_scalar(b) ? 1 : 0;
      }
      o.r.resize(n);
      for (size_t i = 0; i < n; ++i)
        o.r[i] = f(a.r[a.r.size() == 1 ? 0 : i], b.r[b.r.size() == 1 ? 0 : i]);
      o.dims = result_argument == 0 ? a.dims : b.dims;
      if (a.is_int && b.is_int && a.i.size() == 1 && b.i.size() == 1) {
        o.is_int = true;
        o.i = {(int)val(f(T((double)a.i[0]), T((double)b.i[0])))};
      }
      return o;
    };
    auto un = [&](auto f) {
      Value a = eval(e.args[0]);
      if (builtin != nullptr) (void)resolve_registered({a});
      Value o;
      o.dims = a.dims;
      o.r.resize(a.r.size());
      for (size_t i = 0; i < a.r.size(); ++i) o.r[i] = f(a.r[i]);
      return o;
    };
    if (builtin != nullptr &&
        builtin->shape == BuiltinShapePolicy::Elementwise &&
        builtin->result == FunctionArgumentKind::Integer) {
      Value a = eval(e.args[0]);
      const auto restore_legacy_integer = [&](size_t index, Value* value) {
        const mir::Expr& source = e.args[index];
        const bool integer_type =
            source.unsized.leaf == mir::UnsizedLeaf::Int ||
            source.type_ == "UInt";
        if (!value->is_int && integer_type) {
          value->is_int = true;
          value->i.reserve(value->r.size());
          for (const T& lane : value->r)
            value->i.push_back((int)std::llround(val(lane)));
        }
      };
      restore_legacy_integer(0, &a);
      if (!a.is_int) fail(e.name + ": expected integer arguments", e.raw);
      if (builtin->arity == 1) {
        (void)resolve_registered({a});
        Value o;
        o.is_int = true;
        o.dims = a.dims;
        o.i.resize(a.i.size());
        o.r.resize(a.i.size());
        for (size_t i = 0; i < a.i.size(); ++i) {
          const int value = evaluate_integer_unary_builtin(*builtin, a.i[i]);
          o.i[i] = value;
          o.r[i] = T((double)value);
        }
        return o;
      }
      Value b = eval(e.args[1]);
      restore_legacy_integer(1, &b);
      if (!b.is_int) fail(e.name + ": expected integer arguments", e.raw);
      const BuiltinLayout layout = resolve_registered({a, b});
      Value o;
      const size_t n = (size_t)layout.lanes;
      o.is_int = true;
      o.dims = layout.result_argument == 0 ? a.dims : b.dims;
      o.i.resize(n);
      o.r.resize(n);
      for (size_t i = 0; i < n; ++i) {
        const int value = evaluate_integer_binary_builtin(
            *builtin, a.i[a.i.size() == 1 ? 0 : i],
            b.i[b.i.size() == 1 ? 0 : i]);
        o.i[i] = value;
        o.r[i] = T((double)value);
      }
      return o;
    }
    // The comparison operators: compare values, answer 1.0 or 0.0, and
    // otherwise take bin's broadcasting and int-scalar rules unchanged.
    auto cmp = [&](auto f) {
      return bin([f](const T& x, const T& y) {
        return T(f(val(x), val(y)) ? 1.0 : 0.0);
      });
    };
    if ((e.name == "Plus__" || e.name == "Minus__") && e.type_ == "UInt") {
      const long x = as_int(e.args[0]), y = as_int(e.args[1]);
      const long q = e.name == "Plus__" ? x + y : x - y;
      r.is_int = true;
      r.i = {(int)q};
      r.r = {T((double)q)};
      return r;
    }
    if (e.name == "Times__" || e.name == "multiply") {
      // Times on shaped operands is linear algebra, not elementwise; only
      // a scalar operand (either side) scales elementwise. The shared
      // product resolver owns the classification and the inner-dimension
      // check; interpreter values carry no orientation, so a one-axis
      // operand takes the orientation its position implies -- a row before
      // a matrix, a column after one, and the result type splits the
      // one-axis pair into outer product against dot product.
      Value a = eval(e.args[0]), b = eval(e.args[1]);
      const bool a_mat = a.dims.size() == 2, b_mat = b.dims.size() == 2;
      const bool outer_pair = !a_mat && !b_mat && e.type_ == "UMatrix";
      const bool inner_pair =
          !a_mat && !b_mat && (e.type_ == "UReal" || e.type_ == "UInt");
      if (!is_scalar(a) && !is_scalar(b) &&
          (a_mat || b_mat || outer_pair || inner_pair)) {
        using Kind = FunctionContainerKind;
        const auto oriented = [](const Value& v, Kind fallback) {
          return make_function_shape(
              FunctionArgumentKind::Real,
              v.dims.size() == 2 ? Kind::Matrix : fallback, Kind::Scalar,
              v.dims.size() == 2 ? v.dims
                                 : std::vector<int64_t>{(int64_t)v.r.size()},
              (int64_t)v.r.size());
        };
        BuiltinProductMap map;
        try {
          map = builtin_product_map(
              oriented(a, outer_pair ? Kind::Vector : Kind::RowVector),
              oriented(b, outer_pair ? Kind::RowVector : Kind::Vector));
        } catch (const std::invalid_argument& error) {
          fail(e.name + ": " + error.what(), e.raw);
        }
        switch (map.kind) {
          case BuiltinProductMap::Kind::Gemm:
            // Matrix * matrix must use Eigen's product evaluator, just as
            // the graph GEMM and generated Stan do. A hand-written scalar
            // loop has a measurably different association for a one-column
            // product.
            if (a_mat && b_mat) {
              using Mat = Eigen::Matrix<T, Eigen::Dynamic, Eigen::Dynamic>;
              r.r.resize((size_t)(map.m * map.n));
              Eigen::Map<const Mat> am(a.r.data(), map.m, map.k);
              Eigen::Map<const Mat> bm(b.r.data(), map.k, map.n);
              Eigen::Map<Mat>(r.r.data(), map.m, map.n) = am * bm;
              r.dims = {map.m, map.n};
              return r;
            }
            [[fallthrough]];
          case BuiltinProductMap::Kind::MatVec: {
            // Col-major storage on both sides.
            r.r.assign((size_t)(map.m * map.n), T(0.0));
            for (int64_t j = 0; j < map.n; ++j)
              for (int64_t k = 0; k < map.k; ++k) {
                const T& bv = b.r.at((size_t)(b_mat ? j * map.k + k : k));
                for (int64_t i = 0; i < map.m; ++i)
                  r.r[(size_t)(j * map.m + i)] +=
                      a.r.at((size_t)(a_mat ? k * map.m + i : k)) * bv;
              }
            r.dims = {(int64_t)r.r.size()};
            return r;
          }
          case BuiltinProductMap::Kind::Outer: {
            r.dims = {map.m, map.n};
            for (int64_t j = 0; j < map.n; ++j)
              for (int64_t i = 0; i < map.m; ++i)
                r.r.push_back(a.r[(size_t)i] * b.r[(size_t)j]);
            return r;
          }
          case BuiltinProductMap::Kind::Inner: {
            T s = T(0.0);
            for (size_t i = 0; i < a.r.size(); ++i) s += a.r[i] * b.r[i];
            r.r = {s};
            return r;
          }
          case BuiltinProductMap::Kind::ScalarScale:
            break;
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
    // `A \ v` and `rv / A` are linear solves. stanc spells them with the
    // ordinary division operators, so the divisor's type is what tells a
    // solve from elementwise division by a scalar; `./` is never a solve.
    // The named spellings arrive with the same argument order the operators
    // use, and pick a factorisation family with them: the plain solve, the
    // LLT of a symmetric positive definite matrix, or a triangular solve
    // that reads only the lower triangle.
    const BuiltinSpec* solve_spec =
        shaped_builtin_spec(e.name, e.args.size(), BuiltinShapePolicy::Solve);
    if (solve_spec == nullptr && e.name == "Divide__" && e.args.size() == 2 &&
        e.args.at(1).type_ == "UMatrix")
      solve_spec =
          shaped_builtin_spec("mdivide_right", 2, BuiltinShapePolicy::Solve);
    if (solve_spec != nullptr) {
      const bool left = solve_spec->solve_left;
      const BuiltinSolveKind kind = solve_spec->solve;
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
      const auto left_solve = [&](const auto& x) {
        switch (kind) {
          case BuiltinSolveKind::Spd:
            return Mat(stan::math::mdivide_left_spd(d, x));
          case BuiltinSolveKind::TriLow:
            return Mat(stan::math::mdivide_left_tri_low(d, x));
          default:
            return Mat(stan::math::mdivide_left(d, x));
        }
      };
      const auto right_solve = [&](const auto& x) {
        switch (kind) {
          case BuiltinSolveKind::Spd:
            return Mat(stan::math::mdivide_right_spd(x, d));
          case BuiltinSolveKind::TriLow:
            return Mat(stan::math::mdivide_right_tri_low(x, d));
          default:
            return Mat(stan::math::mdivide_right(x, d));
        }
      };
      Mat out;
      if (dividend.dims.size() == 2) {
        out = left ? left_solve(shaped(dividend, left))
                   : right_solve(shaped(dividend, left));
      } else if (left) {
        Eigen::Matrix<T, Eigen::Dynamic, 1> x(dividend.r.size());
        for (size_t i = 0; i < dividend.r.size(); ++i)
          x((Eigen::Index)i) = dividend.r[i];
        out = left_solve(x);
      } else {
        Eigen::Matrix<T, 1, Eigen::Dynamic> x(dividend.r.size());
        for (size_t i = 0; i < dividend.r.size(); ++i)
          x((Eigen::Index)i) = dividend.r[i];
        out = right_solve(x);
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
    // Callable transforms share their name/arity dispatch and their typed
    // execution with graph lowering and runtime-control Programs. The
    // interpreter is used for write_array with jacobian__ false, so it keeps
    // the constrained value and deliberately discards the auxiliary lp.
    CallableTransformSpec transform;
    if (callable_transform(e.name, &transform) &&
        transform.arity == e.args.size()) {
      if (transform.structured &&
          transform.direction == TransformDirection::Unconstrain)
        fail(e.name + ": structured inverse is unsupported", e.raw);
      std::vector<Value> a;
      a.reserve(e.args.size());
      for (const mir::Expr& arg : e.args) a.push_back(eval(arg));
      Program::Transform tr;
      tr.kind = transform.kind;
      tr.direction = transform.direction;
      tr.n_in = transform.structured ? 1 : (int8_t)transform.arity;
      Value o;
      o.dims = a[0].dims;
      size_t outer_rank = 0;
      if (!transform.structured) {
        for (int k = 1; k < tr.n_in; ++k) {
          const size_t n = a[(size_t)k].r.size();
          if (n != 1 && n != a[0].r.size())
            fail(e.name + ": bound has incompatible size", e.raw);
        }
        tr.out_len = (int32_t)a[0].r.size();
        tr.inner_raw = tr.out_len;
      } else {
        outer_rank = e.args[0].unsized.depth;
        const bool input_matrix =
            e.args[0].unsized.leaf == mir::UnsizedLeaf::Matrix;
        if (o.dims.size() < outer_rank + (input_matrix ? 2u : 1u))
          fail(e.name + ": incomplete input dimensions", e.raw);
        int64_t batch = 1;
        for (size_t i = 0; i < outer_rank; ++i) batch *= o.dims[i];
        const int64_t raw_rows =
            input_matrix ? o.dims[outer_rank] : o.dims.back();
        const int64_t raw_cols = input_matrix ? o.dims[outer_rank + 1] : 0;
        int64_t rows = 0, cols = 0;
        switch (transform.kind) {
          case CallableTransformKind::Ordered:
          case CallableTransformKind::PositiveOrdered:
          case CallableTransformKind::UnitVector:
            rows = raw_rows;
            break;
          case CallableTransformKind::Simplex:
            rows = raw_rows + 1;
            break;
          case CallableTransformKind::SumToZero:
            rows = raw_rows + 1;
            if (input_matrix) cols = raw_cols + 1;
            break;
          case CallableTransformKind::StochasticColumn:
            rows = raw_rows + 1;
            cols = raw_cols;
            break;
          case CallableTransformKind::StochasticRow:
            rows = raw_rows;
            cols = raw_cols + 1;
            break;
          case CallableTransformKind::CholeskyFactorCorr:
          case CallableTransformKind::CorrMatrix:
          case CallableTransformKind::CovMatrix:
            rows = cols = as_int(e.args[1]);
            break;
          case CallableTransformKind::CholeskyFactorCov:
            rows = as_int(e.args[1]);
            cols = as_int(e.args[2]);
            break;
          default:
            fail(e.name + ": invalid structured transform", e.raw);
        }
        tr.batch = (int32_t)batch;
        tr.inner_raw =
            input_matrix ? (int32_t)(raw_rows * raw_cols) : (int32_t)raw_rows;
        tr.out_rows = (int32_t)rows;
        tr.out_cols = (int32_t)cols;
        const bool output_matrix = e.unsized.leaf == mir::UnsizedLeaf::Matrix;
        const int64_t inner_con = output_matrix ? rows * cols : rows;
        tr.out_len = (int32_t)(batch * inner_con);
        o.dims.resize(outer_rank);
        o.dims.push_back(rows);
        if (output_matrix) o.dims.push_back(cols);
      }
      std::vector<T> reg;
      for (int k = 0; k < tr.n_in; ++k) {
        tr.in[k] = (int32_t)reg.size();
        std::vector<T> input =
            transform.structured && k == 0 && outer_rank > 0
                ? graph_container_order(a[0].r, a[0].dims, outer_rank)
                : a[(size_t)k].r;
        tr.in_len[k] = (int32_t)input.size();
        reg.insert(reg.end(), input.begin(), input.end());
      }
      tr.out = (int32_t)reg.size();
      reg.resize(reg.size() + (size_t)tr.out_len);
      tr.jac = (int32_t)reg.size();
      reg.emplace_back(T(0));
      run_program_transform(tr, reg.data());
      o.r.assign(reg.begin() + tr.out, reg.begin() + tr.out + tr.out_len);
      if (transform.structured && outer_rank > 0)
        o.r = serialized_container_order(o.r, o.dims, outer_rank);
      return o;
    }
    if ((e.name == "PMinus__" || e.name == "minus") && e.args.size() == 1 &&
        e.type_ == "UInt") {
      const long x = as_int(e.args[0]);
      r.is_int = true;
      r.i = {(int)(-x)};
      r.r = {T((double)(-x))};
      return r;
    }
    if (e.name == "PPlus__" || (e.name == "plus" && e.args.size() == 1))
      return un([](const T& x) { return x; });
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
    if (const std::optional<GpCov> gp = gp_cov_family(e.name);
        gp && e.args.size() == 3) {
      Value x = eval(e.args[0]);
      const T sigma = eval(e.args[1]).r.at(0);
      const T length_scale = eval(e.args[2]).r.at(0);
      const int64_t N = x.dims.size() == 2 ? x.dims[0] : (int64_t)x.r.size();
      const int64_t D = x.dims.size() == 2 ? x.dims[1] : 1;
      using Vec = Eigen::Matrix<T, Eigen::Dynamic, 1>;
      std::vector<Vec> pts((size_t)N, Vec((Eigen::Index)D));
      for (int64_t n = 0; n < N; ++n)
        for (int64_t d = 0; d < D; ++d)
          pts[(size_t)n]((Eigen::Index)d) = x.r.at((size_t)(n + N * d));
      Eigen::Matrix<T, Eigen::Dynamic, Eigen::Dynamic> c;
      if (*gp == kGpMatern32)
        c = stan::math::gp_matern32_cov(pts, sigma, length_scale);
      else if (*gp == kGpMatern52)
        c = stan::math::gp_matern52_cov(pts, sigma, length_scale);
      else
        c = stan::math::gp_exponential_cov(pts, sigma, length_scale);
      Value o;
      o.dims = {N, N};
      o.r.resize((size_t)(N * N));
      for (int64_t j = 0; j < N; ++j)
        for (int64_t i = 0; i < N; ++i) o.r[(size_t)(j * N + i)] = c(i, j);
      return o;
    }
    // Integer sums keep their int mirror for downstream size and index use;
    // real sums, like mean/sd/variance and the unary log_sum_exp reduction,
    // ride the registered reduction kernel through the shared CALL bridge.
    if (e.name == "sum" && e.args.size() == 1) {
      Value a = eval(e.args[0]);
      if (a.is_int && a.i.size() == a.r.size()) {
        int total = 0;
        for (int x : a.i) total += x;
        r.is_int = true;
        r.i = {total};
        r.r = {T((double)total)};
        return r;
      }
      const BuiltinSpec* reduction = reduction_builtin_spec(e.name, 1);
      if (reduction == nullptr)
        fail("sum: missing reduction descriptor", e.raw);
      std::vector<Value> values;
      values.push_back(std::move(a));
      return builtin_kernel_eval(e, *reduction, std::move(values));
    }
    // Registered slice/view selections: the shared resolver maps result
    // cells to source cells over this interpreter's first-index-fast
    // storage, with Stan Math's own index checks (out_of_range and
    // domain_error propagate as CmdStan's rejections). Copying a `var` lane
    // shares its node, so adjoints accumulate on the source exactly as the
    // graph's slice kernels scatter-add.
    if (const BuiltinSpec* slice = shaped_builtin_spec(
            e.name, e.args.size(), BuiltinShapePolicy::SliceView)) {
      Value a = eval(e.args[0]);
      Value b;
      BuiltinSliceMap map;
      try {
        if (builtin_slice_is_append(slice->slice)) {
          // Appends map result cells over both operands' concatenated
          // storage; a source cell at or past the left run reads the right.
          b = eval(e.args[1]);
          map =
              builtin_append_map(*slice, function_argument_shape(e.args[0], a),
                                 function_argument_shape(e.args[1], b),
                                 SliceStorageOrder::FirstIndexFast);
        } else {
          std::vector<int64_t> indexes;
          indexes.reserve(e.args.size() - 1);
          for (size_t k = 1; k < e.args.size(); ++k)
            indexes.push_back(as_int(e.args[k]));
          map = builtin_slice_map(*slice, function_argument_shape(e.args[0], a),
                                  indexes, SliceStorageOrder::FirstIndexFast);
        }
      } catch (const std::invalid_argument& error) {
        fail(e.name + ": " + error.what(), e.raw);
      }
      const bool integer = a.is_int &&
                           map.result.value == FunctionArgumentKind::Integer &&
                           (!builtin_slice_is_append(slice->slice) || b.is_int);
      const int64_t split = (int64_t)a.r.size();
      r.dims = map.result.dimensions;
      r.is_int = integer;
      r.r.reserve((size_t)map.count);
      if (integer) r.i.reserve((size_t)map.count);
      const auto emit_cell = [&](int64_t cell) {
        const Value& source = cell < split ? a : b;
        const size_t index = (size_t)(cell < split ? cell : cell - split);
        r.r.push_back(source.r.at(index));
        if (integer) r.i.push_back(source.i.at(index));
      };
      switch (map.kind) {
        case BuiltinSliceMap::Kind::Contiguous:
          for (int64_t k = 0; k < map.count; ++k) emit_cell(map.offset + k);
          break;
        case BuiltinSliceMap::Kind::Strided:
          for (int64_t k = 0; k < map.count; ++k)
            emit_cell(map.offset + k * map.stride);
          break;
        case BuiltinSliceMap::Kind::Transpose: {
          const int64_t rows = map.result.dimensions[0];
          const int64_t columns = map.result.dimensions[1];
          for (int64_t k = 0; k < map.count; ++k)
            emit_cell(k / rows + columns * (k % rows));
          break;
        }
        case BuiltinSliceMap::Kind::Gather:
          for (const int64_t source : map.gather) emit_cell(source);
          break;
      }
      return r;
    }
    if (e.name == "tcrossprod" && e.args.size() == 1) {
      Value a = eval(e.args[0]);
      if (a.dims.size() != 2) fail("tcrossprod: needs a matrix", e.raw);
      const int64_t rows = a.dims[0], cols = a.dims[1];
      r.dims = {rows, rows};
      r.r.assign((size_t)(rows * rows), T(0.0));
      for (int64_t j = 0; j < rows; ++j)
        for (int64_t k = 0; k < cols; ++k) {
          const T& rhs = a.r.at((size_t)(k * rows + j));
          for (int64_t i = 0; i < rows; ++i)
            r.r[(size_t)(j * rows + i)] += a.r.at((size_t)(k * rows + i)) * rhs;
        }
      return r;
    }
    if (e.name == "prod") {
      Value a = eval(e.args[0]);
      if (a.r.empty()) {
        r.r = {T(1.0)};
        return r;
      }
      const bool eigen_container =
          e.args[0].unsized.depth == 0 &&
          (e.args[0].unsized.leaf == mir::UnsizedLeaf::Vector ||
           e.args[0].unsized.leaf == mir::UnsizedLeaf::RowVector);
      const ExpressionLayout layout =
          udf_depth_ == 0 ? mir::source_expression_layout(e.args[0])
                          : ExpressionLayout::unknown();
      if (!eigen_container || !layout.known()) {
        T product = T(1.0);
        for (const T& value : a.r) product *= value;
        r.r = {product};
        return r;
      }
      if (layout.kind == ExpressionLayout::Kind::Scalar) {
        T product = a.r[0];
        for (size_t i = 1; i < a.r.size(); ++i) product *= a.r[i];
        r.r = {product};
        return r;
      }
      if (layout.kind == ExpressionLayout::Kind::Direct &&
          layout.element_offset != 0) {
        if constexpr (std::is_same_v<T, double>) {
          r.r = {prod_phased(a.r.data(), static_cast<int64_t>(a.r.size()),
                             layout.element_offset)};
          return r;
        }
        T product = T(1.0);
        for (const T& value : a.r) product *= value;
        r.r = {product};
        return r;
      }
      // Match the address-independent packet grouping used by the compiled
      // generated-quantities kernel.  A direct Map (including the Map used
      // by stan::math::prod(std::vector)) can pick a different alignedStart
      // when this vector's allocation is shifted under AVX.
      using Vec = Eigen::Matrix<T, Eigen::Dynamic, 1>;
      const Eigen::Map<const Vec> input(a.r.data(), a.r.size());
      r.r = {stan::math::prod(
          input.unaryExpr(Eigen::internal::core_cast_op<T, T>()))};
      return r;
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
    // Registered predicates: the shared evaluator answers the operator
    // spellings and their logical_* library names identically. Comparisons
    // keep cmp's broadcasting and int-scalar rules; negation maps
    // elementwise as before; the IEEE classifications stay scalar.
    if (const BuiltinSpec* pred = shaped_builtin_spec(
            e.name, e.args.size(), BuiltinShapePolicy::Predicate)) {
      if (pred->arity == 2)
        return cmp([pred](double x, double y) {
          return evaluate_predicate_builtin(*pred, x, y) != 0;
        });
      if (pred->predicate == BuiltinPredicate::Negation)
        return un([pred](const T& x) {
          return T((double)evaluate_predicate_builtin(*pred, val(x)));
        });
      Value a = eval(e.args[0]);
      if (a.r.size() != 1) fail(e.name + ": needs a scalar", e.raw);
      const int answer = evaluate_predicate_builtin(*pred, val(a.r[0]));
      r.is_int = true;
      r.i = {answer};
      r.r = {T((double)answer)};
      return r;
    }
    if (e.name == "max" || e.name == "min") {
      const bool owning_container_context =
          where_ == "write_array" || where_ == "prepare_data";
      const mir::ExtremaCall native =
          owning_container_context && udf_depth_ == 0 ? mir::extrema_call(e)
                                                      : mir::ExtremaCall{};
      const bool real_container =
          native.surface == mir::ExtremaSurface::RealVector ||
          native.surface == mir::ExtremaSurface::RealMatrix ||
          native.surface == mir::ExtremaSurface::RealArray;
      if (native.kind != mir::ExtremaKind::Legacy && real_container) {
        // A bare matrix transpose is deliberately refused by graph lowering:
        // Eigen's transpose evaluator has a packet order that cannot be
        // reconstructed from a materialized col-major transpose. Preserve
        // that evaluator here as the fallback contract, including its
        // platform-dependent NaN and signed-zero selection.
        const mir::Expr& operand = e.args[0];
        const bool bare_matrix_transpose =
            (operand.name == "Transpose__" || operand.name == "transpose") &&
            operand.args.size() == 1 &&
            operand.args[0].kind == mir::Expr::Var &&
            operand.args[0].unsized.depth == 0 &&
            operand.args[0].unsized.leaf == mir::UnsizedLeaf::Matrix;
        if (bare_matrix_transpose) {
          Value source = eval(operand.args[0]);
          if (source.dims.size() != 2 || source.dims[0] < 0 ||
              source.dims[1] < 0 ||
              (source.dims[0] != 0 &&
               source.dims[1] >
                   std::numeric_limits<int64_t>::max() / source.dims[0]) ||
              source.dims[0] * source.dims[1] !=
                  static_cast<int64_t>(source.r.size()))
            fail("min/max matrix transpose has invalid dimensions", e.raw);
          if (source.r.empty()) {
            r.r = {native.kind == mir::ExtremaKind::Min
                       ? T(std::numeric_limits<double>::infinity())
                       : T(-std::numeric_limits<double>::infinity())};
            return r;
          }
          using Mat = Eigen::Matrix<T, Eigen::Dynamic, Eigen::Dynamic>;
          const Eigen::Map<const Mat> matrix(source.r.data(), source.dims[0],
                                             source.dims[1]);
          r.r = {native.kind == mir::ExtremaKind::Min
                     ? stan::math::min(matrix.transpose())
                     : stan::math::max(matrix.transpose())};
          return r;
        }
        Value a = eval(e.args[0]);
        if (a.r.empty()) {
          r.r = {native.kind == mir::ExtremaKind::Min
                     ? T(std::numeric_limits<double>::infinity())
                     : T(-std::numeric_limits<double>::infinity())};
          return r;
        }
        const ExpressionLayout layout =
            mir::source_expression_layout(e.args[0]);
        if (layout.kind == ExpressionLayout::Kind::Direct &&
            layout.element_offset != 0) {
          if constexpr (std::is_same_v<T, double>) {
            r.r = {T(extrema_phased(
                a.r.data(), static_cast<int64_t>(a.r.size()),
                layout.element_offset, native.kind == mir::ExtremaKind::Max))};
            return r;
          }
        }
        if (!layout.packet_access()) {
          // Indexed views have no packet traversal.  Keep the same strict
          // comparison and ascending coefficient order as Eigen's default
          // redux path; this also covers matrix rows and gathers.
          const auto vmax = [](const T& x, const T& y) {
            return val(x) < val(y) ? y : x;
          };
          const auto vmin = [](const T& x, const T& y) {
            return val(y) < val(x) ? y : x;
          };
          T m = a.r.at(0);
          for (const T& x : a.r)
            m = native.kind == mir::ExtremaKind::Max ? vmax(m, x) : vmin(m, x);
          r.r = {m};
          return r;
        }
        // The std::vector backing an interpreted value has no aligned-owning
        // address contract.  Clear DirectAccessBit but retain packet access,
        // making lane zero Eigen's packet start just as for CmdStan's owning
        // vector and the graph kernel.
        using Vec = Eigen::Matrix<T, Eigen::Dynamic, 1>;
        const Eigen::Map<const Vec> input(a.r.data(), a.r.size());
        const auto owning_grouping =
            input.unaryExpr(Eigen::internal::core_cast_op<T, T>());
        r.r = {native.kind == mir::ExtremaKind::Min
                   ? stan::math::min(owning_grouping)
                   : stan::math::max(owning_grouping)};
        return r;
      }
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
      if (a.r.empty()) {
        if (a.is_int) {
          // Preserve Stan Math's exception class and diagnostic for the
          // excluded integer-container overload.
          if (e.name == "min")
            (void)stan::math::min(a.i);
          else
            (void)stan::math::max(a.i);
        }
        r.r = {e.name == "min" ? T(std::numeric_limits<double>::infinity())
                               : T(-std::numeric_limits<double>::infinity())};
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
        // Container identity is independent of storage width. In particular,
        // nested singleton arrays and length-one Eigen leaves still add a
        // logical axis; treating them as scalars collapses the dimensions
        // later consumed by graph and register-program lowering.
        if (!is_scalar(v2) || rows_mode) {
          // Row-vector elements: build a matrix, row-major.
          rows_mode = true;
          row_len = (int64_t)v2.r.size();
          elem_dims = v2.dims;
          o.r.insert(o.r.end(), v2.r.begin(), v2.r.end());
          if (o.is_int && v2.is_int && v2.i.size() == v2.r.size())
            o.i.insert(o.i.end(), v2.i.begin(), v2.i.end());
          else
            o.is_int = false;
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
        if (o.is_int) {
          std::vector<int> cm_i(R * C);
          for (int64_t i = 0; i < R; ++i)
            for (int64_t j = 0; j < C; ++j)
              cm_i[j * R + i] = o.i[(size_t)(i * C + j)];
          o.i = std::move(cm_i);
        }
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
    // Registered shape queries answer through the shared resolver, with
    // vector orientation from the MIR type -- the same source the graph
    // reads, so the two paths cannot drift. FnLength stays outside the
    // registry: the compiler internal stanc3 emits for the observation
    // count in a vectorized `T[,]` normalizer spells stan::math::size,
    // which counts every element, so it answers as num_elements does
    // rather than as size does.
    if (const BuiltinSpec* query =
            e.args.size() == 1
                ? shaped_builtin_spec(e.name, 1, BuiltinShapePolicy::ShapeQuery)
                : nullptr) {
      Value a = eval(e.args[0]);
      if (a.is_int && a.r.size() != a.i.size()) {
        // Integer values may carry only the int mirror; the shape helper
        // reads storage off the real side.
        a.r.clear();
        a.r.reserve(a.i.size());
        for (int value : a.i) a.r.push_back(T((double)value));
      }
      std::vector<int64_t> answer;
      try {
        answer =
            builtin_shape_query(*query, function_argument_shape(e.args[0], a));
      } catch (const std::invalid_argument& error) {
        fail(e.name + ": " + error.what(), e.raw);
      }
      r.is_int = true;
      if (query->shape_query == BuiltinShapeQueryKind::Dims)
        r.dims = {(int64_t)answer.size()};
      for (int64_t v : answer) {
        r.i.push_back((int)v);
        r.r.push_back(T((double)v));
      }
      return r;
    }
    if (e.name == "FnLength" && e.args.size() == 1) {
      Value a = eval(e.args[0]);
      const long v = (long)std::max(a.r.size(), a.i.size());
      r.is_int = true;
      r.i = {(int)v};
      r.r = {T((double)v)};
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
  if (builtin != nullptr && builtin->opcode == code) {                    \
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

    // Every remaining registered real-result builtin executes through its
    // graph kernel: a registry entry plus a kernel is complete interpreter
    // support for a new function, with no branch here. The named branches
    // above are only allocation-free fast paths for the hot operators and
    // unaries; integer results were answered by the shared integer
    // evaluators earlier. Policy-shaped builtins resolve by name and arity
    // so that hand-built MIR without numeric metadata still dispatches.
    if (const BuiltinSpec* ctor = shaped_builtin_spec(
            e.name, e.args.size(), BuiltinShapePolicy::Constructor))
      return constructor_eval(e, *ctor);
    if (const BuiltinSpec* paired = shaped_builtin_spec(
            e.name, e.args.size(), BuiltinShapePolicy::PairedReduction))
      return paired_reduction_eval(e, *paired);
    if (const BuiltinSpec* grouped = shaped_builtin_spec(
            e.name, e.args.size(), BuiltinShapePolicy::GroupedReduction))
      return grouped_dot_eval(e, *grouped);
    if (const BuiltinSpec* matrix = shaped_builtin_spec(
            e.name, e.args.size(), BuiltinShapePolicy::MatrixOp))
      return matrix_op_eval(e, *matrix);
    // An RNG descriptor never reaches the pure-kernel bridge: draws carry
    // stream state the host hook owns, and outside a hook-equipped context
    // the call stays unsupported rather than invoking OP_RNG bare.
    const auto pure_kernel_shape = [](const BuiltinSpec& spec) {
      return spec.shape != BuiltinShapePolicy::Rng &&
             spec.shape != BuiltinShapePolicy::Product &&
             spec.shape != BuiltinShapePolicy::Solve;
    };
    const BuiltinSpec* kernel_builtin =
        builtin != nullptr && builtin->result == FunctionArgumentKind::Real &&
                pure_kernel_shape(*builtin)
            ? builtin
            : reduction_builtin_spec(e.name, e.args.size());
    const bool named_builtin =
        kernel_builtin == nullptr &&
        function_spec(e.name, e.args.size(), FunctionFamily::Builtin) !=
            nullptr;
    if (kernel_builtin != nullptr || named_builtin) {
      std::vector<Value> values;
      values.reserve(e.args.size());
      for (const mir::Expr& arg : e.args) values.push_back(eval(arg));
      if (kernel_builtin == nullptr) {
        // Hand-built MIR may carry no numeric metadata, so the typed
        // resolution above found nothing. Kind each argument from its
        // evaluated value and select the overload the metadata-carrying
        // path would have picked.
        uint64_t integer_arguments = 0;
        for (size_t k = 0; k < values.size(); ++k)
          if (values[k].is_int ||
              e.args[k].unsized.leaf == mir::UnsizedLeaf::Int)
            integer_arguments |= uint64_t{1} << k;
        const FunctionSpec* named =
            function_spec(e.name, e.args.size(), integer_arguments,
                          FunctionArgumentKind::Real);
        if (named != nullptr && named->builtin() != nullptr &&
            named->builtin()->result == FunctionArgumentKind::Real &&
            pure_kernel_shape(*named->builtin()))
          kernel_builtin = named->builtin();
      }
      if (kernel_builtin != nullptr)
        return builtin_kernel_eval(e, *kernel_builtin, std::move(values));
    }

    if (registered != nullptr && registered->density() != nullptr)
      return density_eval(e, *registered->density());
    // Hand-built MIR without numeric metadata resolves densities by name
    // and arity, the way the builtin tail above does; density_eval kinds
    // each argument from its evaluated value.
    if (registered == nullptr)
      if (const FunctionSpec* named_density =
              function_spec(e.name, e.args.size(), FunctionFamily::Density);
          named_density != nullptr && named_density->density() != nullptr)
        return density_eval(e, *named_density->density());

    if (e.name == "csr_extract_w" && e.args.size() == 1) {
      Value a = eval(e.args[0]);
      if (a.dims.size() != 2)
        fail("csr_extract_w: argument must be a matrix", e.raw);
      const int64_t rows = a.dims[0], cols = a.dims[1];
      using Mat = Eigen::Matrix<T, Eigen::Dynamic, Eigen::Dynamic>;
      Eigen::Map<const Mat> matrix(a.r.data(), rows, cols);
      const auto weights = stan::math::csr_extract_w(matrix);
      r.r.assign(weights.data(), weights.data() + weights.size());
      r.dims = {(int64_t)r.r.size()};
      return r;
    }
    // csr_extract_v (column indices) and csr_extract_u (row-start offsets)
    // are the integer companions to csr_extract_w. stan-math needs a
    // concrete (double) sparse view, so evaluate the value_of matrix.
    if ((e.name == "csr_extract_v" || e.name == "csr_extract_u") &&
        e.args.size() == 1) {
      Value a = eval(e.args[0]);
      if (a.dims.size() != 2)
        fail(e.name + ": argument must be a matrix", e.raw);
      Eigen::MatrixXd matrix(a.dims[0], a.dims[1]);
      for (int64_t k = 0; k < (int64_t)a.r.size(); ++k)
        matrix.data()[k] = val(a.r[(size_t)k]);
      const std::vector<int> idx = e.name == "csr_extract_v"
                                       ? stan::math::csr_extract_v(matrix)
                                       : stan::math::csr_extract_u(matrix);
      r.is_int = true;
      r.dims = {(int64_t)idx.size()};
      r.i.assign(idx.begin(), idx.end());
      r.r.reserve(idx.size());
      for (const int x : idx) r.r.push_back(T((double)x));
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

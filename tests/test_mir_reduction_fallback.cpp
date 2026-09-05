// MIR<double>/MIR<var> reductions, matrix algebra and discrete densities vs the
// graph kernel table.
#include <stanli/program.hpp>
#include <stanli/mir.hpp>
#include <stanli/mir_interp.hpp>
#include <stanli/optable.hpp>

#include <stan/math.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <type_traits>
#include <vector>

namespace {

using namespace stanli;

// The reference-side kernel invoker: a throwaway Program::Call over a
// local register file, formerly runtime kernel_bridge.hpp. The runtime
// dispatches through the registry's own bridge now, so this harness
// lives with the test that needs a registry-independent baseline.
template <typename T>
void call_kernel(uint16_t opcode, uint8_t variant, uint8_t input_adjoint_mask,
                 std::vector<int> idata,
                 const std::vector<const std::vector<T>*>& in,
                 std::vector<T>& out) {
  const Kernel* kernel = find_kernel(opcode);
  if (kernel == nullptr)
    throw std::logic_error(std::string("kernel_bridge: unavailable opcode ") +
                           opcode_name(opcode));
  Program::Call call;
  call.opcode = opcode;
  call.variant = variant;
  call.input_adjoint_mask = input_adjoint_mask;
  call.n_in = (int8_t)in.size();
  int32_t total = 0;
  for (size_t k = 0; k < in.size(); ++k) {
    call.in[k] = total;
    call.in_len[k] = (int32_t)in[k]->size();
    total += call.in_len[k];
  }
  call.out = total;
  call.out_len = (int32_t)out.size();
  total += call.out_len;
  call.idata = std::move(idata);
  const int64_t scratch = kernel_call_scratch(
      kernel->scratch_size, opcode, variant, call.n_in, call.in_len,
      call.out_len, call.idata.data(), (int64_t)call.idata.size(), nullptr);
  call.scratch = total;
  call.scratch_len = (int32_t)scratch;
  total += call.scratch_len;
  if (!bind_call(call))
    throw std::logic_error(std::string("kernel_bridge: unbound opcode ") +
                           opcode_name(opcode));

  std::vector<T> reg((size_t)total, T(0.0));
  for (size_t k = 0; k < in.size(); ++k)
    for (size_t i = 0; i < in[k]->size(); ++i)
      reg[(size_t)(call.in[k] + i)] = (*in[k])[i];

  if constexpr (std::is_same_v<T, double>) {
    run_call(call, reg.data(), nullptr);
  } else {
    run_call_var(call, reg.data());
  }
  for (int i = 0; i < call.out_len; ++i)
    out[(size_t)i] = reg[(size_t)(call.out + i)];
}

using stanli::MirInterp;
using stanli::mir::Expr;
using stanli::mir::FunDef;
using stanli::mir::Stmt;

int failures = 0;

template <typename T>
using Val = typename MirInterp<T>::Value;

// One test argument: a real/int scalar, vector or (with explicit dims) matrix.
struct Arg {
  bool is_int = false;
  std::vector<double> vals;
  std::vector<int64_t> dims;
};
Arg real(std::vector<double> v) {
  std::vector<int64_t> dims;
  if (v.size() != 1) dims = {(int64_t)v.size()};
  return {false, std::move(v), std::move(dims)};
}
Arg mat(std::vector<double> v, int64_t rows, int64_t cols) {
  return {false, std::move(v), {rows, cols}};
}
Arg ints(std::vector<int> v) {
  std::vector<int64_t> dims;
  if (v.size() != 1) dims = {(int64_t)v.size()};
  return {true, std::vector<double>(v.begin(), v.end()), std::move(dims)};
}

template <typename T>
Val<T> to_val(const Arg& a) {
  Val<T> v;
  v.r.assign(a.vals.begin(), a.vals.end());
  v.dims = a.dims;
  v.is_int = a.is_int;
  if (a.is_int) v.i.assign(a.vals.begin(), a.vals.end());
  return v;
}

// A fresh output Value of the same T as `a`'s elements, shaped as given.
template <typename VecOfVal>
auto out_like(VecOfVal& a, std::vector<int64_t> dims, size_t len) {
  std::decay_t<decltype(a[0])> r;
  r.dims = std::move(dims);
  r.r.resize(len);
  return r;
}

// Dispatches through call_kernel over the first n_in entries of `a`.
template <typename VecOfVal>
auto kernel(VecOfVal& a, size_t n_in, uint16_t opcode, uint8_t variant,
            std::vector<int> idata, std::vector<int64_t> out_dims,
            size_t out_len) {
  using T = typename decltype(a[0].r)::value_type;
  auto r = out_like(a, std::move(out_dims), out_len);
  std::vector<const std::vector<T>*> in;
  for (size_t k = 0; k < n_in; ++k) in.push_back(&a[k].r);
  call_kernel<T>(opcode, variant, 0x3f, idata, in, r.r);
  return r;
}

uint64_t ordered(double x) {
  uint64_t b;
  std::memcpy(&b, &x, sizeof(b));
  return (b >> 63) ? ~b : (b | (uint64_t(1) << 63));
}
bool close(double got, double want) {
  if (std::isnan(want)) return std::isnan(got);
  if (!std::isfinite(want) || !std::isfinite(got)) return got == want;
  const uint64_t g = ordered(got), w = ordered(want);
  return (g > w ? g - w : w - g) <= 2;
}
void expect_close(const std::string& what, double got, double want) {
  if (close(got, want)) return;
  ++failures;
  std::printf("FAIL %-40s got %.17g want %.17g\n", what.c_str(), got, want);
}
double sum_r(const std::vector<double>& r) {
  double s = 0;
  for (double x : r) s += x;
  return s;
}

// Runs MIR<double>/MIR<var> on name(args...) and compares value and gradient
// against expected(vals), a generic (std::vector<Val<T>>&) -> Val<T> lambda.
template <typename Expected>
void check(const std::string& name, std::vector<Arg> args, Expected expected) {
  FunDef f;
  f.name = "rhs";
  Expr call;
  call.kind = Expr::FunApp;
  call.name = name;
  call.type_ = "UReal";
  call.fn_lib = Expr::Lib::StanLib;
  for (size_t i = 0; i < args.size(); ++i) {
    const std::string nm = "a" + std::to_string(i);
    f.arg_names.push_back(nm);
    Expr v;
    v.kind = Expr::Var;
    v.name = nm;
    v.type_ = "UReal";
    call.args.push_back(v);
  }
  Stmt ret;
  ret.kind = Stmt::Return;
  ret.has_init = true;
  ret.rhs = std::move(call);
  f.body = {std::move(ret)};
  const std::map<std::string, const FunDef*> defs{{f.name, &f}};

  {
    std::vector<Val<double>> vals;
    for (auto& a : args) vals.push_back(to_val<double>(a));
    Val<double> ref = expected(vals);
    MirInterp<double> interp(defs, name + " double");
    auto out = interp.call(f, vals);
    expect_close(name + " MIR<double>", sum_r(out.r), sum_r(ref.r));
  }
  // Two independent nested scopes: grad() sweeps every vari created since
  // the scope began, so a leftover ref vari still on a shared stack would
  // get re-chained (and its adjoint re-accumulated) by the second grad().
  double ref_val;
  std::vector<std::vector<double>> ref_grad(args.size());
  {
    stan::math::nested_rev_autodiff nested;
    std::vector<Val<stan::math::var>> ref_vals;
    for (auto& a : args) ref_vals.push_back(to_val<stan::math::var>(a));
    Val<stan::math::var> ref = expected(ref_vals);
    stan::math::var ref_sum = 0;
    for (auto& x : ref.r) ref_sum += x;
    stan::math::grad(ref_sum.vi_);
    ref_val = ref_sum.val();
    for (size_t k = 0; k < args.size(); ++k) {
      ref_grad[k].resize(args[k].vals.size());
      for (size_t i = 0; i < args[k].vals.size(); ++i)
        ref_grad[k][i] = ref_vals[k].r[i].adj();
    }
  }
  {
    stan::math::nested_rev_autodiff nested;
    std::vector<Val<stan::math::var>> mir_vals;
    for (auto& a : args) mir_vals.push_back(to_val<stan::math::var>(a));
    MirInterp<stan::math::var> interp(defs, name + " var");
    auto out = interp.call(f, mir_vals);
    stan::math::var sum = 0;
    for (auto& x : out.r) sum += x;
    stan::math::grad(sum.vi_);
    expect_close(name + " MIR<var> value", sum.val(), ref_val);
    for (size_t k = 0; k < args.size(); ++k) {
      if (args[k].is_int) continue;
      for (size_t i = 0; i < args[k].vals.size(); ++i)
        expect_close(
            name + " MIR<var> g" + std::to_string(k) + "_" + std::to_string(i),
            mir_vals[k].r[i].adj(), ref_grad[k][i]);
    }
  }
}

// idata is the outcome (lpmf1) or outcome-and-trials (lpmf2) int group(s).
auto lpmf1(uint16_t opcode) {
  return [opcode](auto& a) {
    using T = typename decltype(a[0].r)::value_type;
    auto r = out_like(a, {}, 1);
    std::vector<const std::vector<T>*> in;
    for (size_t k = 1; k < a.size(); ++k) in.push_back(&a[k].r);
    const std::vector<int> idata(a[0].i.begin(), a[0].i.end());
    call_kernel<T>(opcode, 0, 0x3f, idata, in, r.r);
    return r;
  };
}
auto lpmf2(uint16_t opcode) {
  return [opcode](auto& a) {
    using T = typename decltype(a[0].r)::value_type;
    auto r = out_like(a, {}, 1);
    std::vector<int> idata{(int)a[0].i.size()};
    idata.insert(idata.end(), a[0].i.begin(), a[0].i.end());
    idata.push_back((int)a[1].i.size());
    idata.insert(idata.end(), a[1].i.begin(), a[1].i.end());
    const std::vector<const std::vector<T>*> in{&a[2].r};
    call_kernel<T>(opcode, 0, 0x3f, idata, in, r.r);
    return r;
  };
}

}  // namespace

int main() {
  using namespace stanli;

  const std::vector<double> xs{0.7, -1.3, 2.1, 0.4, -0.6};
  const std::vector<double> as{0.5, -1.2, 2.0, 0.3, 1.1};
  const std::vector<double> bs{1.5, 0.7, -0.4, 2.2, -0.8};

  check("sum", {real(xs)},
        [](auto& a) { return kernel(a, 1, OP_SUM_VEC, 0, {}, {}, 1); });
  check("mean", {real(xs)},
        [](auto& a) { return kernel(a, 1, OP_MEAN, 0, {}, {}, 1); });
  check("sd", {real(xs)},
        [](auto& a) { return kernel(a, 1, OP_SD, 0, {}, {}, 1); });
  check("variance", {real(xs)},
        [](auto& a) { return kernel(a, 1, OP_VARIANCE, 0, {}, {}, 1); });
  check("prod", {real(xs)}, [](auto& a) {
    using T = typename decltype(a[0].r)::value_type;
    const uint8_t variant = std::is_same_v<T, stan::math::var> ? 2u : 1u;
    return kernel(a, 1, OP_PROD_VEC, variant, {}, {}, 1);
  });
  check("log_sum_exp", {real(xs)},
        [](auto& a) { return kernel(a, 1, OP_LOG_SUM_EXP, 0, {}, {}, 1); });
  check("dot_product", {real(as), real(bs)},
        [](auto& a) { return kernel(a, 2, OP_DOT, 0, {}, {}, 1); });
  check("dot_self", {real(as)}, [](auto& a) {
    using T = typename decltype(a[0].r)::value_type;
    auto r = out_like(a, {}, 1);
    const std::vector<const std::vector<T>*> in{&a[0].r, &a[0].r};
    call_kernel<T>(OP_DOT, 0, 0x3f, {}, in, r.r);
    return r;
  });
  check("squared_distance", {real(as), real(bs)}, [](auto& a) {
    using T = typename decltype(a[0].r)::value_type;
    auto diff = out_like(a, {}, a[0].r.size());
    const std::vector<const std::vector<T>*> sub_in{&a[0].r, &a[1].r};
    call_kernel<T>(OP_SUB, 0, 0x3f, {}, sub_in, diff.r);
    auto r = out_like(a, {}, 1);
    const std::vector<const std::vector<T>*> dot_in{&diff.r, &diff.r};
    call_kernel<T>(OP_DOT, 0, 0x3f, {}, dot_in, r.r);
    return r;
  });

  // spd: symmetric positive definite 3x3. gen: general invertible 3x3.
  const std::vector<double> spd{4, 1, 0, 1, 3, 1, 0, 1, 2};
  const std::vector<double> gen{2, 1, 0, 0, 3, 1, 1, 0, 2};
  const std::vector<double> rect{1, 2, 3, 0, 1, 1};
  const std::vector<double> v3{0.5, -0.3, 0.8};
  const std::vector<double> d2{0.3, -0.2};

  check("crossprod", {mat(rect, 3, 2)}, [](auto& a) {
    using T = typename decltype(a[0].r)::value_type;
    const uint8_t variant = std::is_same_v<T, stan::math::var> ? 1u : 0u;
    return kernel(a, 1, OP_CROSSPROD, variant, {3, 2}, {2, 2}, 4);
  });
  check("multiply_lower_tri_self_transpose", {mat(rect, 3, 2)}, [](auto& a) {
    using T = typename decltype(a[0].r)::value_type;
    const uint8_t variant = std::is_same_v<T, stan::math::var> ? 1u : 0u;
    return kernel(a, 1, OP_MULT_LOWER_TRI_SELF_TRANSPOSE, variant, {3, 2},
                  {3, 3}, 9);
  });
  check("tcrossprod", {mat(rect, 3, 2)}, [](auto& a) {
    using T = typename decltype(a[0].r)::value_type;
    auto transpose = out_like(a, {2, 3}, 6);
    const std::vector<const std::vector<T>*> t_in{&a[0].r};
    call_kernel<T>(OP_TRANSPOSE, 0, 0x3f, {3, 2}, t_in, transpose.r);
    auto r = out_like(a, {3, 3}, 9);
    const std::vector<const std::vector<T>*> g_in{&a[0].r, &transpose.r};
    call_kernel<T>(OP_GEMM, 0, 0x3f, {3, 2, 3}, g_in, r.r);
    return r;
  });
  check("cholesky_decompose", {mat(spd, 3, 3)},
        [](auto& a) { return kernel(a, 1, OP_CHOLESKY, 0, {3}, {3, 3}, 9); });
  check("inverse", {mat(gen, 3, 3)},
        [](auto& a) { return kernel(a, 1, OP_INVERSE, 0, {3}, {3, 3}, 9); });
  check("inverse_spd", {mat(spd, 3, 3)}, [](auto& a) {
    using T = typename decltype(a[0].r)::value_type;
    const uint8_t variant = std::is_same_v<T, stan::math::var> ? 1u : 0u;
    return kernel(a, 1, OP_INVERSE_SPD, variant, {3}, {3, 3}, 9);
  });
  check("log_determinant", {mat(gen, 3, 3)}, [](auto& a) {
    return kernel(a, 1, OP_LOG_DETERMINANT, 0, {3}, {}, 1);
  });
  check("matrix_exp", {mat(gen, 3, 3)},
        [](auto& a) { return kernel(a, 1, OP_MATRIX_EXP, 0, {3}, {3, 3}, 9); });
  check("diag_matrix", {real(v3)},
        [](auto& a) { return kernel(a, 1, OP_DIAG_MATRIX, 0, {}, {3, 3}, 9); });
  check("quad_form", {mat(spd, 3, 3), real(v3)}, [](auto& a) {
    using T = typename decltype(a[0].r)::value_type;
    const uint8_t variant =
        (uint8_t)(1u | (std::is_same_v<T, stan::math::var> ? 2u : 0u));
    return kernel(a, 2, OP_QUAD_FORM, variant, {3, 1}, {}, 1);
  });
  check("quad_form_sym", {mat(spd, 3, 3), real(v3)}, [](auto& a) {
    using T = typename decltype(a[0].r)::value_type;
    const uint8_t variant =
        (uint8_t)(1u | (std::is_same_v<T, stan::math::var> ? 2u : 0u));
    return kernel(a, 2, OP_QUAD_FORM_SYM, variant, {3, 1}, {}, 1);
  });
  check("add_diag", {mat(rect, 3, 2), real(d2)}, [](auto& a) {
    return kernel(a, 2, OP_ADD_DIAG, 0, {3, 2}, {3, 2}, 6);
  });
  check("multiply", {mat(rect, 3, 2), real(d2)}, [](auto& a) {
    using T = typename decltype(a[0].r)::value_type;
    auto r = out_like(a, {3}, 3);
    const std::vector<const std::vector<T>*> in{&a[0].r, &a[1].r};
    if constexpr (std::is_same_v<T, double>) {
      call_kernel<T>(OP_MATVEC, 0, 0x3f, {3, 2}, in, r.r);
    } else {
      call_kernel<T>(OP_GEMM, 0, 0x3f, {3, 2, 1}, in, r.r);
    }
    return r;
  });
  check("fma", {real(as), real(bs), real(xs)},
        [](auto& a) { return kernel(a, 3, OP_FMA, 0, {}, {}, a[0].r.size()); });
  check("gp_exp_quad_cov", {real(xs), real({1.3}), real({0.9})}, [](auto& a) {
    const int64_t n = (int64_t)a[0].r.size();
    return kernel(a, 3, OP_GP_COV, kGpExpQuad, {(int)n, 1}, {n, n},
                  (size_t)(n * n));
  });

  check("bernoulli_lpmf",
        {ints({0, 1, 1, 0, 1}), real({0.3, 0.6, 0.7, 0.4, 0.5})},
        lpmf1(OP_BERNOULLI_LPMF));
  check("bernoulli_logit_lpmf",
        {ints({0, 1, 1, 0, 1}), real({0.2, -0.5, 1.1, -0.3, 0.7})},
        lpmf1(OP_BERNOULLI_LOGIT_LPMF));
  check("poisson_lpmf",
        {ints({0, 2, 1, 3, 0}), real({1.2, 0.5, 2.0, 0.8, 1.5})},
        lpmf1(OP_POISSON_LPMF));
  check("poisson_log_lpmf",
        {ints({0, 2, 1, 3, 0}), real({0.1, -0.2, 0.3, 0.05, -0.1})},
        lpmf1(OP_POISSON_LOG_LPMF));
  check("neg_binomial_2_lpmf",
        {ints({0, 2, 1, 3, 0}), real({1.1, 0.7, 2.0, 0.9, 1.3}),
         real({2.0, 1.5, 3.0, 1.0, 2.5})},
        lpmf1(OP_NEG_BINOMIAL_2_LPMF));
  check("neg_binomial_2_log_lpmf",
        {ints({0, 2, 1, 3, 0}), real({0.2, -0.1, 0.4, 0.1, -0.2}),
         real({2.0, 1.5, 3.0, 1.0, 2.5})},
        lpmf1(OP_NEG_BINOMIAL_2_LOG_LPMF));
  check("binomial_lpmf",
        {ints({1, 0, 2, 1, 3}), ints({3, 2, 4, 2, 5}),
         real({0.3, 0.4, 0.5, 0.6, 0.35})},
        lpmf2(OP_BINOMIAL_LPMF));
  check("binomial_logit_lpmf",
        {ints({1, 0, 2, 1, 3}), ints({3, 2, 4, 2, 5}),
         real({-0.2, 0.1, 0.4, 0.3, -0.1})},
        lpmf2(OP_BINOMIAL_LOGIT_LPMF));

  if (failures == 0)
    std::printf("test_mir_reduction_fallback: all cases passed\n");
  return failures == 0 ? 0 : 1;
}

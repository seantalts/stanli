// MIR<double>/MIR<var> binary builtins vs the graph kernel table.
#include "graph_helpers.hpp"

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
#include <vector>

namespace {

using stanli::MirInterp;
using stanli::mir::Expr;
using stanli::mir::FunDef;
using stanli::mir::Stmt;

int failures = 0;

template <typename T>
using Val = typename MirInterp<T>::Value;

Expr variable(std::string name) {
  Expr e;
  e.kind = Expr::Var;
  e.name = std::move(name);
  e.type_ = "UReal";
  return e;
}

Expr call_expr(std::string name, std::vector<Expr> args) {
  Expr e;
  e.kind = Expr::FunApp;
  e.name = std::move(name);
  e.args = std::move(args);
  e.type_ = "UReal";
  e.fn_lib = Expr::Lib::StanLib;
  return e;
}

Stmt returning(Expr value) {
  Stmt s;
  s.kind = Stmt::Return;
  s.has_init = true;
  s.rhs = std::move(value);
  return s;
}

FunDef binary_fun(const std::string& name) {
  FunDef f;
  f.name = "binary_rhs";
  f.arg_names = {"a", "b"};
  f.body = {returning(call_expr(name, {variable("a"), variable("b")}))};
  return f;
}

template <typename T>
Val<T> real_val(const std::vector<double>& xs) {
  Val<T> v;
  v.r.assign(xs.begin(), xs.end());
  if (xs.size() != 1) v.dims = {(int64_t)xs.size()};
  return v;
}

template <typename T>
Val<T> int_val(const std::vector<int>& xs) {
  Val<T> v;
  v.is_int = true;
  v.i = xs;
  v.r.assign(xs.begin(), xs.end());
  if (xs.size() != 1) v.dims = {(int64_t)xs.size()};
  return v;
}

uint64_t bits(double x) {
  uint64_t out;
  std::memcpy(&out, &x, sizeof(out));
  return out;
}

uint64_t ordered(double x) {
  const uint64_t b = bits(x);
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
  std::printf("FAIL %-32s got %.17g want %.17g\n", what.c_str(), got, want);
}

void check_shape(const std::string& tag, const std::string& name,
                 uint16_t opcode, const std::vector<double>& av,
                 const std::vector<double>& bv) {
  const int64_t n = std::max((int64_t)av.size(), (int64_t)bv.size());
  const auto ref =
      stanli::testutil::run_op_sum(opcode, n, {av, bv}, {true, true});
  FunDef f = binary_fun(name);
  const std::map<std::string, const FunDef*> defs{{f.name, &f}};

  {
    MirInterp<double> interp(defs, tag + " double");
    auto out = interp.call(f, {real_val<double>(av), real_val<double>(bv)});
    double sum = 0;
    for (double x : out.r) sum += x;
    expect_close(tag + " MIR<double>", sum, ref.value);
  }
  {
    stan::math::nested_rev_autodiff nested;
    MirInterp<stan::math::var> interp(defs, tag + " var");
    auto va = real_val<stan::math::var>(av);
    auto vb = real_val<stan::math::var>(bv);
    auto out = interp.call(f, {va, vb});
    stan::math::var sum = 0;
    for (auto& x : out.r) sum += x;
    stan::math::grad(sum.vi_);
    expect_close(tag + " MIR<var> value", sum.val(), ref.value);
    for (size_t i = 0; i < va.r.size(); ++i)
      expect_close(tag + " MIR<var> ga" + std::to_string(i), va.r[i].adj(),
                   ref.grad[i]);
    for (size_t i = 0; i < vb.r.size(); ++i)
      expect_close(tag + " MIR<var> gb" + std::to_string(i), vb.r[i].adj(),
                   ref.grad[av.size() + i]);
  }
}

void check_binary(const std::string& name, uint16_t opcode,
                  const std::vector<double>& av,
                  const std::vector<double>& bv) {
  check_shape(name + " vv", name, opcode, av, bv);
  check_shape(name + " vs", name, opcode, av, {bv[0]});
  check_shape(name + " sv", name, opcode, {av[0]}, bv);
  check_shape(name + " ss", name, opcode, {av[0]}, {bv[0]});
}

template <bool IntFirst>
void check_int_shape(const std::string& tag, const std::string& name,
                     uint16_t opcode, const std::vector<double>& rv,
                     const std::vector<int>& iv) {
  const int64_t n = std::max((int64_t)rv.size(), (int64_t)iv.size());
  const std::vector<double> idbl(iv.begin(), iv.end());
  const auto ref =
      IntFirst
          ? stanli::testutil::run_op_sum(opcode, n, {idbl, rv}, {false, true})
          : stanli::testutil::run_op_sum(opcode, n, {rv, idbl}, {true, false});
  FunDef f = binary_fun(name);
  const std::map<std::string, const FunDef*> defs{{f.name, &f}};

  {
    MirInterp<double> interp(defs, tag + " double");
    auto vi = int_val<double>(iv);
    auto vr = real_val<double>(rv);
    auto out = interp.call(f, IntFirst ? std::vector<Val<double>>{vi, vr}
                                       : std::vector<Val<double>>{vr, vi});
    double sum = 0;
    for (double x : out.r) sum += x;
    expect_close(tag + " MIR<double>", sum, ref.value);
  }
  {
    stan::math::nested_rev_autodiff nested;
    MirInterp<stan::math::var> interp(defs, tag + " var");
    auto vi = int_val<stan::math::var>(iv);
    auto vr = real_val<stan::math::var>(rv);
    auto out =
        interp.call(f, IntFirst ? std::vector<Val<stan::math::var>>{vi, vr}
                                : std::vector<Val<stan::math::var>>{vr, vi});
    stan::math::var sum = 0;
    for (auto& x : out.r) sum += x;
    stan::math::grad(sum.vi_);
    expect_close(tag + " MIR<var> value", sum.val(), ref.value);
    for (size_t i = 0; i < vr.r.size(); ++i)
      expect_close(tag + " MIR<var> gr" + std::to_string(i), vr.r[i].adj(),
                   ref.grad[i]);
  }
}

template <bool IntFirst>
void check_int_binary(const std::string& name, uint16_t opcode,
                      const std::vector<double>& rv,
                      const std::vector<int>& iv) {
  check_int_shape<IntFirst>(name + " vv", name, opcode, rv, iv);
  check_int_shape<IntFirst>(name + " vs", name, opcode, rv, {iv[0]});
  check_int_shape<IntFirst>(name + " sv", name, opcode, {rv[0]}, iv);
  check_int_shape<IntFirst>(name + " ss", name, opcode, {rv[0]}, {iv[0]});
}

}  // namespace

int main() {
  using namespace stanli;

  // Free-sign pairs.
  const std::vector<double> xs{0.5, -1.2, 2.0, 0.3};
  const std::vector<double> ys{1.5, 0.7, -0.4, 2.2};
  // Positive pairs, for the log-domain functions.
  const std::vector<double> ps{0.9, 1.7, 0.35, 2.4};
  const std::vector<double> qs{1.1, 0.6, 2.2, 0.8};
  // n >= k >= 0, for lchoose.
  const std::vector<double> ns{7.5, 4.0, 9.25, 6.0};
  const std::vector<double> ks{2.5, 1.0, 3.0, 0.5};
  // a > b elementwise AND against each other's first element, so every
  // broadcast shape stays inside log_inv_logit_diff/log_diff_exp's support.
  const std::vector<double> hi{1.5, 0.7, 2.0, 2.2};
  const std::vector<double> lo{0.5, -1.2, -0.4, 0.3};

  check_binary("atan2", OP_ATAN2, xs, ys);
  check_binary("beta", OP_BETA_FN, ps, qs);
  check_binary("fdim", OP_FDIM, xs, ys);
  check_binary("fmax", OP_FMAX, xs, ys);
  check_binary("fmin", OP_FMIN, xs, ys);
  check_binary("fmod", OP_FMOD, xs, ys);
  check_binary("gamma_p", OP_GAMMA_P, ps, qs);
  check_binary("gamma_q", OP_GAMMA_Q, ps, qs);
  check_binary("hypot", OP_HYPOT, xs, ys);
  check_binary("hypergeometric_1F0", OP_HYPERGEOMETRIC_1F0, xs,
               {0.1, -0.5, 0.25, 0.75});
  check_binary("lbeta", OP_LBETA, ps, qs);
  check_binary("lchoose", OP_LCHOOSE, ns, ks);
  check_binary("binomial_coefficient_log", OP_LCHOOSE, ns, ks);
  check_binary("lmultiply", OP_LMULTIPLY, xs, qs);
  check_binary("multiply_log", OP_LMULTIPLY, xs, qs);
  check_binary("log_falling_factorial", OP_LOG_FALLING_FACTORIAL, ns, ks);
  check_binary("log_inv_logit_diff", OP_LOG_INV_LOGIT_DIFF, hi, lo);
  check_binary("log_modified_bessel_first_kind", OP_LOG_MODIFIED_BESSEL_1, ps,
               qs);
  check_binary("log_rising_factorial", OP_LOG_RISING_FACTORIAL, ps, qs);
  check_binary("owens_t", OP_OWENS_T, xs, ys);
  check_binary("log_sum_exp", OP_LSE2, xs, ys);
  check_binary("log_diff_exp", OP_LOG_DIFF_EXP, hi, lo);
  check_binary("pow", OP_POW, ps, ys);
  check_binary("add", OP_ADD, xs, ys);
  check_binary("subtract", OP_SUB, xs, ys);
  check_binary("divide", OP_DIV, xs, qs);
  check_binary("elt_multiply", OP_MUL, xs, ys);
  check_binary("elt_divide", OP_DIV, xs, qs);

  const std::vector<int> orders{0, 1, 2, 3};
  check_int_binary<true>("bessel_first_kind", OP_BESSEL_1, xs, orders);
  check_int_binary<true>("bessel_second_kind", OP_BESSEL_2, ps, orders);
  check_int_binary<true>("modified_bessel_first_kind", OP_MODIFIED_BESSEL_1, xs,
                         orders);
  check_int_binary<true>("modified_bessel_second_kind", OP_MODIFIED_BESSEL_2,
                         ps, orders);
  check_int_binary<true>("binary_log_loss", OP_BINARY_LOG_LOSS,
                         {0.2, 0.6, 0.35, 0.8}, {0, 1, 1, 0});
  check_int_binary<true>("lmgamma", OP_LMGAMMA, {0.9, 1.7, 1.35, 2.4},
                         {1, 2, 1, 2});

  const std::vector<int> counts{0, 1, 2, 3};
  check_int_binary<false>("falling_factorial", OP_FALLING_FACTORIAL, ps,
                          counts);
  check_int_binary<false>("rising_factorial", OP_RISING_FACTORIAL, ps, counts);
  check_int_binary<false>("ldexp", OP_LDEXP, xs, {-2, 0, 1, 5});

  if (failures == 0)
    std::printf("test_mir_binary_fallback: all cases passed\n");
  return failures == 0 ? 0 : 1;
}

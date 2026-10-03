#ifndef STANLI_ADJOINT_RULES_HPP
#define STANLI_ADJOINT_RULES_HPP

#include <stanli/optable.hpp>

#include <stan/math.hpp>

#include <cmath>
#include <cstdint>
#include <limits>

namespace stanli {

// The rules with more to them than one expression, shared by the scalar
// sweep and the ranged one. `t` is the output adjoint, already consumed
// from its cell.
inline void pow_rule(uint8_t law, double t, double va, double vb, double vd,
                     double& adj_a, double& adj_b) {
  if (va == 0.0) {
    adj_a += pow_zero_base_partial(law, t, va, vb);
    return;
  }
  const double m = t * vd;
  adj_a += m * vb / va;
  adj_b += m * std::log(va);
}

// fmax/fmin build no node at all: they return whichever operand won,
// so the whole adjoint routes to it. Which operand wins a tie is an
// instantiation property: the var,var overloads compare `a > b`
// (ties to b) where var,double compares `a >= b` (ties to the var),
// and a mixed call whose constant side wins returns a fresh constant
// that carries no adjoint at all. `law` holds the operands' activity
// from lowering (bit 0: a, bit 1: b; 0 is the legacy all-var form),
// matching program_extremum's replay. NaN needs saying separately --
// `a > b` is false when either is NaN, so the plain comparison would
// hand fmax(x, NaN) to the NaN, where stan-math returns x. A local
// declared and never assigned is NaN (mir_prog.hpp), so this is
// reachable and not hypothetical.
inline void extremum_rule(bool maximum, uint8_t law, double t, double x,
                          double y, double& adj_a, double& adj_b) {
  const bool a_active = law == 0 || (law & 0x1u) != 0;
  const bool b_active = law == 0 || (law & 0x2u) != 0;
  if (std::isnan(x) && std::isnan(y)) {
    if (a_active) adj_a = std::numeric_limits<double>::quiet_NaN();
    if (b_active) adj_b = std::numeric_limits<double>::quiet_NaN();
  } else if (std::isnan(y)) {
    if (a_active) adj_a += t;
  } else if (std::isnan(x)) {
    if (b_active) adj_b += t;
  } else {
    const bool a_wins = a_active && !b_active ? (maximum ? x >= y : x <= y)
                                              : (maximum ? x > y : x < y);
    if (a_wins) {
      if (a_active) adj_a += t;
    } else if (b_active) {
      adj_b += t;
    }
  }
}

// At exactly zero stan-math returns a fresh node with no operand, so the
// derivative is dropped rather than being either sign; at NaN it poisons
// the operand's adjoint outright, which is what makes a sampler reject the
// draw rather than accept a finite gradient computed from nothing.
inline void fabs_rule(double t, double x, double& adj_a) {
  if (std::isnan(x))
    adj_a = std::numeric_limits<double>::quiet_NaN();
  else if (x > 0.0)
    adj_a += t;
  else if (x < 0.0)
    adj_a -= t;
}

inline void lse2_rule(double t, double va, double vb, double& adj_a,
                      double& adj_b) {
  adj_a += t * stan::math::inv_logit(va - vb);
  adj_b += t * stan::math::inv_logit(vb - va);
}

// Match rev/fun/log_diff_exp.hpp exactly. Besides being stable when the
// arguments are close, expm1 has observably different rounding from
// spelling either denominator with exp.
inline void log_diff_exp_rule(double t, double va, double vb, double& adj_a,
                              double& adj_b) {
  adj_a -= t / stan::math::expm1(vb - va);
  adj_b -= t / stan::math::expm1(va - vb);
}

// rev/fun/log_mix.hpp: partials through the helper, with the arms swapped
// when lambda1 <= lambda2 so the exponential cannot overflow. Transcribed
// rather than reused because log_mix's partials live in the rev overload,
// which rvar cannot select.
inline void log_mix_rule(double t, double va, double vb, double vc,
                         double& adj_a, double& adj_b, double& adj_c) {
  double theta_d = va;
  const double lam1 = vb, lam2 = vc;
  double one_m_exp, one_m_t_prod, one_d;
  auto helper = [&](double th, double la, double lb) {
    const double e = std::exp(lb - la);
    one_m_exp = 1.0 - e;
    const double one_m_t = 1.0 - th;
    one_m_t_prod = one_m_t * e;
    one_d = 1.0 / (th + one_m_t_prod);
  };
  if (lam1 > lam2) {
    helper(theta_d, lam1, lam2);
  } else {
    helper(1.0 - theta_d, lam2, lam1);
    one_m_exp = -one_m_exp;
    const double swapped = one_m_t_prod;
    one_m_t_prod = 1.0 - theta_d;
    theta_d = swapped;
  }
  // Descending operand order, as the propagator's per-edge tape entries
  // unwind.
  adj_c += t * (one_m_t_prod * one_d);
  adj_b += t * (theta_d * one_d);
  adj_a += t * (one_m_exp * one_d);
}

template <int Opcode>
struct UnaryRule;

#define STANLI_DEFINE_UNARY_RULE(code, name, VAL, DELTA, TOPOLOGY)           \
  template <>                                                                \
  struct UnaryRule<code> {                                                   \
    static constexpr UnaryTopology topology = TOPOLOGY;                      \
    static double delta(double x, double y, double seed) { return (DELTA); } \
  };
STANLI_SCALAR_UNARY_LIST(STANLI_DEFINE_UNARY_RULE)
#undef STANLI_DEFINE_UNARY_RULE

#define STANLI_NATIVE_MATH_LIST(X) \
  X(ERFC, OP_ERFC)                 \
  X(LOG1P, OP_LOG1P)               \
  X(LOG1M_EXP, OP_LOG1M_EXP)       \
  X(INV_SQUARE, OP_INV_SQUARE)

}  // namespace stanli

#endif

#ifndef STANLI_TOOLS_LOOP_ADJOINT_CHECKS_HPP
#define STANLI_TOOLS_LOOP_ADJOINT_CHECKS_HPP
#include "loop_adjoint_probe.hpp"
#include <stan/math.hpp>
#include <cstring>

// Program-level adversarial checks, separate from the MIR/solver comparisons.
// Copies must share adjoint cells across visits; overwritten values must not.
inline size_t loop_adjoint_checks() {
  using P = stanli::Program;
  const auto exact = [](double a, double b) {
    if (std::memcmp(&a, &b, sizeof(double)))
      throw std::runtime_error("loop history bitwise mismatch");
  };
  const auto refuses = [](auto&& f) {
    bool threw = false;
    try {
      f();
    } catch (const std::exception&) {
      threw = true;
    }
    if (!threw) throw std::runtime_error("loop probe failed to refuse");
  };
  P p;
  p.n_regs = 9;
  p.pool = {0, 1};
  p.out_regs = {5, 8, 5};
  p.code = {{P::CONST, 3, 0},  {P::CONST, 4, 1},   {P::MOV, 5, 0},
            {P::MOV, 8, 0},    {P::LT, 6, 3, 2},   {P::JZ, 15, 6},
            {P::EQ, 6, 3, 4},  {P::JZ, 10, 6},     {P::MOV, 7, 5},
            {P::JMP, 11},      {P::MUL, 7, 5, 1},  {P::MOV, 8, 5},
            {P::ADD, 5, 7, 8}, {P::IADD, 3, 3, 4}, {P::JMP, 4}};
  LoopAdjointProbe plan(p, {0, 1, 2});
  LoopAdjointProbe::Workspace w;
  size_t checked = 0;
  for (double n : {0., 1., 2., 17., 3., 16., 0.})
    for (double x : {-0.7, 0., 0.31})
      for (double rate : {-0.3, 1.2}) {
        std::vector<double> input(p.n_regs), gradient;
        input[0] = x;
        input[1] = rate;
        input[2] = n;
        plan.forward(input, w);
        auto copy = w;
        for (const auto& seed : {std::vector<double>{1, -0.3, 2.75},
                                 std::vector<double>{0, 1, 0}}) {
          stan::math::nested_rev_autodiff nested;
          std::vector<stan::math::var> reg(p.n_regs);
          stan::math::var xv = x, rv = rate, nv = n;
          reg[0] = xv;
          reg[1] = rv;
          reg[2] = nv;
          stanli::run_program(p, reg);
          for (size_t i = 0; i < seed.size(); ++i)
            reg[p.out_regs[i]].adj() += seed[i];
          stan::math::grad();
          plan.reverse(copy, seed, gradient);
          exact(gradient[0], xv.adj());
          exact(gradient[1], rv.adj());
          exact(gradient[2], nv.adj());
          for (size_t i = 0; i < seed.size(); ++i)
            exact(plan.output(copy, i), reg[p.out_regs[i]].val());
          ++checked;
        }
      }
  // Constructor refusal must not mutate the supplied canonical program.
  for (auto op : {P::CALL, P::FMAX, P::FMIN, P::LOG_DIFF_EXP, P::REJECT}) {
    P invalid = p;
    invalid.code[0].code = op;
    refuses([&] { LoopAdjointProbe probe(invalid, {0, 1, 2}); });
    if (invalid.code[0].code != op)
      throw std::runtime_error("refusal mutated input");
  }
  P invalid = p;
  invalid.code[5].dst = 999;
  refuses([&] { LoopAdjointProbe probe(invalid, {0, 1, 2}); });
  invalid = p;
  invalid.code[0].a = 999;
  refuses([&] { LoopAdjointProbe probe(invalid, {0, 1, 2}); });
  // A failed forward invalidates an earlier success and can then recover.
  P partial;
  partial.n_regs = 2;
  partial.pool = {1};
  partial.out_regs = {1};
  partial.code = {{P::JZ, 2, 0}, {P::CONST, 1, 0}};
  LoopAdjointProbe guarded(partial, {0});
  std::vector<double> gradient;
  guarded.forward({1, 0}, w);
  refuses([&] { guarded.forward({0, 0}, w); });
  refuses([&] { guarded.reverse(w, {1}, gradient); });
  guarded.forward({1, 0}, w);
  guarded.reverse(w, {1}, gradient);
  exact(guarded.output(w, 0), 1);
  exact(gradient[0], 0);
  return checked;
}
#endif

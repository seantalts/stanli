#include <stanli/island.hpp>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
using namespace stanli;
bool same(double a, double b) {
  return (std::isnan(a) && std::isnan(b)) || std::memcmp(&a, &b, 8) == 0;
}
int constants(const Program& p) {
  int n = 0;
  for (auto& i : p.code) n += i.code == Program::CONST;
  return n;
}
IslandProg make(double v) {
  IslandProg p;
  p.n_regs = 6;
  p.pool = {v, v};
  p.ins = {{0, 1}};
  p.out_regs = {5};
  p.code = {{Program::CONST, 1, 0},
            {Program::CONST, 2, 1},
            {Program::MUL, 3, 0, 1},
            {Program::MUL, 4, 0, 2},
            {Program::ADD, 5, 3, 4}};
  return p;
}
std::array<double, 3> evaluate(IslandProg p, double x, double seed) {
  if (!gen_adjoint(p)) throw std::runtime_error("adjoint refused");
  std::vector<double> r(p.n_regs), adj(p.adj.n_regs);
  r[p.ins[0].reg] = x;
  run_program(p, r);
  adj[p.adj.adj_reg[p.out_regs[0]]] = seed;
  run_adjoint(p, p.adj, r.data(), adj.data());
  double grad = adj[p.adj.adj_reg[p.ins[0].reg]];
  stan::math::nested_rev_autodiff nested;
  std::vector<stan::math::var> vr(p.n_regs);
  stan::math::var in = x;
  vr[p.ins[0].reg] = in;
  run_program(p, vr);
  stan::math::var obj = vr[p.out_regs[0]] * seed;
  obj.grad();
  return {r[p.out_regs[0]], grad, in.adj()};
}
int main() {
  int errors = 0, cases = 0;
  double nan;
  uint64_t bits = 0x7ff8000000000021ULL;
  std::memcpy(&nan, &bits, 8);
  for (double v :
       {0., -0., 1., -2.5, 1e100, std::numeric_limits<double>::infinity(), nan})
    for (double x : {0., -0., .7, -3., 1e-100,
                     std::numeric_limits<double>::infinity(), nan})
      for (double seed : {0., -0., 1., -.75, 2.25}) {
        auto p = make(v), q = p;
        std::vector<std::pair<int, int>> inputs{{0, 1}};
        compact_program_gated(q, inputs, false);
        q.ins[0].reg = inputs[0].first;
        if (constants(q) != 1) ++errors;
        auto a = evaluate(p, x, seed), b = evaluate(q, x, seed);
        for (int k = 0; k < 3; ++k)
          if (!same(a[k], b[k])) ++errors;
        ++cases;
      }
  for (int width : {2, 3, 8, 17, 64, 128})
    for (int pattern = 0; pattern < 8; ++pattern) {
      IslandProg p;
      p.n_regs = 3 * width;
      p.ins = {{0, 1}};
      for (int k = 0; k < width; ++k) {
        uint64_t bits;
        double value = (k % 7 - 3) * .125;
        if (pattern == 1) value = k % 2 ? 0. : -0.;
        if (pattern == 2) value = k % 2 ? 1e100 : -1e100;
        if (pattern == 3)
          value = k % 2 ? std::numeric_limits<double>::infinity()
                        : -std::numeric_limits<double>::infinity();
        if (pattern >= 4) {
          bits = UINT64_C(0x7ff8000000000020) + (k % pattern);
          std::memcpy(&value, &bits, 8);
        }
        p.pool.push_back(value);
        p.code.emplace_back(Program::CONST, 1 + k, k);
      }
      for (int k = 0; k < width; ++k)
        p.code.emplace_back(Program::MUL, 1 + width + k, 0, 1 + k);
      int result = 1 + width;
      for (int k = 1; k < width; ++k) {
        p.code.emplace_back(Program::ADD, 2 * width + k, result, 1 + width + k);
        result = 2 * width + k;
      }
      p.out_regs = {result};
      auto q = p;
      std::vector<std::pair<int, int>> inputs{{0, 1}};
      compact_program_gated(q, inputs, false);
      q.ins[0].reg = inputs[0].first;
      for (double seed : {0., 1., -.75}) {
        auto a = evaluate(p, .7, seed), b = evaluate(q, .7, seed);
        for (int k = 0; k < 3; ++k)
          if (!same(a[k], b[k])) ++errors;
        ++cases;
      }
    }
  for (int mode = 0; mode < 8; ++mode) {
    auto p = make(-0.);
    std::vector<std::pair<int, int>> inputs{{0, 1}};
    if (mode == 0) p.out_regs.push_back(2);
    if (mode == 1) inputs.push_back({2, 1});
    if (mode == 2) p.code.push_back({Program::CONST, 2, 0});
    if (mode == 3) p.code.insert(p.code.begin(), {Program::MOV, 3, 2});
    if (mode == 4) p.code.push_back({Program::DOT, 3, 1, 1, 0, 2});
    if (mode == 5) {
      p.pool.push_back(0.);
      p.code[1].a = 2;
    }
    if (mode == 6) {
      p.pool.resize(20);
    }
    if (mode == 7) p.code.push_back({Program::JZ, 0, 0, 0, 0, 0});
    compact_program_gated(p, inputs, false);
    if (constants(p) != (mode == 2 ? 3 : mode == 6 ? 1 : 2)) {
      std::printf("refusal mode %d CONST=%d\n", mode, constants(p));
      ++errors;
    }
    ++cases;
  }
  std::printf("cases=%d errors=%d\n", cases, errors);
  return errors ? 1 : 0;
}

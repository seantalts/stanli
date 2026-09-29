// Fixed-shape, one-state/one-theta callback experiment. Compare the canonical
// register/Stan autodiff oracle with generated reverse over loop history.
#include "loop_adjoint_probe.hpp"
#include "loop_adjoint_checks.hpp"
#include <stanli/ode_prog.hpp>
#include <stanli/sexp.hpp>
#include <stan/math.hpp>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace {
using Clock = std::chrono::steady_clock;
volatile double sink = 0;
std::string slurp(const char* path) {
  std::ifstream f(path);
  if (!f) throw std::runtime_error("cannot open input");
  std::ostringstream s;
  s << f.rdbuf();
  return s.str();
}
uint64_t bits(double x) {
  uint64_t b;
  std::memcpy(&b, &x, sizeof(b));
  return b;
}
struct Result {
  double value, gradient[3];
};
Result oracle(const stanli::RhsProgram& p, double t, double y, double theta,
              double weight) {
  stan::math::nested_rev_autodiff nested;
  stan::math::var tv = t, yv = y, th = theta;
  std::vector<stan::math::var> reg, out;
  stanli::run_rhs(p, tv, &yv, &th, static_cast<const double*>(nullptr), out,
                  reg);
  // Seed directly; multiplying by weight would introduce another operation.
  out[0].adj() = weight;
  stan::math::grad();
  return {out[0].val(), {tv.adj(), yv.adj(), th.adj()}};
}
void exact(double a, double b, const char* what) {
  if (bits(a) != bits(b)) {
    std::cerr << what << ": " << std::setprecision(17) << a << " != " << b
              << '\n';
    throw std::runtime_error("bitwise callback mismatch");
  }
}

template <class F>
double measure(F&& f) {
  auto start = Clock::now();
  while (Clock::now() - start < std::chrono::milliseconds(100)) sink = f();
  start = Clock::now();
  size_t n = 0;
  while (Clock::now() - start < std::chrono::milliseconds(150)) {
    sink = f();
    ++n;
  }
  return std::chrono::duration<double, std::nano>(Clock::now() - start)
             .count() /
         n;
}
}  // namespace
int main(int argc, char** argv) {
  try {
    if (argc != 3)
      throw std::runtime_error("usage: bench_loop_adjoint_local MIR N");
    const auto history_checks = loop_adjoint_checks();
    size_t end = 0;
    const int n = std::stoi(argv[2], &end);
    if (end != std::strlen(argv[2]) || n < 0 || n > 100000)
      throw std::runtime_error("N must be an integer in [0,100000]");
    const std::string text = slurp(argv[1]);
    auto start = Clock::now();
    const auto mir = stanli::mir::read_program(stanli::sexp::parse(text));
    std::map<std::string, const stanli::mir::FunDef*> funs;
    for (const auto& f : mir.fun_defs) funs[f.name] = &f;
    auto it = funs.find("rhs");
    if (it == funs.end()) throw std::runtime_error("wrapper must expose rhs");
    stanli::RhsArg count;
    count.is_int = true;
    count.ints = {n};
    stanli::RhsArg theta;
    theta.is_param = true;
    theta.len = 1;
    start = Clock::now();
    const auto rhs =
        stanli::compile_rhs_args(*it->second, funs, 1, {count, theta});
    const double register_prep_ns =
        std::chrono::duration<double, std::nano>(Clock::now() - start).count();
    if (!rhs.ok) throw std::runtime_error(rhs.why);
    if (rhs.n_y != 1 || rhs.n_th != 1 || rhs.n_xr != 0 || rhs.n_yp != 0)
      throw std::runtime_error("local probe requires one state and one real parameter");
    start = Clock::now();
    LoopRhsProbe probe(rhs);
    const double prep_ns =
        std::chrono::duration<double, std::nano>(Clock::now() - start).count();
    const auto evaluate = [](LoopRhsProbe& p, double t, double y, double th,
                             double weight) {
      p.forward(t, &y, {th}, nullptr);
      p.reverse({weight});
      return p.output(0);
    };
    start = Clock::now();
    evaluate(probe, 0.2, 0.75, 0.31, 1);
    const double first_ns =
        std::chrono::duration<double, std::nano>(Clock::now() - start).count();
    LoopRhsProbe clone(probe);
    size_t checked = 0;
    // Repeated, interleaved clones change the executed branches and loop trip
    // counts after the engine has already warmed its recording/replay state.
    for (int repeat = 0; repeat < 3; ++repeat)
      for (double t : {0.2, -0.1})
        for (double y : {0.75, -0.25})
          for (double th : {0.31, -0.7})
            for (double seed : {1.0, -0.3, 0.0, 2.75}) {
              Result want = oracle(rhs, t, y, th, seed), got{};
              auto& p = (checked % 2) ? clone : probe;
              got.value = evaluate(p, t, y, th, seed);
              std::copy(p.gradient().begin(), p.gradient().end(), got.gradient);
              exact(got.value, want.value, "value");
              for (int i = 0; i < 3; ++i)
                exact(got.gradient[i], want.gradient[i], "gradient");
              p.forward(t, &y, {th}, nullptr);
              exact(p.output(0), want.value, "value-only after reverse");
              ++checked;
            }
    std::cout << "{\"N\":" << n << ",\"checks\":" << checked
              << ",\"history_checks\":" << history_checks
              << ",\"loop_prep_ns\":" << prep_ns
              << ",\"register_prep_ns\":" << register_prep_ns
              << ",\"first_gradient_ns\":" << first_ns
              << ",\"register_instructions\":" << rhs.code.size()
              << ",\"prepared_instructions\":"
              << probe.plan().prepared_instructions()
              << ",\"blocks_executed\":" << probe.workspace().blocks_executed
              << ",\"history_bytes\":" << probe.workspace().retained_bytes()
              << ",\"register_var_ns\":"
              << measure([&] { return oracle(rhs, 0.2, 0.75, 0.31, 1).value; })
              << ",\"loop_gradient_ns\":"
              << measure([&] { return evaluate(probe, 0.2, 0.75, 0.31, 1); })
              << ",\"loop_value_ns\":" << measure([&] {
                   double y = 0.75;
                   probe.forward(0.2, &y, {0.31}, nullptr);
                   return probe.output(0);
                 })
              << "}\n";
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}

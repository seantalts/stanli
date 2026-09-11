// Developer-only Phase 0 spike for generated ODE RHS adjoints.
//
// This deliberately uses only existing production primitives.  The generated
// path copies a compacted RhsProgram into an IslandProg, asks gen_adjoint to
// add checkpoints and build the reverse program, then measures the complete
// callback lifecycle proposed for ODEs:
//
//   * seed and execute the checkpointed forward once on doubles;
//   * run one generated reverse sweep per RHS output;
//   * construct precomputed-gradient output vars; and
//   * run the subsequent Stan reverse sweeps and harvest their Jacobian rows.
//
// The comparison path is today's callback lifecycle: reuse the solver's
// deep-copied theta vars, create current-state vars, run the RHS register
// program on vars, and run/harvest one Stan reverse sweep per output.
// Correctness and consecutive-call behavior are checked bitwise before any
// timing.  This target is neither a test nor installed; it is an experiment
// kept beside the other developer benchmarks.
#include <stanli/adjoint.hpp>
#include <stanli/island.hpp>
#include <stanli/mir.hpp>
#include <stanli/ode_prog.hpp>
#include <stanli/program.hpp>
#include <stanli/sexp.hpp>

#include <stan/math.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using stan::math::var;
using stanli::AdjProgram;
using stanli::IslandProg;
using stanli::Program;
using stanli::RhsProgram;

volatile double benchmark_sink = 0.0;

std::string slurp(const std::string& path) {
  std::ifstream in(path);
  if (!in) throw std::runtime_error("cannot open " + path);
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

int positive_arg(const char* text, const char* name) {
  char* end = nullptr;
  const long value = std::strtol(text, &end, 10);
  if (!text[0] || !end || *end != '\0' || value <= 0 || value > 100000000L)
    throw std::runtime_error(std::string(name) + " must be a positive integer");
  return static_cast<int>(value);
}

int nonnegative_arg(const char* text, const char* name) {
  char* end = nullptr;
  const long value = std::strtol(text, &end, 10);
  if (!text[0] || !end || *end != '\0' || value < 0 || value > 100000000L)
    throw std::runtime_error(std::string(name) +
                             " must be a nonnegative integer");
  return static_cast<int>(value);
}

uint64_t bits(double value) {
  uint64_t out;
  std::memcpy(&out, &value, sizeof(out));
  return out;
}

bool same_vector(const char* label, const std::vector<double>& got,
                 const std::vector<double>& want) {
  if (got.size() != want.size()) {
    std::fprintf(stderr, "%s size: got %zu, want %zu\n", label, got.size(),
                 want.size());
    return false;
  }
  for (size_t i = 0; i < got.size(); ++i) {
    if (bits(got[i]) == bits(want[i])) continue;
    std::fprintf(stderr,
                 "%s[%zu]: got %.17g (0x%016llx), want %.17g "
                 "(0x%016llx)\n",
                 label, i, got[i], (unsigned long long)bits(got[i]), want[i],
                 (unsigned long long)bits(want[i]));
    return false;
  }
  return true;
}

bool adjoints_are_positive_zero(const char* label,
                                const std::vector<var>& vars) {
  for (size_t i = 0; i < vars.size(); ++i) {
    if (bits(vars[i].adj()) == bits(0.0)) continue;
    std::fprintf(stderr, "%s[%zu]: got %.17g (0x%016llx), want +0\n", label, i,
                 vars[i].adj(), (unsigned long long)bits(vars[i].adj()));
    return false;
  }
  return true;
}

struct Prototype {
  RhsProgram rhs;
  IslandProg generated;
  bool generated_ok = false;
  std::string refusal;
};

std::string structural_refusal(const Program& p) {
  std::vector<std::string> opcodes;
  for (const auto& instruction : p.code) {
    const auto& spec = stanli::program_code_spec(instruction.code);
    if (!spec.has(stanli::kProgramNoAdjoint)) continue;
    const std::string name(spec.name);
    if (std::find(opcodes.begin(), opcodes.end(), name) == opcodes.end())
      opcodes.push_back(name);
  }
  std::string out;
  for (size_t i = 0; i < opcodes.size(); ++i) {
    if (i) out += ", ";
    out += opcodes[i];
  }
  return out;
}

Prototype make_prototype(const std::string& fixture,
                         const std::string& rhs_name, int n_y, int n_theta,
                         int n_x_r) {
  stanli::mir::Program mir =
      stanli::mir::read_program(stanli::sexp::parse(slurp(fixture)));
  std::map<std::string, const stanli::mir::FunDef*> funs;
  for (const auto& f : mir.fun_defs) funs[f.name] = &f;
  const auto it = funs.find(rhs_name);
  if (it == funs.end())
    throw std::runtime_error("fixture has no " + rhs_name + " RHS");

  Prototype out;
  out.rhs = stanli::compile_rhs(*it->second, funs, n_y, n_theta, n_x_r, {3});
  if (!out.rhs.ok)
    throw std::runtime_error(rhs_name +
                             " register compilation failed: " + out.rhs.why);

  // Slice only the canonical compacted Program.  gen_adjoint is allowed to
  // mutate this copy with checkpoints; the RhsProgram remains the exact old
  // var replay used by the comparison path.
  static_cast<Program&>(out.generated) = static_cast<const Program&>(out.rhs);
  out.generated.ins.push_back(
      IslandProg::LiveIn{out.rhs.t_reg, 1, -1, 0, false});
  out.generated.ins.push_back(
      IslandProg::LiveIn{out.rhs.y0, out.rhs.n_y, -1, 0, true});
  out.generated.ins.push_back(
      IslandProg::LiveIn{out.rhs.th0, out.rhs.n_th, -1, 0, true});
  out.generated.ins.push_back(
      IslandProg::LiveIn{out.rhs.xr0, out.rhs.n_xr, -1, 0, false});
  out.generated_ok = stanli::gen_adjoint(out.generated);
  if (!out.generated_ok) {
    const std::string opcodes = structural_refusal(out.rhs);
    out.refusal = opcodes.empty() ? "adjoint generation failed"
                                  : "unsupported opcodes: " + opcodes;
  }
  return out;
}

std::vector<double> deterministic_values(int count, double base, double step) {
  std::vector<double> out((size_t)count);
  for (int i = 0; i < count; ++i)
    out[(size_t)i] = base + step * (double)(i + 1);
  return out;
}

struct Observation {
  std::vector<double> values;
  std::vector<double> local_jacobian;
  std::vector<double> harvested_jacobian;
};

struct OldWorkspace {
  std::vector<var> registers;
  std::vector<var> y;
  std::vector<var> outputs;
  std::vector<double> harvested;

  OldWorkspace(size_t n_y, size_t n_theta, size_t n_out)
      : harvested(n_out * (n_y + n_theta)) {
    y.reserve(n_y);
    outputs.reserve(n_out);
  }
};

struct GeneratedWorkspace {
  std::vector<var> y;
  std::vector<var> outputs;
  std::vector<double> values;
  std::vector<double> adjoints;
  std::vector<double> harvested;

  GeneratedWorkspace(const Prototype& p)
      : values((size_t)p.generated.n_regs),
        adjoints((size_t)p.generated.adj.n_regs),
        harvested(p.generated.out_regs.size() *
                  (size_t)(p.rhs.n_y + p.rhs.n_th)) {
    y.reserve((size_t)p.rhs.n_y);
    outputs.reserve(p.generated.out_regs.size());
  }
};

template <typename Vars>
void refill_vars(Vars& vars, const std::vector<double>& values) {
  vars.clear();
  for (double value : values) vars.emplace_back(value);
}

// Match coupled_ode_system's callback lifecycle exactly.  The state vars and
// outputs live on the per-callback nested tape, while the deep-copied theta
// vars live outside it.  Rows are swept in output order; nested adjoints only
// need clearing between rows, and the persistent theta adjoints must be
// cleared after every row (including the last one).
void sweep_and_harvest(const std::vector<var>& outputs,
                       const std::vector<var>& y, const std::vector<var>& theta,
                       stan::math::nested_rev_autodiff& nested,
                       std::vector<double>& harvested) {
  const size_t width = y.size() + theta.size();
  harvested.resize(outputs.size() * width);
  for (size_t o = 0; o < outputs.size(); ++o) {
    stan::math::grad(outputs[o].vi_);
    for (size_t i = 0; i < y.size(); ++i) harvested[o * width + i] = y[i].adj();
    for (size_t i = 0; i < theta.size(); ++i) {
      harvested[o * width + y.size() + i] = theta[i].adj();
      theta[i].adj() = 0.0;
    }
    if (o + 1 < outputs.size()) nested.set_zero_all_adjoints();
  }
}

double checksum(const std::vector<var>& outputs,
                const std::vector<double>& harvested) {
  double out = 0.0;
  for (size_t o = 0; o < outputs.size(); ++o)
    out += (double)(o + 1) * outputs[o].val();
  for (size_t i = 0; i < harvested.size(); ++i)
    out += (0.03125 + (double)(i + 1) * 0.0078125) * harvested[i];
  return out;
}

double old_callback(const Prototype& p, double t,
                    const std::vector<double>& y_values,
                    const std::vector<var>& theta,
                    const std::vector<double>& x_r, OldWorkspace& ws,
                    Observation* observation = nullptr) {
  // Clear stale handles before opening the next nested arena; their pointed-to
  // varis belonged to the preceding callback and have already been recovered.
  ws.y.clear();
  ws.outputs.clear();
  stan::math::nested_rev_autodiff nested;
  refill_vars(ws.y, y_values);

  stanli::run_rhs<var>(p.rhs, t, ws.y.data(), theta.data(), theta.size(),
                       x_r.data(), ws.outputs, ws.registers);
  const size_t n_out = ws.outputs.size();
  sweep_and_harvest(ws.outputs, ws.y, theta, nested, ws.harvested);

  if (observation) {
    observation->values.resize(n_out);
    for (size_t o = 0; o < n_out; ++o)
      observation->values[o] = ws.outputs[o].val();
    observation->local_jacobian.clear();
    observation->harvested_jacobian = ws.harvested;
  }
  return checksum(ws.outputs, ws.harvested);
}

double generated_callback(const Prototype& p, double t,
                          const std::vector<double>& y_values,
                          const std::vector<var>& theta,
                          const std::vector<double>& x_r,
                          GeneratedWorkspace& ws,
                          Observation* observation = nullptr) {
  ws.y.clear();
  ws.outputs.clear();
  stan::math::nested_rev_autodiff nested;
  refill_vars(ws.y, y_values);

  const size_t n_out = p.generated.out_regs.size();
  const size_t width = ws.y.size() + theta.size();
  auto& arena = stan::math::ChainableStack::instance_->memalloc_;
  stan::math::vari** operands = arena.alloc_array<stan::math::vari*>(width);
  double* local_jacobian = arena.alloc_array<double>(n_out * width);
  for (size_t i = 0; i < ws.y.size(); ++i) operands[i] = ws.y[i].vi_;
  for (size_t i = 0; i < theta.size(); ++i)
    operands[ws.y.size() + i] = theta[i].vi_;

  // Seed the checkpoint-capable forward's original live-in registers.  The
  // appended checkpoint registers are written by the generated forward.  The
  // whole reusable file is cleared so unwritten or newly appended cells can
  // never retain data from a preceding callback.
  std::fill(ws.values.begin(), ws.values.end(), 0.0);
  ws.values[(size_t)p.rhs.t_reg] = t;
  for (int i = 0; i < p.rhs.n_y; ++i)
    ws.values[(size_t)(p.rhs.y0 + i)] = y_values[(size_t)i];
  for (int i = 0; i < p.rhs.n_th; ++i)
    ws.values[(size_t)(p.rhs.th0 + i)] = theta[(size_t)i].val();
  for (int i = 0; i < p.rhs.n_xr; ++i)
    ws.values[(size_t)(p.rhs.xr0 + i)] = x_r[(size_t)i];
  stanli::run_program(static_cast<const Program&>(p.generated),
                      ws.values.data());

  const AdjProgram& reverse = p.generated.adj;
  for (size_t o = 0; o < n_out; ++o) {
    const int out_reg = p.generated.out_regs[o];
    std::fill(ws.adjoints.begin(), ws.adjoints.end(), 0.0);
    // Add the seed through adj_reg: if an output aliases an active live-in,
    // this preserves the identity contribution instead of overwriting it.
    ws.adjoints[(size_t)reverse.adj_reg[(size_t)out_reg]] += 1.0;
    stanli::run_adjoint(p.generated, reverse, ws.values.data(),
                        ws.adjoints.data());
    double* row = local_jacobian + o * width;
    for (int i = 0; i < p.rhs.n_y; ++i) {
      const int reg = p.rhs.y0 + i;
      row[(size_t)i] = ws.adjoints[(size_t)reverse.adj_reg[(size_t)reg]];
    }
    for (int i = 0; i < p.rhs.n_th; ++i) {
      const int reg = p.rhs.th0 + i;
      row[(size_t)p.rhs.n_y + (size_t)i] =
          ws.adjoints[(size_t)reverse.adj_reg[(size_t)reg]];
    }
  }

  // Match ode_store_sensitivities' low-level layout: every output node shares
  // one arena operand array and points at its row in one contiguous arena
  // Jacobian.  This avoids the public helper's per-output pointer and gradient
  // copies while retaining Stan Math's ordinary precomputed-gradient nodes.
  for (size_t o = 0; o < n_out; ++o) {
    const int out_reg = p.generated.out_regs[o];
    ws.outputs.emplace_back(new stan::math::precomputed_gradients_vari(
        ws.values[(size_t)out_reg], width, operands,
        local_jacobian + o * width));
  }

  // Account for the sweeps the surrounding Stan Jacobian machinery performs
  // after the callback returns its precomputed-gradient vars.
  sweep_and_harvest(ws.outputs, ws.y, theta, nested, ws.harvested);

  if (observation) {
    observation->values.resize(n_out);
    for (size_t o = 0; o < n_out; ++o)
      observation->values[o] = ws.outputs[o].val();
    observation->local_jacobian.assign(local_jacobian,
                                       local_jacobian + n_out * width);
    observation->harvested_jacobian = ws.harvested;
  }
  return checksum(ws.outputs, ws.harvested);
}

template <typename F>
double time_callbacks(int iterations, F&& callback) {
  double local_sink = 0.0;
  const auto begin = std::chrono::steady_clock::now();
  for (int i = 0; i < iterations; ++i) local_sink += callback();
  const auto end = std::chrono::steady_clock::now();
  benchmark_sink += local_sink;
  return std::chrono::duration<double, std::nano>(end - begin).count() /
         (double)iterations;
}

double median(std::vector<double> values) {
  std::sort(values.begin(), values.end());
  const size_t middle = values.size() / 2;
  if (values.size() % 2) return values[middle];
  return 0.5 * (values[middle - 1] + values[middle]);
}

}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc > 8) {
      std::fprintf(stderr,
                   "usage: %s [iterations-per-batch] [batches] [fixture] "
                   "[rhs-name] [n-y] [n-theta] [n-x-r]\n",
                   argv[0]);
      return 2;
    }
    const int iterations =
        argc > 1 ? positive_arg(argv[1], "iterations") : 5000;
    const int batches = argc > 2 ? positive_arg(argv[2], "batches") : 21;
    const std::string fixture =
        argc > 3 ? argv[3] : "tests/fixtures/odefns.tmir.sexp";
    const std::string rhs_name = argc > 4 ? argv[4] : "f_lin";
    const int n_y = argc > 5 ? positive_arg(argv[5], "n-y") : 2;
    const int n_theta = argc > 6 ? nonnegative_arg(argv[6], "n-theta") : 4;
    const int n_x_r = argc > 7 ? nonnegative_arg(argv[7], "n-x-r") : 2;

    const Prototype prototype =
        make_prototype(fixture, rhs_name, n_y, n_theta, n_x_r);
    if (!prototype.generated_ok) {
      const bool structural =
          prototype.refusal.rfind("unsupported opcodes:", 0) == 0;
      std::printf("%s generated-adjoint %s: %s\n", rhs_name.c_str(),
                  structural ? "structurally refused" : "refused",
                  prototype.refusal.c_str());
      std::printf(
          "forward instructions: canonical=%zu checkpointed=refused; "
          "reverse=refused\n",
          prototype.rhs.code.size());
      std::printf("registers: canonical=%d; outputs=%zu inputs=%d\n",
                  prototype.rhs.n_regs, prototype.rhs.out_regs.size(),
                  prototype.rhs.n_y + prototype.rhs.n_th);
      return 0;
    }

    const std::vector<double> y = deterministic_values(n_y, 0.7, 0.2);
    const std::vector<double> theta_values =
        deterministic_values(n_theta, 0.11, 0.07);
    const std::vector<double> x_r = deterministic_values(n_x_r, 1.0, 0.5);
    constexpr double t = 0.73;
    // coupled_ode_system deep-copies active arguments once when the solver is
    // constructed.  Keep that copy on the outer tape; only state vars are
    // created inside each callback's nested scope.
    std::vector<var> old_theta;
    std::vector<var> generated_theta;
    old_theta.reserve(theta_values.size());
    generated_theta.reserve(theta_values.size());
    for (double value : theta_values) {
      old_theta.emplace_back(value);
      generated_theta.emplace_back(value);
    }

    OldWorkspace old_ws(y.size(), old_theta.size(),
                        prototype.rhs.out_regs.size());
    GeneratedWorkspace generated_ws(prototype);

    Observation old_observation, generated_observation;
    (void)old_callback(prototype, t, y, old_theta, x_r, old_ws,
                       &old_observation);
    (void)generated_callback(prototype, t, y, generated_theta, x_r,
                             generated_ws, &generated_observation);
    bool verified = true;
    verified &= same_vector("RHS values", generated_observation.values,
                            old_observation.values);
    verified &= same_vector("generated local Jacobian",
                            generated_observation.local_jacobian,
                            old_observation.harvested_jacobian);
    verified &= same_vector("precomputed-gradient harvest",
                            generated_observation.harvested_jacobian,
                            old_observation.harvested_jacobian);
    verified &= adjoints_are_positive_zero("old theta adjoint", old_theta);
    verified &=
        adjoints_are_positive_zero("generated theta adjoint", generated_theta);

    Observation old_repeat, generated_repeat;
    (void)old_callback(prototype, t, y, old_theta, x_r, old_ws, &old_repeat);
    (void)generated_callback(prototype, t, y, generated_theta, x_r,
                             generated_ws, &generated_repeat);
    verified &= same_vector("repeated old RHS values", old_repeat.values,
                            old_observation.values);
    verified &=
        same_vector("repeated old Jacobian", old_repeat.harvested_jacobian,
                    old_observation.harvested_jacobian);
    verified &=
        same_vector("repeated generated RHS values", generated_repeat.values,
                    generated_observation.values);
    verified &= same_vector("repeated generated local Jacobian",
                            generated_repeat.local_jacobian,
                            generated_observation.local_jacobian);
    verified &= same_vector("repeated precomputed-gradient harvest",
                            generated_repeat.harvested_jacobian,
                            generated_observation.harvested_jacobian);
    verified &=
        adjoints_are_positive_zero("repeated old theta adjoint", old_theta);
    verified &= adjoints_are_positive_zero("repeated generated theta adjoint",
                                           generated_theta);
    if (!verified) return 1;

    const int warmups = std::max(2000, iterations / 2);
    double warm_sink = 0.0;
    for (int i = 0; i < warmups; ++i) {
      if (i & 1) {
        warm_sink += generated_callback(prototype, t, y, generated_theta, x_r,
                                        generated_ws);
        warm_sink += old_callback(prototype, t, y, old_theta, x_r, old_ws);
      } else {
        warm_sink += old_callback(prototype, t, y, old_theta, x_r, old_ws);
        warm_sink += generated_callback(prototype, t, y, generated_theta, x_r,
                                        generated_ws);
      }
    }
    benchmark_sink += warm_sink;

    std::vector<double> old_samples, generated_samples, ratios;
    old_samples.reserve((size_t)batches);
    generated_samples.reserve((size_t)batches);
    ratios.reserve((size_t)batches);
    for (int batch = 0; batch < batches; ++batch) {
      double old_ns, generated_ns;
      if (batch & 1) {
        generated_ns = time_callbacks(iterations, [&] {
          return generated_callback(prototype, t, y, generated_theta, x_r,
                                    generated_ws);
        });
        old_ns = time_callbacks(iterations, [&] {
          return old_callback(prototype, t, y, old_theta, x_r, old_ws);
        });
      } else {
        old_ns = time_callbacks(iterations, [&] {
          return old_callback(prototype, t, y, old_theta, x_r, old_ws);
        });
        generated_ns = time_callbacks(iterations, [&] {
          return generated_callback(prototype, t, y, generated_theta, x_r,
                                    generated_ws);
        });
      }
      old_samples.push_back(old_ns);
      generated_samples.push_back(generated_ns);
      ratios.push_back(old_ns / generated_ns);
    }

    const double old_median = median(old_samples);
    const double generated_median = median(generated_samples);
    const double paired_median = median(ratios);
    const double aggregate_ratio =
        std::accumulate(old_samples.begin(), old_samples.end(), 0.0) /
        std::accumulate(generated_samples.begin(), generated_samples.end(),
                        0.0);

    std::printf("%s generated-adjoint prototype verified bitwise\n",
                rhs_name.c_str());
    std::printf(
        "forward instructions: canonical=%zu checkpointed=%zu; "
        "reverse=%zu\n",
        prototype.rhs.code.size(), prototype.generated.code.size(),
        prototype.generated.adj.code.size());
    std::printf(
        "registers: canonical=%d checkpointed=%d adjoint=%d; "
        "outputs=%zu inputs=%d\n",
        prototype.rhs.n_regs, prototype.generated.n_regs,
        prototype.generated.adj.n_regs, prototype.generated.out_regs.size(),
        prototype.rhs.n_y + prototype.rhs.n_th);
    std::printf("warmups=%d batches=%d iterations/batch=%d\n", warmups, batches,
                iterations);
    std::printf("old var callback       median %9.1f ns/callback\n",
                old_median);
    std::printf("generated + precomputed median %9.1f ns/callback\n",
                generated_median);
    std::printf(
        "speedup: %.3fx paired median, %.3fx aggregate "
        "(generated %.1f%% of old)\n",
        paired_median, aggregate_ratio, 100.0 * generated_median / old_median);
    std::printf("sink=%.17g\n", (double)benchmark_sink);
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "bench_rhs_adjoint: %s\n", e.what());
    return 1;
  }
}

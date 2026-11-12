#include "env_helpers.hpp"
#include "tools/stanc_process.hpp"
#include <stanli/compile.hpp>
#include <stanli/dae.hpp>
#include <stanli/density_registry.hpp>
#include <stanli/graph.hpp>
#include <stanli/island.hpp>
#include <stanli/ode_adjoint.hpp>
#include <stanli/optable.hpp>
#include <stanli/structured_loop.hpp>
#include <stanli/packet.hpp>
#include <stanli/region_map.hpp>
#include <stanli/wa_interp.hpp>

#include <stan/math.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

static int failures = 0;
static void check(bool ok, const std::string& what) {
  if (!ok) {
    ++failures;
    std::printf("FAIL %s\n", what.c_str());
  }
}
static int64_t ulp_key(double d) {
  int64_t i;
  std::memcpy(&i, &d, sizeof(i));
  return i < 0 ? std::numeric_limits<int64_t>::min() - i : i;
}
// Project parity budget: up to 2 ULP vs references is acceptable. A caller
// whose reference reassociates -- or, for matrix_exp's block-exponential
// identity, runs a different algorithm than the nested-tape replay it
// compares against -- may pass a wider stated budget.
static void expect_ulp(const std::string& what, double got, double want,
                       int64_t max_ulp = 2) {
  const int64_t d = std::llabs(ulp_key(got) - ulp_key(want));
  if (d > max_ulp) {
    ++failures;
    std::printf("FAIL %-16s got %.17g want %.17g (%lld ulp)\n", what.c_str(),
                got, want, (long long)d);
  }
}
static std::string slurp(const std::string& path) {
  std::ifstream f(path);
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}
static int count_opcode(const stanli::CompiledModel& cm, uint16_t opcode) {
  return static_cast<int>(
      std::count_if(cm.graph.ops.begin(), cm.graph.ops.end(),
                    [=](const stanli::Op& op) { return op.opcode == opcode; }));
}

static bool same_graph_structure(const stanli::CompiledModel& a,
                                 const stanli::CompiledModel& b) {
  if (a.graph.result_slot != b.graph.result_slot ||
      a.graph.slots.size() != b.graph.slots.size() ||
      a.graph.ops.size() != b.graph.ops.size() ||
      a.fills.size() != b.fills.size())
    return false;
  for (size_t i = 0; i < a.graph.slots.size(); ++i)
    if (a.graph.slots[i].len != b.graph.slots[i].len ||
        a.graph.slots[i].is_param != b.graph.slots[i].is_param)
      return false;
  for (size_t i = 0; i < a.fills.size(); ++i)
    if (a.fills[i].first != b.fills[i].first ||
        a.fills[i].second.size() != b.fills[i].second.size())
      return false;
  for (size_t i = 0; i < a.graph.ops.size(); ++i) {
    const stanli::Op& x = a.graph.ops[i];
    const stanli::Op& y = b.graph.ops[i];
    if (x.opcode != y.opcode || x.variant != y.variant || x.out != y.out ||
        x.out2 != y.out2 || x.n_in != y.n_in || x.n_idata != y.n_idata ||
        (x.udata == nullptr) != (y.udata == nullptr))
      return false;
    for (int k = 0; k < x.n_in; ++k)
      if (x.in[k] != y.in[k]) return false;
    for (int64_t k = 0; k < x.n_idata; ++k)
      if (x.idata[k] != y.idata[k]) return false;
  }
  return true;
}

namespace region_map_test {

using stan::math::var;
using stanli::CompiledModel;
using stanli::DataMap;
using stanli::Executor;
using stanli::OP_ISLAND;
using stanli::OP_LOOP;
using stanli::OP_REGION_MAP;

struct Series {
  std::vector<double> y, x;
};

Series make_series(int n) {
  Series s;
  for (int i = 0; i < n; ++i) {
    const double x = -2.0 + 4.0 * i / (n - 1);
    s.x.push_back(x);
    s.y.push_back(0.5 * x + 0.9 * std::sin(7.0 * i + 1.0));
  }
  return s;
}

DataMap make_data(int n, const std::string& extra = "") {
  const Series s = make_series(n);
  std::ostringstream out;
  out.precision(17);
  out << "{\"N\":" << n << ",\"y\":[";
  for (int i = 0; i < n; ++i) out << (i ? "," : "") << s.y[i];
  out << "],\"x\":[";
  for (int i = 0; i < n; ++i) out << (i ? "," : "") << s.x[i];
  out << "]" << (extra.empty() ? "" : "," + extra) << "}";
  return DataMap::from_json(out.str());
}

using Params = std::vector<double>;
using Ref = std::function<var(const std::vector<var>&, const Series&,
                              std::vector<int>*)>;

std::string fixture_mir(const std::string& stem) {
  return slurp("tests/fixtures/" + stem + ".tmir.sexp");
}

CompiledModel compile_with(const std::string& stem, int n,
                           const char* env = nullptr, const char* value = "0",
                           const std::string& extra = "") {
  if (env) test_setenv(env, value, 1);
  try {
    CompiledModel cm =
        stanli::compile_model(fixture_mir(stem), make_data(n, extra));
    if (env) test_unsetenv(env);
    return cm;
  } catch (...) {
    if (env) test_unsetenv(env);
    throw;
  }
}

std::map<uint16_t, int> histogram(const CompiledModel& cm) {
  std::map<uint16_t, int> h;
  for (const stanli::Op& op : cm.graph.ops) ++h[op.opcode];
  return h;
}

struct Eval {
  double lp = 0;
  std::vector<double> grad;
};

Eval evaluate(Executor& ex, const Params& p) {
  for (size_t i = 0; i < p.size(); ++i) ex.params_data()[i] = p[i];
  Eval e;
  e.grad.assign(p.size(), 0.0);
  e.lp = ex.gradient(e.grad.data());
  return e;
}

Eval reference(const Ref& ref, const Series& s, const Params& p,
               std::vector<int>* branches) {
  std::vector<var> v(p.begin(), p.end());
  var lp = ref(v, s, branches);
  lp.grad();
  Eval e;
  e.lp = lp.val();
  for (const var& q : v) e.grad.push_back(q.adj());
  stan::math::recover_memory();
  return e;
}

void expect_eval(const std::string& what, const Eval& got, const Eval& want,
                 int64_t ulp = 10) {
  expect_ulp(what + " lp", got.lp, want.lp, ulp);
  for (size_t i = 0; i < want.grad.size(); ++i)
    expect_ulp(what + " grad[" + std::to_string(i) + "]", got.grad[i],
               want.grad[i], ulp);
}

int flips(const std::vector<int>& a, const std::vector<int>& b) {
  int k = 0;
  for (size_t i = 0; i < a.size() && i < b.size(); ++i) k += a[i] != b[i];
  return k;
}

void expect_mapped(const std::string& what, const CompiledModel& cm) {
  check(count_opcode(cm, OP_REGION_MAP) == 1, what + ": one OP_REGION_MAP");
  check(count_opcode(cm, OP_LOOP) == 0, what + ": no OP_LOOP");
  check(count_opcode(cm, OP_ISLAND) == 0, what + ": no OP_ISLAND");
}

void run_points(const std::string& stem, int n,
                const std::vector<Params>& points, const Ref& ref,
                bool expect_map = true, const char* env = nullptr,
                const char* value = "0", const std::string& extra = "") {
  const Series s = make_series(n);
  CompiledModel cm = compile_with(stem, n, env, value, extra);
  if (expect_map)
    expect_mapped(stem, cm);
  else
    check(count_opcode(cm, OP_REGION_MAP) == 0, stem + ": no OP_REGION_MAP");
  Executor ex(cm.graph);
  cm.bind(ex);
  std::vector<std::vector<int>> taken(points.size());
  for (size_t k = 0; k < points.size(); ++k) {
    const Eval want = reference(ref, s, points[k], &taken[k]);
    expect_eval(stem + " point " + std::to_string(k), evaluate(ex, points[k]),
                want);
  }
  if (points.size() > 1)
    check(flips(taken[0], taken[1]) > 0,
          stem + ": parameter points flip branches");
}

const Ref ref_branch = [](const std::vector<var>& p, const Series& s,
                          std::vector<int>* taken) {
  var lp = 0;
  for (size_t i = 0; i < s.y.size(); ++i) {
    const var r = s.y[i] - p[0] * s.x[i];
    const bool low = r < p[1];
    if (taken) taken->push_back(low);
    lp += low ? -stan::math::square(r) -
                    stan::math::log1p(stan::math::square(p[1]))
              : -0.5 * stan::math::square(r) + p[1];
  }
  return lp;
};

const Ref ref_simple = [](const std::vector<var>& p, const Series& s,
                          std::vector<int>* taken) {
  var lp = 0;
  for (size_t i = 0; i < s.y.size(); ++i) {
    const var r = s.y[i] - p[0] * s.x[i];
    const bool low = r < p[1];
    if (taken) taken->push_back(low);
    lp += low ? -stan::math::square(r) : -0.5 * stan::math::square(r) + p[1];
  }
  return lp;
};

const Ref ref_udf = [](const std::vector<var>& p, const Series& s,
                       std::vector<int>* taken) {
  const var sc = stan::math::exp(p[1]);
  var lp = 0;
  for (size_t i = 0; i < s.y.size(); ++i) {
    const var d = s.y[i] - p[0] * s.x[i];
    const bool tail = d < -sc;
    if (taken) taken->push_back(tail);
    lp += tail ? -0.5 * stan::math::square(d / sc) - stan::math::log(sc) -
                     0.5 * sc
               : stan::math::log1p_exp(-stan::math::square(d) * sc) -
                     stan::math::log(sc);
  }
  return lp;
};

const std::vector<Params> branch_points = {
    {0.3, -0.5}, {-1.5, 0.7}, {0.3, -0.5}};
const std::vector<Params> udf_points = {{0.3, -0.4}, {-1.5, 0.9}, {0.3, -0.4}};

void test_same_executor_two_points() {
  run_points("region_map_branch", 64, branch_points, ref_branch);
  run_points("region_map_udf", 64, udf_points, ref_udf);
  run_points("region_map_branch", 512, branch_points, ref_branch);
}

void test_size_independent() {
  for (const char* stem :
       {"region_map_branch", "region_map_udf", "region_map_many"}) {
    size_t instrs[2] = {0, 0}, regs[2] = {0, 0};
    int slot = 0;
    for (int n : {64, 512}) {
      CompiledModel cm = compile_with(stem, n);
      for (const stanli::Op& op : cm.graph.ops) {
        if (op.opcode != OP_REGION_MAP) continue;
        const auto& p = *static_cast<const stanli::RegionMapProg*>(op.udata);
        int live = 0;
        for (const auto& li : p.ins) live += li.len;
        instrs[slot] = p.code.size();
        regs[slot] = (size_t)(p.n_regs - live);
      }
      ++slot;
    }
    check(instrs[0] > 0 && instrs[0] == instrs[1],
          std::string(stem) + ": instruction count independent of N");
    check(regs[0] == regs[1],
          std::string(stem) + ": non-live-in registers independent of N");
  }
}

void expect_refused(const std::string& stem, int n, const Ref& ref,
                    const std::vector<Params>& points) {
  CompiledModel cm = compile_with(stem, n);
  CompiledModel off = compile_with(stem, n, "STANLI_REGION_MAP", "0");
  check(count_opcode(cm, OP_REGION_MAP) == 0, stem + ": refused");
  check(histogram(cm) == histogram(off),
        stem + ": refusal keeps today's graph op kinds");
  const Series s = make_series(n);
  Executor ex(cm.graph), ex_off(off.graph);
  cm.bind(ex);
  off.bind(ex_off);
  for (const Params& p : points) {
    std::vector<int> taken;
    const Eval want = reference(ref, s, p, &taken);
    const Eval got = evaluate(ex, p);
    const Eval before = evaluate(ex_off, p);
    expect_eval(stem + " refused", got, want);
    check(got.lp == before.lp && got.grad == before.grad,
          stem + ": refused loop matches the ablation bitwise");
  }
}

void test_refusals() {
  const Ref ref_carry = [](const std::vector<var>& p, const Series& s,
                           std::vector<int>* taken) {
    var acc = 0;
    for (size_t i = 0; i < s.y.size(); ++i) {
      const bool low = s.y[i] - p[0] * s.x[i] < p[1];
      if (taken) taken->push_back(low);
      acc = low ? 0.9 * acc + p[0] : 0.9 * acc + s.y[i] * p[1];
    }
    return -0.01 * stan::math::square(acc);
  };
  expect_refused("region_map_carry", 64, ref_carry, branch_points);
  expect_refused("region_map_outer_local", 64, ref_simple, branch_points);
  expect_refused("region_map_break", 64, ref_simple, branch_points);
  expect_refused("region_map_target_read", 64, ref_simple, branch_points);
  expect_refused("region_map_acc_read", 64, ref_simple, branch_points);
  const Ref ref_data = [](const std::vector<var>& p, const Series& s,
                          std::vector<int>* taken) {
    var lp = 0;
    for (size_t i = 0; i < s.y.size(); ++i) {
      const var r = s.y[i] - p[0] * s.x[i];
      const bool low = s.y[i] < 0.1;
      if (taken) taken->push_back(low);
      lp += low ? -stan::math::square(r) + p[1]
                : -0.5 * stan::math::square(r) - p[1];
    }
    return lp;
  };
  expect_refused("region_map_data_control", 64, ref_data, branch_points);
  expect_refused("region_map_branch", 16, ref_branch, branch_points);
  {
    CompiledModel off =
        compile_with("region_map_branch", 64, "STANLI_REGION_MAP", "0");
    check(count_opcode(off, OP_REGION_MAP) == 0,
          "STANLI_REGION_MAP=0 disables the map");
    const Series s = make_series(64);
    Executor ex(off.graph);
    off.bind(ex);
    std::vector<int> taken;
    expect_eval("ablated branch", evaluate(ex, branch_points[0]),
                reference(ref_branch, s, branch_points[0], &taken));
  }
}

void test_shapes() {
  const Ref ref_many = [](const std::vector<var>& p, const Series& s,
                          std::vector<int>* taken) {
    var lp = 0;
    for (size_t i = 0; i < s.y.size(); ++i) {
      const double x = s.x[i];
      const var v1 = p[0] * x, v2 = p[1] * x + 0.3, v3 = p[2] * x - 0.2,
                v4 = p[3] * stan::math::square(x);
      const var r = s.y[i] - v1;
      const bool low = r < v2;
      if (taken) taken->push_back(low);
      lp += low ? -stan::math::square(v3 - s.y[i]) - v4 * x
                : -0.5 * stan::math::square(r) + v4 - 0.1 * v2 * v3;
    }
    return lp;
  };
  run_points(
      "region_map_many", 64,
      {{0.3, -0.6, 0.5, 0.2}, {-1.5, 0.4, -0.3, 0.1}, {0.3, -0.6, 0.5, 0.2}},
      ref_many);

  const Ref ref_alias = [](const std::vector<var>& p, const Series& s,
                           std::vector<int>* taken) {
    var lp = 0;
    var ss = 0;
    std::vector<var> mu;
    for (size_t i = 0; i < s.y.size(); ++i) mu.push_back(p[0] * s.x[i]);
    for (const var& m : mu) ss += stan::math::square(m);
    lp += -0.01 * ss;
    for (size_t i = 0; i < s.y.size(); ++i) {
      const var d = s.y[i] - mu[i];
      const bool low = d < p[1];
      if (taken) taken->push_back(low);
      lp +=
          low ? -stan::math::square(d) - 0.5 * stan::math::square(mu[i]) * p[1]
              : -0.5 * stan::math::square(s.y[i] - mu[i]) + p[1];
    }
    for (const var& m : mu)
      lp += -0.5 * stan::math::square(m / 5.0) - std::log(5.0) -
            0.9189385332046727;
    return lp;
  };
  run_points("region_map_alias", 64, {{0.3, -0.5}, {-1.5, 0.7}, {0.3, -0.5}},
             ref_alias);

  const Ref ref_int = [](const std::vector<var>& p, const Series& s,
                         std::vector<int>* taken) {
    var lp = 0;
    for (size_t i = 0; i < s.y.size(); ++i) {
      const var r = s.y[i] - p[0] * s.x[i];
      const bool low = r < p[1];
      if (taken) taken->push_back(low);
      lp += low ? -2 * stan::math::square(r)
                : -0.5 * stan::math::square(r) + p[1];
    }
    return lp;
  };
  run_points("region_map_local_int", 64, branch_points, ref_int);

  const Ref ref_call = [](const std::vector<var>& p, const Series& s,
                          std::vector<int>* taken) {
    const var sc = stan::math::exp(p[1]);
    var lp = 0;
    for (size_t i = 0; i < s.y.size(); ++i) {
      const var mu = p[0] * s.x[i];
      const bool low = s.y[i] - mu < -0.3;
      if (taken) taken->push_back(low);
      lp += low ? stan::math::normal_lpdf<false>(s.y[i], mu, sc)
                : stan::math::student_t_lpdf<false>(s.y[i], 4, mu, sc);
    }
    return lp;
  };
  run_points("region_map_call", 64, {{0.3, -0.4}, {-1.5, 0.3}, {0.3, -0.4}},
             ref_call);
  run_points("region_map_call", 64, {{0.3, -0.4}, {-1.5, 0.3}, {0.3, -0.4}},
             ref_call, true, "STANLI_REGION_MAP_SAVE_LIMIT");

  const Ref ref_multi = [](const std::vector<var>& p, const Series& s,
                           std::vector<int>* taken) {
    var lp = stan::math::normal_lpdf<false>(p[0], 0, 2);
    for (size_t i = 0; i < s.y.size(); ++i) {
      const var r = s.y[i] - p[0] * s.x[i];
      const bool low = r < p[1];
      if (taken) taken->push_back(low);
      lp += -0.1 * stan::math::square(p[1]);
      lp += low ? -stan::math::square(r) : -0.5 * stan::math::square(r) + p[1];
      lp += -0.01 * r;
    }
    lp += stan::math::normal_lpdf<false>(p[1], 0, 3);
    return lp;
  };
  run_points("region_map_multi", 64, branch_points, ref_multi);
}

const stanli::RegionMapProg& map_payload(const CompiledModel& cm);

void test_local_accumulator() {
  const auto branch_from = [](size_t first, double init, double scale) {
    return Ref([=](const std::vector<var>& p, const Series& s,
                   std::vector<int>* taken) {
      var acc = init;
      for (size_t i = first; i < s.y.size(); ++i) {
        const var r = s.y[i] - p[0] * s.x[i];
        const bool low = r < p[1];
        if (taken) taken->push_back(low);
        acc += low ? -stan::math::square(r) -
                         stan::math::log1p(stan::math::square(p[1]))
                   : -0.5 * stan::math::square(r) + p[1];
      }
      return scale == 0 ? acc : -scale * stan::math::square(acc);
    });
  };
  run_points("region_map_acc_udf", 64, branch_points, branch_from(2, 0, 0));
  run_points("region_map_acc_reduce_sum", 64, branch_points,
             branch_from(0, 0, 0));
  run_points("region_map_acc_init", 64, branch_points,
             branch_from(0, 2.5, 0.01));
  run_points("region_map_acc_udf", 512, branch_points, branch_from(2, 0, 0));

  const std::string stem = "region_map_acc_udf";
  CompiledModel lanes = compile_with(stem, 64);
  CompiledModel scalar = compile_with(stem, 64, "STANLI_REGION_MAP_LANES", "0");
  check(map_payload(lanes).lanes.active && !map_payload(scalar).lanes.active,
        stem + ": offset index arithmetic runs in lanes");
  Executor lanes_ex(lanes.graph), scalar_ex(scalar.graph);
  lanes.bind(lanes_ex);
  scalar.bind(scalar_ex);
  for (const Params& point : branch_points) {
    const Eval a = evaluate(lanes_ex, point);
    const Eval b = evaluate(scalar_ex, point);
    check(a.lp == b.lp && a.grad == b.grad,
          stem + ": lanes bitwise equal to the scalar map");
  }
}

void test_integer_ops_map() {
  const Ref ref_intops = [](const std::vector<var>& p, const Series& s,
                            std::vector<int>* taken) {
    var lp = 0;
    for (size_t i = 0; i < s.y.size(); ++i) {
      const int n = static_cast<int>(i) + 1;
      const int k = n % 3;
      const int h = n / 2;
      const var r = s.y[i] - p[0] * s.x[i];
      const bool low = r < p[1];
      if (taken) taken->push_back(low);
      lp += low ? -(k + 1) * stan::math::square(r) - 0.01 * h * p[1]
                : -0.5 * stan::math::square(r) + p[1] * (h % 2);
    }
    return lp;
  };
  run_points("region_map_intops", 64, branch_points, ref_intops);
}

void test_long_body_map() {
  const Ref ref_long = [](const std::vector<var>& p, const Series& s,
                          std::vector<int>* taken) {
    var lp = 0;
    for (size_t i = 0; i < s.y.size(); ++i) {
      const var r = s.y[i] - p[0] * s.x[i];
      var acc = 0;
      for (int j = 1; j <= 340; ++j)
        acc += stan::math::exp(-0.002 * j * stan::math::square(r)) * p[1];
      const bool low = r < p[1];
      if (taken) taken->push_back(low);
      lp += low ? -stan::math::square(r) + 0.001 * acc
                : -0.5 * stan::math::square(r) + p[1] - 0.002 * acc;
    }
    return lp;
  };
  run_points("region_map_long_body", 32, branch_points, ref_long);
  run_points("region_map_long_body", 32, branch_points, ref_long, true,
             "STANLI_REGION_MAP_SAVE_LIMIT");
}

std::string outcome(const std::string& stem, int n, const char* env) {
  try {
    CompiledModel cm = compile_with(stem, n, env);
    Executor ex(cm.graph);
    cm.bind(ex);
    evaluate(ex, {0.3, -0.5});
    return "ok";
  } catch (const std::exception& e) {
    return std::string(typeid(e).name()) + ": " + e.what();
  }
}

void test_out_of_range() {
  const std::string mapped = outcome("region_map_oob", 64, nullptr);
  const std::string before = outcome("region_map_oob", 64, "STANLI_REGION_MAP");
  check(before != "ok", "out of range index throws today");
  check(mapped == before, "out of range index throws the same exception: " +
                              mapped + " vs " + before);
}

void test_copied_executors() {
  const Series s = make_series(64);
  CompiledModel cm = compile_with("region_map_udf", 64);
  expect_mapped("copied executors", cm);
  Executor a(cm.graph);
  cm.bind(a);
  Executor b(a);
  Executor fresh_a(cm.graph), fresh_b(cm.graph);
  cm.bind(fresh_a);
  cm.bind(fresh_b);
  const Params pa = udf_points[0], pb = udf_points[1];
  for (int round = 0; round < 3; ++round) {
    const Eval ea = evaluate(a, pa);
    const Eval eb = evaluate(b, pb);
    const Eval fa = evaluate(fresh_a, pa);
    const Eval fb = evaluate(fresh_b, pb);
    check(ea.lp == fa.lp && ea.grad == fa.grad,
          "copied executor A matches a fresh executor");
    check(eb.lp == fb.lp && eb.grad == fb.grad,
          "copied executor B matches a fresh executor");
    std::vector<int> t;
    expect_eval("copied executor A reference", ea,
                reference(ref_udf, s, pa, &t));
  }
}

const stanli::RegionMapProg& map_payload(const CompiledModel& cm) {
  for (const stanli::Op& op : cm.graph.ops)
    if (op.opcode == OP_REGION_MAP)
      return *static_cast<const stanli::RegionMapProg*>(op.udata);
  throw std::runtime_error("no OP_REGION_MAP");
}

template <typename F>
void each_written_span(const stanli::Program& p,
                       const stanli::Program::Instr& I, F fn) {
  if (I.code == stanli::Program::CALL) {
    const auto& c = p.calls[(size_t)I.a];
    fn(c.out, c.out_len);
    fn(c.scratch, c.scratch_len);
    return;
  }
  const int len = stanli::program_output_len(I);
  if (len > 0) fn(I.dst, len);
}

bool map_calls(const CompiledModel& cm, uint16_t opcode) {
  for (const auto& c : map_payload(cm).calls)
    if (c.opcode == opcode) return true;
  return false;
}

bool map_has_code(const CompiledModel& cm, stanli::Program::Code code) {
  for (const auto& I : map_payload(cm).code)
    if (I.code == code) return true;
  return false;
}

var shifted_residual(double y, const var& mu) {
  return y > mu ? y - mu : y - (mu + 0.25);
}

const Ref ref_cf_udf = [](const std::vector<var>& p, const Series& s,
                          std::vector<int>* taken) {
  const var sc = stan::math::exp(p[1]);
  var lp = 0;
  for (size_t i = 0; i < s.y.size(); ++i) {
    const var d = shifted_residual(s.y[i], p[0] * s.x[i]);
    const bool tail = d < -sc;
    if (taken) taken->push_back(tail);
    lp += tail ? -0.5 * stan::math::square(d / sc) - stan::math::log(sc) -
                     0.5 * sc
               : stan::math::log1p_exp(-stan::math::square(d) * sc) -
                     stan::math::log(sc);
  }
  return lp;
};

Ref zero_probe_ref(double A) {
  return
      [A](const std::vector<var>& p, const Series& s, std::vector<int>* taken) {
        // Keep the source's vector product and Stan Math's reduction order
        // in its pullback, rather than accumulating scalar products per row.
        const Eigen::Matrix<var, -1, 1> mu = stan::math::multiply(
            p[0], Eigen::Map<const Eigen::VectorXd>(s.x.data(), s.x.size()));
        const var sc = stan::math::exp(p[1]);
        var lp = 0;
        for (size_t i = 0; i < s.y.size(); ++i) {
          const var d = shifted_residual(s.y[i], mu(i));
          const bool tail = d < -sc;
          if (taken) taken->push_back(tail);
          const var base =
              tail ? -0.5 * stan::math::square(d / sc) - stan::math::log(sc)
                   : stan::math::log1p_exp(-stan::math::square(d) * sc) -
                         stan::math::log(sc);
          if (A == 0)
            lp += base + (std::signbit(A) ? -1.0 : 1.0);
          else if (std::isnan(A))
            lp += base + 2;
          else
            lp += base +
                  stan::math::log(stan::math::erfc(stan::math::fma(d, A, 3.0)));
        }
        return lp;
      };
}

const Ref ref_cf_early = [](const std::vector<var>& p, const Series& s,
                            std::vector<int>* taken) {
  const var sc = stan::math::exp(p[1]);
  var lp = 0;
  for (size_t i = 0; i < s.y.size(); ++i) {
    const var mu = p[0] * s.x[i];
    if (taken) taken->push_back(s.y[i] > mu);
    const var d = shifted_residual(s.y[i], mu);
    lp += -0.5 * stan::math::square(d / sc) - stan::math::log(sc) - 0.5 * sc;
  }
  return lp;
};

void run_folded(const std::string& stem, const std::string& extra,
                const Ref& ref, bool dead_arm_present) {
  const std::string what = stem + " [" + extra + "]";
  CompiledModel cm =
      compile_with(stem, 64, "STANLI_NO_CFG_DEAD_CONSTANTS", "1", extra);
  expect_mapped(what, cm);
  const bool erfc_compiled =
      map_calls(cm, stanli::OP_ERFC) || map_has_code(cm, stanli::Program::ERFC);
  check(erfc_compiled == dead_arm_present,
        what + (dead_arm_present ? ": live arm compiled"
                                 : ": dead arm not compiled"));
  run_points(stem, 64, udf_points, ref, true, nullptr, "0", extra);
}

void test_constant_folds_into_loop_body() {
  run_folded("region_map_cf_literal", "", ref_cf_udf, false);
  run_folded("region_map_cf_nested", "", ref_cf_udf, false);
  run_folded("region_map_cf_early", "", ref_cf_early, false);
  run_folded("region_map_cf_data", "\"A\":0", zero_probe_ref(0.0), false);
  run_folded("region_map_cf_data", "\"A\":-0.0", zero_probe_ref(-0.0), false);
  run_folded("region_map_cf_data", "\"A\":NaN",
             zero_probe_ref(std::numeric_limits<double>::quiet_NaN()), false);
  run_folded("region_map_cf_data", "\"A\":0.5", zero_probe_ref(0.5), true);

  const Ref ref_nan = [](const std::vector<var>& p, const Series& s,
                         std::vector<int>* taken) {
    const var sc = stan::math::exp(p[1]);
    var lp = 0;
    for (size_t i = 0; i < s.y.size(); ++i) {
      const var d = shifted_residual(s.y[i], p[0] * s.x[i]);
      const bool tail = d < -sc;
      if (taken) taken->push_back(tail);
      lp += (tail ? -0.5 * stan::math::square(d / sc) - stan::math::log(sc)
                  : stan::math::log1p_exp(-stan::math::square(d) * sc) -
                        stan::math::log(sc)) +
            3;
    }
    return lp;
  };
  run_folded("region_map_cf_nan", "", ref_nan, false);
}

void test_loop_variant_argument_is_not_folded() {
  std::ostringstream w;
  w.precision(17);
  w << "\"w\":[";
  for (int i = 0; i < 64; ++i)
    w << (i ? "," : "") << (i % 3 == 0 ? 0.0 : 0.2 + 0.01 * (i % 7));
  w << "]";
  const Ref ref = [](const std::vector<var>& p, const Series& s,
                     std::vector<int>* taken) {
    const var sc = stan::math::exp(p[1]);
    var lp = 0;
    for (size_t i = 0; i < s.y.size(); ++i) {
      const double wi = i % 3 == 0 ? 0.0 : 0.2 + 0.01 * (i % 7);
      const var d = shifted_residual(s.y[i], p[0] * s.x[i]);
      const bool tail = d < -sc;
      if (taken) taken->push_back(tail);
      if (wi == 0)
        lp += tail ? -0.5 * stan::math::square(d / sc) - stan::math::log(sc) -
                         0.5 * sc
                   : stan::math::log1p_exp(-stan::math::square(d) * sc) -
                         stan::math::log(sc);
      else
        lp += stan::math::log(stan::math::erfc(d * wi + 3)) - std::log(wi);
    }
    return lp;
  };
  run_folded("region_map_cf_variant", w.str(), ref, true);
  run_folded("region_map_cf_variant_o0", w.str(), ref, true);
}

void test_break_inside_unrolled_loop_in_branch() {
  const Ref ref = [](const std::vector<var>& p, const Series& s,
                     std::vector<int>* taken) {
    const var sc = stan::math::exp(p[1]);
    var lp = 0;
    for (size_t i = 0; i < s.y.size(); ++i) {
      const var d = s.y[i] - p[0] * s.x[i];
      const bool tail = d < -sc;
      if (taken) taken->push_back(tail);
      if (tail) {
        var acc = 0;
        for (int k = 1; k <= 4; ++k) {
          if (acc + k * sc > 1.5) break;
          if (k == 2) continue;
          acc += k * sc;
        }
        lp += -0.5 * stan::math::square(d / sc) - acc;
      } else {
        lp += stan::math::log1p_exp(-stan::math::square(d) * sc) -
              stan::math::log(sc);
      }
    }
    return lp;
  };
  run_points("region_map_cf_break", 64, udf_points, ref);
  run_points("region_map_cf_break", 64, {{0.3, -1.4}, {-1.5, -0.6}}, ref);
}

void test_hoisted_constants_and_saved_state() {
  for (const char* stem :
       {"region_map_branch", "region_map_udf", "region_map_call"}) {
    CompiledModel cm = compile_with(stem, 64, "STANLI_REGION_MAP_LANES");
    const auto& p = map_payload(cm);
    const std::string what(stem);
    check(!p.prologue.empty(), what + ": constants hoisted into a prologue");
    std::vector<int> body_writes((size_t)p.n_regs, 0);
    for (const auto& I : p.code)
      each_written_span(p, I, [&](int reg, int len) {
        for (int k = 0; k < len; ++k) ++body_writes[(size_t)(reg + k)];
      });
    for (const auto& I : p.prologue) {
      check(I.code == stanli::Program::CONST ||
                I.code == stanli::Program::CONSTR ||
                I.code == stanli::Program::FILL,
            what + ": prologue holds only constant stores");
      const int len = stanli::program_output_len(I);
      for (int k = 0; k < len; ++k) {
        check(body_writes[(size_t)(I.dst + k)] == 0,
              what + ": the body does not rewrite a hoisted register");
      }
    }
    for (const auto& I : p.code) {
      if (I.code != stanli::Program::CONST &&
          I.code != stanli::Program::CONSTR && I.code != stanli::Program::FILL)
        continue;
      bool rewritten = false;
      const int len = stanli::program_output_len(I);
      for (int k = 0; k < len; ++k)
        rewritten |= body_writes[(size_t)(I.dst + k)] > 1;
      check(rewritten, what + ": a constant left in the body is rewritten");
    }
    std::vector<char> saved((size_t)p.n_regs, 0);
    int cells = 0;
    for (const auto& span : p.saved)
      for (int k = 0; k < span.second; ++k) {
        saved[(size_t)(span.first + k)] = 1;
        ++cells;
      }
    for (int reg = 0; reg < p.n_regs; ++reg)
      if (body_writes[(size_t)reg] > 0)
        check(saved[(size_t)reg],
              what + ": every body-written register is saved");
    for (size_t k = 1; k < p.saved.size(); ++k)
      check(p.saved[k].first - (p.saved[k - 1].first + p.saved[k - 1].second) >
                stanli::kRegionMapSavedGap,
            what + ": saved spans separated by a small gap are merged");
    check(cells > 0 && cells == p.saved_cells,
          what + ": saved cell count matches its spans");
    for (const auto& li : p.ins) {
      if (li.len <= stanli::kRegionMapSavedGap) continue;
      for (const auto& span : p.saved)
        check(
            span.first + span.second <= li.reg || li.reg + li.len <= span.first,
            what + ": saved state skips live-in vectors");
    }
  }
}

void test_recompute_payload() {
  for (const char* stem : {"region_map_long_body", "region_map_call"}) {
    CompiledModel cm = compile_with(stem, 32, "STANLI_REGION_MAP_SAVE_LIMIT");
    const auto& p = map_payload(cm);
    check(p.recompute && p.saved.empty() && p.saved_cells == 0,
          std::string(stem) + ": over the save limit the map recomputes");
  }
}

void test_call_partials_differ_per_iteration(bool recompute) {
  const int n = 64;
  std::vector<double> offsets;
  for (int i = 0; i < n; ++i) offsets.push_back(0.3 * std::sin(1.7 * i + 0.4));
  std::ostringstream json;
  json.precision(17);
  json << "{\"N\":" << n << ",\"offsets\":[";
  for (int i = 0; i < n; ++i) json << (i ? "," : "") << offsets[(size_t)i];
  json << "],\"z_data\":0.25}";
  if (recompute) test_setenv("STANLI_REGION_MAP_SAVE_LIMIT", "0", 1);
  CompiledModel cm = stanli::compile_model(
      slurp("tests/fixtures/hypergeometric_1f0.tmir.sexp"),
      DataMap::from_json(json.str()));
  if (recompute) test_unsetenv("STANLI_REGION_MAP_SAVE_LIMIT");
  expect_mapped("hypergeometric loop", cm);
  const auto& p = map_payload(cm);
  check(!p.calls.empty(), "hypergeometric map body holds a CALL");
  check(!p.prologue.empty() && p.recompute == recompute &&
            (p.lanes.active || (p.saved_cells > 0) != recompute),
        "hypergeometric map hoists constants and saves or recomputes state");

  const auto cov = [](const var& a, const var& z) {
    return stan::math::hypergeometric_1F0(a, z);
  };
  const auto ref = [&](const std::vector<var>& v) {
    const var a = v[0];
    const var z = 0.2 * stan::math::tanh(v[1]);
    var lp = -0.5 * stan::math::square(a) - 0.5 * stan::math::square(v[1]);
    lp += cov(var(0.5), var(0.25)) * a;
    lp += cov(a, var(0.25)) + cov(var(0.5), z);
    for (int i = 0; i < n; ++i)
      lp += a.val() > 0 ? cov(a, z + offsets[(size_t)i])
                        : cov(-a, z - offsets[(size_t)i]);
    return lp;
  };
  Executor ex(cm.graph);
  cm.bind(ex);
  for (const Params& point :
       {Params{0.6, 0.4}, Params{-0.8, -0.3}, Params{0.6, 0.4},
        Params{0.2, -0.9}, Params{-0.8, -0.3}}) {
    std::vector<var> v(point.begin(), point.end());
    var lp = ref(v);
    lp.grad();
    Eval want;
    want.lp = lp.val();
    for (const var& q : v) want.grad.push_back(q.adj());
    stan::math::recover_memory();
    expect_eval("hypergeometric map at " + std::to_string(point[0]),
                evaluate(ex, point), want);
  }
}

const Ref ref_logic = [](const std::vector<var>& p, const Series& s,
                         std::vector<int>* taken) {
  var lp = 0;
  const var a = p[0], b = p[1];
  for (size_t i = 0; i < s.y.size(); ++i) {
    const double y = s.y[i], x = s.x[i];
    if (b <= -5 || a > 6 || a < -6 || y > 40 || x > 40) {
      lp += -std::numeric_limits<double>::infinity();
      continue;
    }
    const var r = y - a * x;
    const var sc = stan::math::exp(b);
    const bool tail = r < -sc || r > sc;
    if (taken) taken->push_back(tail);
    var w = tail ? -0.5 * stan::math::square(r / sc) - b
                 : -stan::math::square(r) * sc;
    if (r > 0 && b > 0) w += 0.1 * a;
    lp += w;
  }
  return lp;
};

const Ref ref_short_circuit = [](const std::vector<var>& p, const Series& s,
                                 std::vector<int>* taken) {
  var lp = 0;
  const var a = p[0], b = p[1];
  for (size_t i = 0; i < s.y.size(); ++i) {
    const var r = s.y[i] - a * s.x[i];
    const bool low =
        b <= 0 || stan::math::normal_lpdf(s.y[i], a * s.x[i], b) < -5;
    if (taken) taken->push_back(low);
    lp += low ? -0.1 * stan::math::square(r) + b
              : -0.5 * stan::math::square(r) + a;
  }
  return lp;
};

const Ref ref_nan = [](const std::vector<var>& p, const Series& s,
                       std::vector<int>* taken) {
  const double nv = std::numeric_limits<double>::quiet_NaN();
  var lp = 0;
  const var a = p[0], b = p[1];
  for (size_t i = 0; i < s.y.size(); ++i) {
    const var r = s.y[i] - a * s.x[i];
    var t;
    if (r < nv || b > nv)
      t = -stan::math::square(r);
    else
      t = -0.5 * stan::math::square(r) + b;
    const bool gate = !(r >= nv) && r < b;
    if (taken) taken->push_back(gate);
    if (gate) t += 0.1 * a;
    t += (r > nv || r < -b) ? 0.2 * b : -0.1 * r;
    lp += t;
  }
  return lp;
};

void test_logic_points() {
  run_points("region_map_logic", 64, branch_points, ref_logic);
  run_points("region_map_short_circuit", 64, branch_points, ref_short_circuit);
  run_points("region_map_nan", 64, branch_points, ref_nan, true, nullptr, "0",
             "\"nv\":NaN");
}

void test_logic_compiles_to_jumps() {
  using stanli::Program;
  for (const char* stem :
       {"region_map_logic", "region_map_nan", "region_map_short_circuit"}) {
    const std::string what(stem);
    CompiledModel cm = compile_with(
        stem, 64, nullptr, "0", what == "region_map_nan" ? "\"nv\":NaN" : "");
    const auto& p = map_payload(cm);
    const auto comparison = [](Program::Code c) {
      return c == Program::GT || c == Program::GE || c == Program::LT ||
             c == Program::LE || c == Program::EQ || c == Program::NE;
    };
    std::vector<char> from_comparison((size_t)p.n_regs, 0);
    for (const auto& I : p.code)
      if (comparison(I.code)) from_comparison[(size_t)I.dst] = 1;
    int normalizations = 0;
    for (const auto& I : p.code)
      if (I.code == Program::NE && from_comparison[(size_t)I.a])
        ++normalizations;
    check(normalizations == 0, what +
                                   ": no NE normalizes a comparison result (" +
                                   std::to_string(normalizations) + ")");

    check(!p.adj.segments.empty(), what + ": the body has reverse segments");
    if (p.adj.segments.empty()) continue;
    int paths = 0, resets = 0, wide = 0;
    for (const auto& I : p.code) {
      paths += I.code == Program::PATH;
      resets += I.code == Program::CONST && I.dst == p.adj.trail;
      wide += I.code == Program::CONSTR && I.len > 1;
    }
    check(paths == (int)p.adj.segments.size(),
          what + ": one path record per reverse segment (" +
              std::to_string(paths) + " records, " +
              std::to_string(p.adj.segments.size()) + " segments)");
    check(resets == 1, what + ": one store resets the trail");
    check(wide == 0, what + ": no wide clear in the body");
  }
}

struct Run {
  bool threw = false;
  std::string what;
  Eval e;
};

Run try_evaluate(Executor& ex, const Params& p) {
  Run r;
  try {
    r.e = evaluate(ex, p);
  } catch (const std::exception& e) {
    r.threw = true;
    r.what = std::string(typeid(e).name()) + ": " + e.what();
  }
  return r;
}

bool same_bits(double a, double b) {
  if (std::isnan(a) && std::isnan(b)) return true;
  return std::memcmp(&a, &b, sizeof(double)) == 0;
}

bool grad_close(double got, double want) {
  if (std::isnan(got) || std::isnan(want))
    return std::isnan(got) && std::isnan(want);
  if (got == want) return true;
  if (std::isinf(got) || std::isinf(want)) return false;
  if (std::fabs(got - want) <= 1e-13) return true;
  return std::llabs(ulp_key(got) - ulp_key(want)) <= 10;
}

Params spread_point(size_t k, size_t n_params, double amplitude = 1.2) {
  Params p(n_params);
  for (size_t j = 0; j < n_params; ++j)
    p[j] = amplitude * std::sin(1.7 * (double)k + 2.3 * (double)j + 0.4);
  return p;
}

std::vector<Params> spread_points(size_t n_params, size_t count = 6,
                                  double amplitude = 1.2) {
  std::vector<Params> points;
  for (size_t k = 0; k < count; ++k)
    points.push_back(spread_point(k, n_params, amplitude));
  points.push_back(points.front());
  return points;
}

struct LaneOutcome {
  int threw = 0;
  int ok = 0;
};

LaneOutcome expect_lane_parity(const std::string& what,
                               const CompiledModel& lanes,
                               const CompiledModel& scalar,
                               const std::vector<Params>& points,
                               bool want_lanes) {
  const auto& pl = map_payload(lanes);
  const auto& ps = map_payload(scalar);
  check(
      pl.lanes.active == want_lanes,
      what + ": lane plan " +
          (pl.lanes.active ? "active" : "refused (" + pl.lanes.refusal + ")") +
          ", expected " + (want_lanes ? "active" : "refused"));
  check(!ps.lanes.active && ps.lanes.refusal == "off",
        what + ": STANLI_REGION_MAP_LANES=0 leaves no plan");
  Executor el(lanes.graph), es(scalar.graph);
  lanes.bind(el);
  scalar.bind(es);
  LaneOutcome out;
  for (size_t k = 0; k < points.size(); ++k) {
    const std::string at = what + " point " + std::to_string(k);
    Params point = points[k];
    if (point.size() != (size_t)el.n_params()) {
      point = spread_point(k, (size_t)el.n_params());
      for (size_t j = 0; j < points[k].size() && j < point.size(); ++j)
        point[j] = points[k][j];
    }
    const uint64_t before = stanli::region_map_lane_runs();
    const Run got = try_evaluate(el, point);
    const uint64_t after = stanli::region_map_lane_runs();
    const Run want = try_evaluate(es, point);
    check(stanli::region_map_lane_runs() == after,
          at + ": the scalar map never counts as a lane run");
    check((after > before) == pl.lanes.active,
          at + ": lane forward ran iff the plan is active");
    check(got.threw == want.threw, at + ": lanes and scalar agree on throwing");
    if (got.threw || want.threw) {
      check(got.what == want.what,
            at + ": same exception: " + got.what + " vs " + want.what);
      ++out.threw;
      continue;
    }
    ++out.ok;
    check(same_bits(got.e.lp, want.e.lp),
          at + ": lp bitwise equal (" + std::to_string(got.e.lp) + " vs " +
              std::to_string(want.e.lp) + ")");
    for (size_t i = 0; i < want.e.grad.size(); ++i)
      check(grad_close(got.e.grad[i], want.e.grad[i]),
            at + ": grad[" + std::to_string(i) + "] " +
                std::to_string(got.e.grad[i]) + " vs " +
                std::to_string(want.e.grad[i]) + " (" +
                std::to_string(std::llabs(ulp_key(got.e.grad[i]) -
                                          ulp_key(want.e.grad[i]))) +
                " ulp)");
  }
  return out;
}

struct LaneFixture {
  const char* stem;
  bool lanes;
  const char* extra;
};

std::string big_data(int n) {
  std::ostringstream out;
  out.precision(17);
  out << "\"big\":[";
  for (int i = 0; i < n; ++i)
    out << (i ? "," : "")
        << (i % 8 == 1   ? "1e300"
            : i % 8 == 2 ? "-1e300"
                         : "0");
  out << "]";
  return out.str();
}

std::vector<LaneFixture> lane_fixture_list() {
  return {
      {"region_map_branch", true, ""},
      {"region_map_udf", true, ""},
      {"region_map_density", true, ""},
      {"region_map_hurdle_param", true, ""},
      {"region_map_udf_local", true, ""},
      {"region_map_many", true, ""},
      {"region_map_alias", true, ""},
      {"region_map_local_int", true, ""},
      {"region_map_multi", true, ""},
      {"region_map_logic", true, ""},
      {"region_map_short_circuit", true, ""},
      {"region_map_nan", true, "\"nv\":NaN"},
      {"region_map_call", true, ""},
      {"region_map_long_body", true, ""},
      {"region_map_nanflag", true, ""},
      {"region_map_dup", true, ""},
      {"region_map_nested", true, ""},
      {"region_map_intops", false, ""},
      {"region_map_lse", true, ""},
      {"region_map_lse_const", true, ""},
      {"region_map_inv_logit", true, ""},
      {"region_map_sinks", true, ""},
  };
}

void test_lane_fixtures() {
  const std::vector<LaneFixture> fixtures = lane_fixture_list();
  for (const LaneFixture& f : fixtures) {
    for (int n : {37, 64, 65, 200}) {
      const std::string what = std::string(f.stem) + " N=" + std::to_string(n);
      CompiledModel lanes = compile_with(f.stem, n, nullptr, "0", f.extra);
      CompiledModel scalar =
          compile_with(f.stem, n, "STANLI_REGION_MAP_LANES", "0", f.extra);
      expect_mapped(what, lanes);
      expect_lane_parity(what, lanes, scalar, spread_points(2), f.lanes);
    }
  }
  for (int n : {37, 64, 65, 200}) {
    const std::string extra = big_data(n);
    const std::string what = "region_map_cancel N=" + std::to_string(n);
    CompiledModel lanes =
        compile_with("region_map_cancel", n, nullptr, "0", extra);
    CompiledModel scalar = compile_with("region_map_cancel", n,
                                        "STANLI_REGION_MAP_LANES", "0", extra);
    expect_mapped(what, lanes);
    expect_lane_parity(what, lanes, scalar, spread_points(2), true);
  }
}

void test_lse_range_lanes() {
  const stanli::Program::Code lse = stanli::Program::LSE_RANGE;
  const Ref ref_lse = [](const std::vector<var>& p, const Series& s,
                         std::vector<int>* taken) {
    var lp = 0;
    for (size_t i = 0; i < s.y.size(); ++i) {
      const bool race = p[0] * s.x[i] > 0.2;
      if (taken) taken->push_back(race);
      if (!race) {
        lp += -stan::math::square(s.y[i] - p[1] * s.x[i]);
        continue;
      }
      std::vector<var> t;
      for (int j = 1; j <= 5; ++j)
        t.push_back(-0.5 *
                        stan::math::square(s.y[i] - p[0] * s.x[i] * j / 5.0) *
                        stan::math::exp(p[1]) -
                    0.1 * j * p[1]);
      lp += stan::math::log_sum_exp(t);
    }
    return lp;
  };
  const Ref ref_const = [](const std::vector<var>& p, const Series& s,
                           std::vector<int>* taken) {
    var lp = 0;
    for (size_t i = 0; i < s.y.size(); ++i) {
      const bool race = p[0] * s.x[i] > 0.2;
      if (taken) taken->push_back(race);
      if (!race) {
        lp += -stan::math::square(s.y[i] - p[1] * s.x[i]);
        continue;
      }
      std::vector<var> t = {
          var(-1.5), -0.5 * stan::math::square(s.y[i] - p[0] * s.x[i]) - p[1],
          -0.5 * stan::math::square(s.y[i] + p[1] * s.x[i]) - 0.2, var(-2.0)};
      lp += stan::math::log_sum_exp(t);
    }
    return lp;
  };
  const std::vector<Params> points = {{0.3, -0.5}, {-1.5, 0.7}, {0.9, 0.2}};
  for (const char* stem : {"region_map_lse", "region_map_lse_const"}) {
    const Ref& ref =
        std::string(stem) == "region_map_lse" ? ref_lse : ref_const;
    for (int n : {37, 64, 200}) {
      const std::string what = std::string(stem) + " N=" + std::to_string(n);
      CompiledModel lanes = compile_with(stem, n);
      expect_mapped(what, lanes);
      check(map_has_code(lanes, lse), what + ": body holds LSE_RANGE");
      CompiledModel scalar =
          compile_with(stem, n, "STANLI_REGION_MAP_LANES", "0");
      expect_lane_parity(what, lanes, scalar, spread_points(2), true);
      const Series series = make_series(n);
      Executor ex(lanes.graph);
      lanes.bind(ex);
      for (size_t k = 0; k < points.size(); ++k) {
        std::vector<int> taken;
        expect_eval(what + " point " + std::to_string(k),
                    evaluate(ex, points[k]),
                    reference(ref, series, points[k], &taken));
      }
    }
  }
}

void test_inv_logit_lanes() {
  const Ref ref = [](const std::vector<var>& p, const Series& s,
                     std::vector<int>* taken) {
    var lp = 0;
    for (size_t i = 0; i < s.y.size(); ++i) {
      const var r = s.y[i] - p[0] * s.x[i];
      const bool low = r.val() < p[1].val();
      if (taken) taken->push_back(low);
      if (low)
        lp +=
            -stan::math::square(r) *
                stan::math::inv_logit(p[0] * s.x[i] + 3 * p[1]) -
            0.1 * stan::math::inv_logit(40 * (r + stan::math::inv_logit(p[1])));
      else
        lp += -0.5 * stan::math::square(r) * stan::math::inv_logit(p[1]);
    }
    return lp;
  };
  const std::vector<Params> points = {{0.3, -0.5}, {-1.5, 0.7}, {0.9, 0.2}};
  for (int n : {37, 64, 200}) {
    const std::string what = "region_map_inv_logit N=" + std::to_string(n);
    CompiledModel lanes = compile_with("region_map_inv_logit", n);
    expect_mapped(what, lanes);
    check(map_has_code(lanes, stanli::Program::INV_LOGIT),
          what + ": body holds INV_LOGIT");
    int shared = 0, per_lane = 0;
    for (const auto& I : map_payload(lanes).code)
      if (I.code == stanli::Program::INV_LOGIT)
        (map_payload(lanes).lanes.reg_slot[(size_t)I.a] < 0 ? shared
                                                            : per_lane)++;
    check(shared > 0 && per_lane > 0,
          what + ": INV_LOGIT reads both shared and per-lane registers");
    CompiledModel scalar =
        compile_with("region_map_inv_logit", n, "STANLI_REGION_MAP_LANES", "0");
    expect_lane_parity(what, lanes, scalar, spread_points(2, 8, 12.0), true);
    const Series series = make_series(n);
    Executor ex(lanes.graph);
    lanes.bind(ex);
    for (size_t k = 0; k < points.size(); ++k) {
      std::vector<int> taken;
      expect_eval(what + " point " + std::to_string(k), evaluate(ex, points[k]),
                  reference(ref, series, points[k], &taken));
    }
  }
}

struct NativeMathCode {
  stanli::Program::Code code;
  uint16_t opcode;
  const char* name;
};

const std::vector<NativeMathCode>& native_math_codes() {
  static const std::vector<NativeMathCode> codes = {
      {stanli::Program::ERFC, stanli::OP_ERFC, "erfc"},
      {stanli::Program::LOG1P, stanli::OP_LOG1P, "log1p"},
      {stanli::Program::LOG1M_EXP, stanli::OP_LOG1M_EXP, "log1m_exp"},
      {stanli::Program::INV_SQUARE, stanli::OP_INV_SQUARE, "inv_square"},
  };
  return codes;
}

void test_native_math() {
  struct Fixture {
    const char* stem;
    std::vector<size_t> used;
    double amplitude;
  };
  const std::vector<Fixture> fixtures = {
      {"region_map_native_math", {0, 1, 2, 3}, 1.2},
      {"region_map_native_domain", {1}, 2.5},
  };
  const auto& codes = native_math_codes();
  for (const Fixture& f : fixtures) {
    int threw = 0;
    for (int n : {37, 64, 65, 200}) {
      const std::string what = std::string(f.stem) + " N=" + std::to_string(n);
      CompiledModel native = compile_with(f.stem, n);
      CompiledModel off = compile_with(f.stem, n, "STANLI_NO_NATIVE_MATH", "1");
      CompiledModel scalar =
          compile_with(f.stem, n, "STANLI_REGION_MAP_LANES", "0");
      expect_mapped(what, native);
      expect_mapped(what + " (native math off)", off);
      for (size_t k : f.used) {
        const NativeMathCode& c = codes[k];
        check(map_has_code(native, c.code),
              what + ": " + c.name + " compiles to its own opcode");
        check(!map_calls(native, c.opcode),
              what + ": " + c.name + " has no CALL");
        check(!map_has_code(off, c.code),
              what + ": STANLI_NO_NATIVE_MATH leaves no " + c.name + " opcode");
        check(map_calls(off, c.opcode),
              what + ": STANLI_NO_NATIVE_MATH keeps the " + c.name + " CALL");
      }
      const auto points = spread_points(2, 8, f.amplitude);
      Executor en(native.graph), eo(off.graph);
      native.bind(en);
      off.bind(eo);
      for (size_t k = 0; k < points.size(); ++k) {
        const std::string at = what + " point " + std::to_string(k);
        const Run got = try_evaluate(en, points[k]);
        const Run want = try_evaluate(eo, points[k]);
        check(got.threw == want.threw, at + ": native and CALL agree on throw");
        if (got.threw || want.threw) {
          check(got.what == want.what,
                at + ": same exception: " + got.what + " vs " + want.what);
          ++threw;
          continue;
        }
        check(same_bits(got.e.lp, want.e.lp),
              at + ": lp bitwise equal to the CALL build");
        for (size_t i = 0; i < want.e.grad.size(); ++i)
          check(same_bits(got.e.grad[i], want.e.grad[i]),
                at + ": grad[" + std::to_string(i) +
                    "] bitwise equal to the CALL build");
      }
      expect_lane_parity(what, native, scalar, points, true);
    }
    if (std::string(f.stem) == "region_map_native_domain")
      check(threw > 0, "the domain fixture reaches a throwing point");
  }
}

void expect_tile_call_parity(const std::string& what, const CompiledModel& on,
                             const CompiledModel& off,
                             const CompiledModel& scalar,
                             const std::vector<Params>& points,
                             LaneOutcome& tally, int& batched_models) {
  const auto& pon = map_payload(on);
  check(pon.lanes.active, what + ": lane plan active");
  int callable = 0;
  for (const auto& c : pon.calls)
    if (stanli::tile_call_kind(c.opcode) != stanli::TileCallKind::None)
      ++callable;
  check(pon.lanes.tile_calls == callable,
        what + ": every density and unary call is marked (" +
            std::to_string(pon.lanes.tile_calls) + " of " +
            std::to_string(callable) + ")");
  const bool want_batched = callable > 0;
  batched_models += want_batched;
  check(map_payload(off).lanes.tile_calls == 0,
        what + ": STANLI_REGION_MAP_TILE_CALLS=0 marks nothing");
  Executor eon(on.graph), eoff(off.graph), es(scalar.graph);
  on.bind(eon);
  off.bind(eoff);
  scalar.bind(es);
  bool ran_any = false;
  size_t threw_here = 0;
  for (size_t k = 0; k < points.size(); ++k) {
    const std::string at = what + " point " + std::to_string(k);
    Params point = points[k];
    if (point.size() != (size_t)eon.n_params()) {
      point = spread_point(k, (size_t)eon.n_params());
      for (size_t j = 0; j < points[k].size() && j < point.size(); ++j)
        point[j] = points[k][j];
    }
    const uint64_t before = stanli::region_map_tile_call_runs();
    const Run got = try_evaluate(eon, point);
    const uint64_t after = stanli::region_map_tile_call_runs();
    const Run per_lane = try_evaluate(eoff, point);
    const Run want = try_evaluate(es, point);
    check(stanli::region_map_tile_call_runs() == after,
          at + ": per-lane calls never count as tile calls");
    ran_any = ran_any || after > before;
    check(got.threw == per_lane.threw && got.threw == want.threw,
          at + ": the three paths agree on throwing");
    if (got.threw || per_lane.threw || want.threw) {
      check(got.what == per_lane.what && got.what == want.what,
            at + ": same exception: " + got.what + " | " + per_lane.what +
                " | " + want.what);
      ++tally.threw;
      ++threw_here;
      continue;
    }
    ++tally.ok;
    check(same_bits(got.e.lp, per_lane.e.lp) && same_bits(got.e.lp, want.e.lp),
          at + ": lp bitwise equal");
    for (size_t i = 0; i < want.e.grad.size(); ++i)
      check(same_bits(got.e.grad[i], per_lane.e.grad[i]) &&
                same_bits(got.e.grad[i], want.e.grad[i]),
            at + ": grad[" + std::to_string(i) + "] bitwise equal (" +
                std::to_string(got.e.grad[i]) + " | " +
                std::to_string(per_lane.e.grad[i]) + " | " +
                std::to_string(want.e.grad[i]) + ")");
  }
  check(threw_here == points.size() || ran_any == want_batched,
        what + ": a batched kernel call ran iff a call is marked");
}

void test_tile_call_parity() {
  std::vector<LaneFixture> fixtures;
  for (const LaneFixture& f : lane_fixture_list())
    if (f.lanes) fixtures.push_back(f);
  fixtures.push_back({"region_map_calldomain", true, ""});
  LaneOutcome tally;
  int batched_models = 0;
  for (const LaneFixture& f : fixtures)
    for (int n : {37, 64, 65, 200}) {
      const std::string what =
          std::string(f.stem) + " tile calls N=" + std::to_string(n);
      CompiledModel on = compile_with(f.stem, n, nullptr, "0", f.extra);
      CompiledModel off =
          compile_with(f.stem, n, "STANLI_REGION_MAP_TILE_CALLS", "0", f.extra);
      CompiledModel scalar =
          compile_with(f.stem, n, "STANLI_REGION_MAP_LANES", "0", f.extra);
      expect_tile_call_parity(what, on, off, scalar, spread_points(2, 6), tally,
                              batched_models);
    }
  check(tally.ok > 0, "tile call parity evaluates some points");
  check(tally.threw > 0, "tile call parity covers throwing points");
  check(batched_models >= 16, "tile calls run in several fixtures (" +
                                  std::to_string(batched_models) + ")");
}

void test_lane_tile_recompute() {
  CompiledModel big = compile_with("region_map_long_body", 200);
  const auto& b = map_payload(big);
  check(b.lanes.active && !b.lanes.tile_recompute,
        "a large body keeps every tile's forward state while it fits");
  const std::string limit =
      std::to_string(2 * (int64_t)stanli::kRegionMapTile *
                     (b.lanes.fwd_regs + b.lanes.adj_cells));
  CompiledModel tight =
      compile_with("region_map_long_body", 200, "STANLI_REGION_MAP_SAVE_LIMIT",
                   limit.c_str());
  const auto& t = map_payload(tight);
  check(t.lanes.active && t.lanes.tile_recompute,
        "a body whose tiles exceed the save limit recomputes tiles");
  check(t.lanes.storage == (int64_t)stanli::kRegionMapTile *
                               (t.lanes.fwd_regs + t.lanes.adj_cells),
        "tile recompute keeps one forward tile and one adjoint tile");
  CompiledModel small = compile_with("region_map_branch", 64);
  const auto& s = map_payload(small);
  check(s.lanes.active && !s.lanes.tile_recompute,
        "a small body keeps every tile's forward state");
  CompiledModel forced =
      compile_with("region_map_branch", 200, "STANLI_REGION_MAP_TILE_CELLS");
  check(map_payload(forced).lanes.tile_recompute,
        "the threshold override forces tile recompute");

  test_setenv("STANLI_REGION_MAP_TILE_CELLS", "0", 1);
  for (const LaneFixture& f : lane_fixture_list()) {
    if (!f.lanes) continue;
    for (int n : {37, 64, 65, 200}) {
      const std::string what =
          std::string(f.stem) + " tile recompute N=" + std::to_string(n);
      CompiledModel lanes = compile_with(f.stem, n, nullptr, "0", f.extra);
      CompiledModel scalar =
          compile_with(f.stem, n, "STANLI_REGION_MAP_LANES", "0", f.extra);
      expect_mapped(what, lanes);
      check(map_payload(lanes).lanes.tile_recompute,
            what + ": tile recompute selected");
      expect_lane_parity(what, lanes, scalar, spread_points(2), true);
    }
  }
  for (int n : {37, 64, 200}) {
    const std::string extra = big_data(n);
    const std::string what =
        "region_map_cancel tile recompute N=" + std::to_string(n);
    CompiledModel lanes =
        compile_with("region_map_cancel", n, nullptr, "0", extra);
    CompiledModel scalar = compile_with("region_map_cancel", n,
                                        "STANLI_REGION_MAP_LANES", "0", extra);
    expect_lane_parity(what, lanes, scalar, spread_points(2), true);
  }
  for (int n : {37, 64, 200}) {
    const std::string what =
        "region_map_oob tile recompute N=" + std::to_string(n);
    CompiledModel lanes = compile_with("region_map_oob", n);
    CompiledModel scalar =
        compile_with("region_map_oob", n, "STANLI_REGION_MAP_LANES", "0");
    const LaneOutcome r =
        expect_lane_parity(what, lanes, scalar, spread_points(2, 3), true);
    check(r.threw == 4 && r.ok == 0,
          what + ": out of range index throws every time");
  }
  for (int n : {37, 65, 200}) {
    const std::string what =
        "region_map_calldomain tile recompute N=" + std::to_string(n);
    CompiledModel lanes = compile_with("region_map_calldomain", n);
    CompiledModel scalar = compile_with("region_map_calldomain", n,
                                        "STANLI_REGION_MAP_LANES", "0");
    expect_lane_parity(what, lanes, scalar, spread_points(2, 3), true);
  }
  test_unsetenv("STANLI_REGION_MAP_TILE_CELLS");
}

void test_lane_seed_and_clean() {
  for (const LaneFixture& f : lane_fixture_list()) {
    if (!f.lanes) continue;
    CompiledModel lanes = compile_with(f.stem, 64, nullptr, "0", f.extra);
    check(map_payload(lanes).lanes.seed_regs.empty(),
          std::string(f.stem) + ": only the iteration register is seeded");
  }
  test_setenv("STANLI_REGION_MAP_CHECK_CLEAN", "1", 1);
  for (const char* tile_cells : {"1000000000", "0"}) {
    test_setenv("STANLI_REGION_MAP_TILE_CELLS", tile_cells, 1);
    for (const LaneFixture& f : lane_fixture_list()) {
      if (!f.lanes) continue;
      for (int n : {37, 64, 65, 200}) {
        const std::string what = std::string(f.stem) +
                                 " clean sweep N=" + std::to_string(n) +
                                 " tile cells " + tile_cells;
        CompiledModel lanes = compile_with(f.stem, n, nullptr, "0", f.extra);
        CompiledModel scalar =
            compile_with(f.stem, n, "STANLI_REGION_MAP_LANES", "0", f.extra);
        expect_lane_parity(what, lanes, scalar, spread_points(2), true);
      }
    }
  }
  test_unsetenv("STANLI_REGION_MAP_TILE_CELLS");
  test_unsetenv("STANLI_REGION_MAP_CHECK_CLEAN");
}

void test_lane_adjoint_clear() {
  for (const LaneFixture& f : lane_fixture_list()) {
    if (!f.lanes) continue;
    for (const char* tile_cells : {"1000000000", "0"}) {
      test_setenv("STANLI_REGION_MAP_TILE_CELLS", tile_cells, 1);
      const std::string what =
          std::string(f.stem) + " adjoint zeroing, tile cells " + tile_cells;
      CompiledModel cm =
          compile_with(f.stem, 130, "STANLI_REGION_MAP_PROFILE", "1", f.extra);
      const auto& p = map_payload(cm);
      check(p.lanes.active && p.lanes.profile != nullptr, what + ": lanes");
      if (!p.lanes.active || !p.lanes.profile) continue;
      Executor ex(cm.graph);
      cm.bind(ex);
      int swept = 0;
      for (int k = 0; k < 4; ++k) {
        try {
          evaluate(ex, spread_point((size_t)k, (size_t)ex.n_params()));
          ++swept;
        } catch (const std::exception&) {
        }
      }
      const auto& t = p.lanes.profile->total;
      check(swept > 0 && t.adj_exec > 0, what + ": backward ran");
      check(t.adj_cleared == 0, what +
                                    ": no adjoint cell zeroed per backward (" +
                                    std::to_string(t.adj_cleared) + " over " +
                                    std::to_string(swept) + " gradients)");
    }
  }
  test_unsetenv("STANLI_REGION_MAP_TILE_CELLS");
}

void test_lane_adjoint_sequence() {
  for (const LaneFixture& f : lane_fixture_list()) {
    if (!f.lanes) continue;
    for (int n : {37, 65, 200}) {
      for (const char* tile_cells : {"1000000000", "0"}) {
        test_setenv("STANLI_REGION_MAP_TILE_CELLS", tile_cells, 1);
        const std::string what = std::string(f.stem) +
                                 " 24 points N=" + std::to_string(n) +
                                 " tile cells " + tile_cells;
        CompiledModel lanes = compile_with(f.stem, n, nullptr, "0", f.extra);
        CompiledModel scalar =
            compile_with(f.stem, n, "STANLI_REGION_MAP_LANES", "0", f.extra);
        std::vector<Params> points = spread_points(2, 12, 2.0);
        for (const Params& q : spread_points(2, 12, 0.6)) points.push_back(q);
        expect_lane_parity(what, lanes, scalar, points, true);
      }
    }
  }
  test_unsetenv("STANLI_REGION_MAP_TILE_CELLS");
}

void test_map_selection_follows_data_through_udfs() {
  CompiledModel data_branch = compile_with("region_map_hurdle_data", 64);
  CompiledModel unmapped =
      compile_with("region_map_hurdle_data", 64, "STANLI_REGION_MAP");
  check(count_opcode(data_branch, OP_REGION_MAP) == 0,
        "a UDF branching on a data argument does not select the map");
  check(same_graph_structure(data_branch, unmapped) &&
            data_branch.fills == unmapped.fills,
        "a UDF branching on a data argument lowers as with the map off");
  expect_mapped("udf branch on a parameter-derived argument",
                compile_with("region_map_hurdle_param", 64));
  expect_mapped("udf branch on a local derived from a parameter argument",
                compile_with("region_map_udf_local", 64));
}

void test_lane_flag_clear_segments() {
  for (int n : {37, 64, 65}) {
    const std::string what = "region_map_nanflag N=" + std::to_string(n);
    CompiledModel lanes = compile_with("region_map_nanflag", n);
    CompiledModel scalar =
        compile_with("region_map_nanflag", n, "STANLI_REGION_MAP_LANES", "0");
    expect_mapped(what, lanes);
    const std::vector<Params> points = {
        {0.9, 1.3}, {-0.4, 0.8}, {1.7, -0.6}, {0.0, 0.0}, {1.1, 0.0}};
    const LaneOutcome r = expect_lane_parity(what, lanes, scalar, points, true);
    check(r.ok == (int)points.size(), what + ": every point evaluates");
    Executor ex(lanes.graph);
    lanes.bind(ex);
    const Eval e = evaluate(ex, points[0]);
    check(std::isfinite(e.lp), what + ": lp finite");
    for (double g : e.grad)
      check(std::isfinite(g),
            what + ": gradient finite (flag-clear lanes stay out)");
  }
}

void test_lane_cancellation_bitwise() {
  const int n = 200;
  const std::string extra = big_data(n);
  CompiledModel lanes =
      compile_with("region_map_cancel", n, nullptr, "0", extra);
  CompiledModel scalar = compile_with("region_map_cancel", n,
                                      "STANLI_REGION_MAP_LANES", "0", extra);
  check(map_payload(lanes).lanes.active, "cancellation model batches");
  Executor el(lanes.graph), es(scalar.graph);
  lanes.bind(el);
  scalar.bind(es);
  std::set<double> distinct;
  for (const Params& p : spread_points(2, 8, 0.9)) {
    const Eval a = evaluate(el, p), b = evaluate(es, p);
    check(same_bits(a.lp, b.lp), "cancellation lp bitwise");
    distinct.insert(a.lp);
  }
  check(distinct.size() > 2, "cancellation lp varies across points");
}

void test_lane_exceptions() {
  for (int n : {37, 64, 200}) {
    const std::string what = "region_map_oob N=" + std::to_string(n);
    CompiledModel lanes = compile_with("region_map_oob", n);
    CompiledModel scalar =
        compile_with("region_map_oob", n, "STANLI_REGION_MAP_LANES", "0");
    const LaneOutcome r =
        expect_lane_parity(what, lanes, scalar, spread_points(2, 3), true);
    check(r.threw == 4 && r.ok == 0,
          what + ": out of range index throws every time");
    const std::string mapped = outcome("region_map_oob", n, nullptr);
    check(mapped.find("structured index out of range") != std::string::npos,
          what + ": lane exception is the structured message: " + mapped);
  }
  for (int n : {37, 64, 65, 200}) {
    const std::string what = "region_map_calldomain N=" + std::to_string(n);
    CompiledModel lanes = compile_with("region_map_calldomain", n);
    CompiledModel scalar = compile_with("region_map_calldomain", n,
                                        "STANLI_REGION_MAP_LANES", "0");
    expect_mapped(what, lanes);
    std::vector<Params> points = spread_points(2, 8, 1.0);
    points.push_back({0.3, 0.0});
    points.push_back({-0.2, -0.8});
    const LaneOutcome r = expect_lane_parity(what, lanes, scalar, points, true);
    check(r.threw > 0,
          what + ": some points raise a domain error inside a CALL");
  }
}

void test_lane_duplicate_live_in() {
  CompiledModel cm = compile_with("region_map_dup", 64);
  const auto& p = map_payload(cm);
  int duplicates = 0;
  for (size_t i = 0; i + 1 < p.ins.size(); ++i)
    for (size_t j = i + 1; j + 1 < p.ins.size(); ++j)
      if (p.ins[i].input != p.ins[j].input && p.ins[i].len == p.ins[j].len &&
          p.ins[i].len > 1)
        ++duplicates;
  check(duplicates > 0, "equal vectors arrive as two live-ins");
}

void test_lane_guard_masks() {
  for (const char* stem :
       {"region_map_branch", "region_map_udf", "region_map_logic",
        "region_map_short_circuit", "region_map_nested"}) {
    const std::string what = std::string(stem) + " guard masks";
    CompiledModel cm =
        compile_with(stem, 130, "STANLI_REGION_MAP_PROFILE", "1");
    const auto& p = map_payload(cm);
    check(p.lanes.active && p.lanes.profile != nullptr, what + ": plan active");
    if (!p.lanes.active || !p.lanes.profile) continue;
    check(!p.adj.segments.empty(), what + ": the adjoint has segments");
    check(p.lanes.segment_block.size() == p.adj.segments.size(),
          what + ": every segment maps to a forward block");
    for (size_t s = 0; s < p.lanes.segment_block.size(); ++s) {
      const int b = p.lanes.segment_block[s];
      check(b >= 0 && (size_t)b < p.lanes.blocks.size(),
            what + ": segment " + std::to_string(s) +
                " is recorded by a lane block");
    }
    for (size_t k = 0; k <= p.adj.segments.size(); ++k)
      check(p.lanes.reg_slot[(size_t)p.adj.trail + k] < 0,
            what + ": a trail register takes no tile storage");
    Executor ex(cm.graph);
    cm.bind(ex);
    for (int k = 0; k < 3; ++k) evaluate(ex, spread_point((size_t)k, 2));
    const auto& t = p.lanes.profile->total;
    check(t.fwd_exec > 0 && t.adj_exec > 0, what + ": executions counted");
  }
}

bool pure_clear_code(stanli::Program::Code c) {
  using P = stanli::Program;
  return c == P::CONST || c == P::GT || c == P::GE || c == P::LT ||
         c == P::LE || c == P::EQ || c == P::IADD || c == P::IMOD ||
         c == P::INEG || c == P::FILL || c == P::CONSTR;
}

void test_lane_adjoint_clears() {
  for (const char* stem :
       {"region_map_branch", "region_map_udf", "region_map_logic",
        "region_map_short_circuit", "region_map_nested"}) {
    const std::string what = std::string(stem) + " adjoint clears";
    CompiledModel cm = compile_with(stem, 130);
    const auto& p = map_payload(cm);
    check(p.lanes.active, what + ": plan active");
    if (!p.lanes.active) continue;
    check(p.lanes.adj_skip.size() == p.adj.code.size(),
          what + ": one skip flag per adjoint instruction");
    if (p.lanes.adj_skip.size() != p.adj.code.size()) continue;
    std::vector<char> read((size_t)p.adj.n_regs, 0);
    for (const auto& A : p.adj.code) {
      if (pure_clear_code(A.code)) continue;
      if (A.code == stanli::Program::CALL) {
        const auto& call = p.calls[(size_t)A.a];
        for (int k = 0; k < call.out_len; ++k)
          read[(size_t)(call.bwd_adj_out + k)] = 1;
      } else {
        read[(size_t)A.dst] = 1;
      }
    }
    int skipped = 0;
    for (size_t pc = 0; pc < p.adj.code.size(); ++pc) {
      const auto& A = p.adj.code[pc];
      if (!p.lanes.adj_skip[pc]) continue;
      ++skipped;
      check(pure_clear_code(A.code), what + ": only pure clears are skipped");
      if (A.code == stanli::Program::FILL || A.code == stanli::Program::CONSTR)
        continue;
      check(!read[(size_t)A.dst],
            what + ": a skipped clear targets a cell no rule reads");
    }
    check(skipped > 0, what + ": some clears are skipped");
  }
}

void test_lane_profile_counters() {
  CompiledModel off = compile_with("region_map_logic", 130);
  check(!map_payload(off).lanes.profile, "no profile without the variable");
  CompiledModel cm =
      compile_with("region_map_logic", 130, "STANLI_REGION_MAP_PROFILE", "1");
  const auto& p = map_payload(cm);
  check(p.lanes.active && p.lanes.profile != nullptr,
        "STANLI_REGION_MAP_PROFILE attaches a profile to an active plan");
  if (!p.lanes.profile) return;
  Executor ex(cm.graph);
  cm.bind(ex);
  const int evaluations = 4;
  for (int k = 0; k < evaluations; ++k)
    evaluate(ex, spread_point((size_t)k, 2));
  const auto& t = p.lanes.profile->total;
  check(t.evaluations == (uint64_t)evaluations,
        "one forward counted per evaluation");
  check(t.tiles == (uint64_t)evaluations * (uint64_t)p.lanes.tiles,
        "tiles counted per evaluation");
  check(t.block_full + t.block_partial + t.block_empty ==
            t.tiles * p.lanes.blocks.size(),
        "every block is entered, partially or not at all, once per tile");
  check(t.fwd_exec > 0 && t.adj_exec > 0, "instructions counted both ways");
  check(t.fwd_lanes <= t.fwd_exec * (uint64_t)stanli::kRegionMapTile &&
            t.adj_lanes <= t.adj_exec * (uint64_t)stanli::kRegionMapTile,
        "active lanes never exceed a full tile per execution");
  check(t.fwd_invariant <= t.fwd_exec, "invariant executions are a subset");
  uint64_t by_op = 0;
  for (int c = 0; c < stanli::kRegionMapProfileCodes; ++c) by_op += t.fwd_op[c];
  check(by_op == t.fwd_exec, "the per-opcode split sums to the total");
}

void test_lane_copied_executors_interleaved() {
  for (const char* stem : {"region_map_branch", "region_map_logic"}) {
    const std::string what = std::string(stem) + " copies";
    CompiledModel cm = compile_with(stem, 65);
    check(map_payload(cm).lanes.active, what + ": plan active");
    Executor a(cm.graph);
    cm.bind(a);
    Executor b(a);
    Executor fresh_a(cm.graph), fresh_b(cm.graph);
    cm.bind(fresh_a);
    cm.bind(fresh_b);
    const Params pa = spread_point(0, 2), pb = spread_point(3, 2);
    for (int round = 0; round < 3; ++round) {
      const Eval ea = evaluate(a, pa);
      const Eval eb = evaluate(b, pb);
      const Eval fa = evaluate(fresh_a, pa);
      const Eval fb = evaluate(fresh_b, pb);
      check(same_bits(ea.lp, fa.lp) && ea.grad == fa.grad,
            what + ": copy A matches a fresh executor");
      check(same_bits(eb.lp, fb.lp) && eb.grad == fb.grad,
            what + ": copy B matches a fresh executor");
    }
  }
}

void test_hoisted_shared_scalar_mixture() {
  const int n = 64;
  CompiledModel cm = compile_with("partition_hoisted_mix", n);
  check(count_opcode(cm, stanli::OP_LSE2) == 1,
        "mixture over a shared probability fuses to one log_sum_exp");
  check(cm.graph.ops.size() < 40, "mixture graph does not grow with N");
  const Series s = make_series(n);
  const Ref ref = [&](const std::vector<var>& v, const Series& d,
                      std::vector<int>*) {
    using namespace stan::math;
    const var mu = v[0];
    const var sigma = exp(v[1]);
    const var p = inv_logit(v[2]);
    var lp = v[1] + log(p) + log1m(p);
    for (size_t i = 0; i < d.y.size(); ++i)
      lp += log_sum_exp(log(p) + normal_lpdf(d.y[i], 0.0, 3.0),
                        log1m(p) + normal_lpdf(d.y[i], mu * d.x[i], sigma));
    return lp;
  };
  Executor ex(cm.graph);
  cm.bind(ex);
  for (const Params& p : spread_points(3, 5, 0.6)) {
    const Eval got = evaluate(ex, p);
    const Eval want = reference(ref, s, p, nullptr);
    check(grad_close(got.lp, want.lp), "mixture lp matches Stan Math");
    for (size_t i = 0; i < 3; ++i)
      check(grad_close(got.grad[i], want.grad[i]),
            "mixture gradient " + std::to_string(i) + " matches Stan Math");
  }
}

void test_data_early_return_unrolls() {
  const int n = 64;
  check(count_opcode(compile_with("data_early_return", 20000),
                     stanli::OP_LOOP) == 0,
        "data-only early return does not become a structured loop");
  CompiledModel cm = compile_with("data_early_return", n);
  const Series s = make_series(n);
  const Ref ref = [&](const std::vector<var>& v, const Series& d,
                      std::vector<int>*) {
    using namespace stan::math;
    const var sigma = exp(v[1]);
    var lp = v[1];
    for (size_t i = 0; i < d.y.size(); ++i)
      lp += normal_lpdf(d.y[i], v[0] * d.x[i], sigma);
    return lp;
  };
  Executor ex(cm.graph);
  cm.bind(ex);
  for (const Params& p : spread_points(2, 5, 0.6)) {
    const Eval got = evaluate(ex, p);
    const Eval want = reference(ref, s, p, nullptr);
    check(grad_close(got.lp, want.lp), "early-return lp matches Stan Math");
    for (size_t i = 0; i < 2; ++i)
      check(
          grad_close(got.grad[i], want.grad[i]),
          "early-return gradient " + std::to_string(i) + " matches Stan Math");
  }
}

#ifdef STANLI_TEST_STANC
struct CogmodExpectation {
  const char* stem;
  bool mapped;
  bool lanes;
};

void test_lane_cogmod() {
  const std::vector<CogmodExpectation> models = {
      {"cm_betadiscrete", false, false},
      {"cm_betagate", true, true},
      {"cm_bisa", true, true},
      {"cm_choco", true, true},
      {"cm_ddm", true, true},
      {"cm_exgaussian", true, true},
      {"cm_exwald", true, true},
      {"cm_gamma", true, true},
      {"cm_geg", true, true},
      {"cm_invgamma", true, true},
      {"cm_invgaussian", false, false},
      {"cm_invweibull", true, true},
      {"cm_lba1", true, true},
      {"cm_lba2", true, true},
      {"cm_lnr", true, true},
      {"cm_lnr_bench", true, true},
      {"cm_loggamma", true, true},
      {"cm_lognormal", true, true},
      {"cm_logstudent", true, true},
      {"cm_logweibull", true, true},
      {"cm_rdm", true, true},
      {"cm_weibull", true, true},
  };
  for (const CogmodExpectation& m : models) {
    const std::string stem = m.stem;
    const std::string mir = stanli::tooling::run_stanc_process(
        STANLI_TEST_STANC, "tests/cogmod/" + stem + ".stan");
    const DataMap data =
        DataMap::from_json_file("tests/cogmod/" + stem + ".json");
    CompiledModel lanes = stanli::compile_model(mir, data);
    test_setenv("STANLI_REGION_MAP_LANES", "0", 1);
    CompiledModel scalar = stanli::compile_model(mir, data);
    test_unsetenv("STANLI_REGION_MAP_LANES");
    const bool mapped = count_opcode(lanes, OP_REGION_MAP) > 0;
    check(mapped == m.mapped, stem + ": mapped " + (mapped ? "yes" : "no"));
    if (!mapped) continue;
    Executor probe(lanes.graph);
    lanes.bind(probe);
    const size_t n_params = (size_t)probe.n_params();
    expect_lane_parity(stem, lanes, scalar, spread_points(n_params, 6, 0.8),
                       m.lanes);
  }
}
#endif

void test_mode_policy() {
  CompiledModel big = compile_with("region_map_long_body", 32);
  const auto& b = map_payload(big);
  check(b.lanes.active && !b.recompute && b.lanes.fwd_regs > 1024,
        "a body the lane plan accepts saves in lane tiles whatever its size");
  CompiledModel forced =
      compile_with("region_map_long_body", 32, "STANLI_REGION_MAP_SAVE_LIMIT");
  const auto& f = map_payload(forced);
  check(!f.lanes.active && f.recompute,
        "a zero save limit refuses the lane plan and recomputes");
  CompiledModel off =
      compile_with("region_map_long_body", 32, "STANLI_REGION_MAP_LANES");
  const auto& o = map_payload(off);
  check(!o.lanes.active && o.recompute && o.transient.empty(),
        "lanes off gives the scalar recompute for a large body");
  CompiledModel small_off =
      compile_with("region_map_branch", 64, "STANLI_REGION_MAP_LANES");
  const auto& s = map_payload(small_off);
  check(!s.lanes.active && !s.recompute && !s.transient.empty(),
        "lanes off keeps the scalar save for a small body");
}

void test_large_body_recomputes_by_default() {
  CompiledModel cm = compile_with("region_map_long_body", 32);
  expect_mapped("long body", cm);
  const auto& p = map_payload(cm);
  check(p.recompute && p.saved.empty() && p.saved_cells == 0,
        "a body saving more than 1024 cells per iteration recomputes");
  CompiledModel small = compile_with("region_map_branch", 64);
  const auto& q = map_payload(small);
  check(!q.recompute && q.saved_cells > 0 && q.saved_cells <= 1024,
        "a small body keeps saving");
}

void test_clear_strategy_follows_mode() {
  using stanli::Program;
  const auto clears = [](const stanli::RegionMapProg& p) {
    return std::count_if(
        p.adj.code.begin(), p.adj.code.end(), [](const stanli::AdjInstr& i) {
          return i.code == Program::CONST || i.code == Program::LT ||
                 i.code == Program::GT;
        });
  };
  CompiledModel small = compile_with("region_map_branch", 64);
  CompiledModel forced =
      compile_with("region_map_branch", 64, "STANLI_REGION_MAP_SAVE_LIMIT");
  CompiledModel big = compile_with("region_map_long_body", 32);
  const auto& s = map_payload(small);
  const auto& f = map_payload(forced);
  const auto& b = map_payload(big);
  check(!s.recompute && !s.transient.empty(),
        "save mode clears the transient adjoint spans per observation");
  check(f.recompute && f.transient.empty(),
        "recompute mode has no per-observation clear list");
  check(b.recompute && b.transient.empty(),
        "a large body has no per-observation clear list");
  check(clears(f) > clears(s),
        "recompute mode keeps clears that save mode elides (" +
            std::to_string(clears(f)) + " vs " + std::to_string(clears(s)) +
            ")");
}

void test_recompute_visits_taken_blocks() {
  using stanli::Program;
  const std::string stem = "region_map_wide_branch";
  const int n = 64;
  const Series s = make_series(n);
  const Ref ref = [](const std::vector<var>& p, const Series& d,
                     std::vector<int>* taken) {
    var lp = 0;
    for (size_t i = 0; i < d.y.size(); ++i) {
      const var r = d.y[i] - p[0] * d.x[i];
      const bool low = r.val() < p[1].val();
      if (taken) taken->push_back(low);
      if (low) {
        lp += -stan::math::square(r);
        continue;
      }
      var acc = 0;
      for (int j = 1; j <= 48; ++j) {
        if (p[0].val() * j < r.val())
          acc += -0.01 * j * stan::math::square(r - p[0]);
        else
          acc -= 0.01 * p[1] * j;
      }
      lp += -0.5 * stan::math::square(r) + acc;
    }
    return lp;
  };
  const std::vector<Params> points = {
      {0.3, -0.5}, {-1.5, 0.7}, {0.3, 5.0}, {0.3, -5.0}};

  CompiledModel lanes = compile_with(stem, n);
  expect_mapped(stem + " lanes", lanes);
  check(map_payload(lanes).lanes.active, stem + ": lanes run the body");
  Executor lane_ex(lanes.graph);
  lanes.bind(lane_ex);

  for (bool recompute : {true, false}) {
    const std::string what = stem + (recompute ? " recompute" : " save");
    if (recompute) test_setenv("STANLI_REGION_MAP_SAVE_LIMIT", "0", 1);
    CompiledModel cm = compile_with(stem, n, "STANLI_REGION_MAP_LANES");
    if (recompute) test_unsetenv("STANLI_REGION_MAP_SAVE_LIMIT");
    expect_mapped(what, cm);
    const auto& p = map_payload(cm);
    check(!p.lanes.active && p.recompute == recompute, what + ": scalar mode");
    check(p.adj.segments.size() > 40,
          what + ": many reverse segments (" +
              std::to_string(p.adj.segments.size()) + ")");
    check(p.adj.trail >= 0, what + ": the taken blocks are recorded");
    size_t widest_clear = 0;
    for (const auto& I : p.code)
      if (I.code == Program::CONSTR)
        widest_clear = std::max(widest_clear, (size_t)I.len);
    check(widest_clear < p.adj.segments.size(),
          what + ": nothing clears a flag per segment (" +
              std::to_string(widest_clear) + ")");
    Executor ex(cm.graph);
    cm.bind(ex);
    for (size_t k = 0; k < points.size(); ++k) {
      const Eval got = evaluate(ex, points[k]);
      expect_eval(what + " point " + std::to_string(k), got,
                  reference(ref, s, points[k], nullptr));
      const Eval lane = evaluate(lane_ex, points[k]);
      check(same_bits(got.lp, lane.lp) && got.grad == lane.grad,
            what + " point " + std::to_string(k) + ": bitwise equal to lanes");
    }
  }
}

void test_lane_reverse_order() {
  const std::string stem = "region_map_chain";
  const int n = 64;
  const Series s = make_series(n);
  const Ref ref = [](const std::vector<var>& p, const Series& d,
                     std::vector<int>* taken) {
    var lp = 0;
    for (size_t i = 0; i < d.y.size(); ++i) {
      const var u = d.y[i] - p[0] * d.x[i];
      const bool first = u.val() < p[1].val();
      var v = first ? p[2] * stan::math::square(u) : stan::math::exp(-u) - p[1];
      const bool second = v.val() < p[2].val();
      var w = second ? stan::math::log1p(stan::math::square(v)) : p[0] * v;
      if (taken) taken->push_back(first * 2 + second);
      lp += -w - 0.5 * stan::math::square(u);
    }
    return lp;
  };
  const std::vector<Params> points = {
      {0.3, -0.2, 0.4}, {-1.2, 0.6, -0.3}, {0.8, 0.1, 1.5}, {0.0, 0.5, 0.5}};
  CompiledModel lanes = compile_with(stem, n);
  CompiledModel scalar = compile_with(stem, n, "STANLI_REGION_MAP_LANES", "0");
  expect_mapped(stem, lanes);
  const auto& plan = map_payload(lanes);
  check(plan.lanes.active, stem + ": lanes run the body");
  check(plan.adj.segments.size() >= 4,
        stem + ": a segment per block (" +
            std::to_string(plan.adj.segments.size()) + ")");
  Executor lane_ex(lanes.graph), scalar_ex(scalar.graph);
  lanes.bind(lane_ex);
  scalar.bind(scalar_ex);
  for (size_t k = 0; k < points.size(); ++k) {
    const std::string what = stem + " point " + std::to_string(k);
    const Eval want = reference(ref, s, points[k], nullptr);
    const Eval lane = evaluate(lane_ex, points[k]);
    const Eval flat = evaluate(scalar_ex, points[k]);
    expect_eval(what + " lanes", lane, want);
    expect_eval(what + " scalar", flat, want);
    check(same_bits(lane.lp, flat.lp) && lane.grad == flat.grad,
          what + ": lanes bitwise equal to scalar");
  }
}

struct CleanFixture {
  const char* stem;
  int n;
  const char* extra;
};

void test_clean_sweep() {
  const std::vector<CleanFixture> fixtures = {
      {"region_map_branch", 64, ""},
      {"region_map_udf", 64, ""},
      {"region_map_many", 64, ""},
      {"region_map_alias", 64, ""},
      {"region_map_local_int", 64, ""},
      {"region_map_call", 64, ""},
      {"region_map_multi", 64, ""},
      {"region_map_intops", 64, ""},
      {"region_map_logic", 64, ""},
      {"region_map_short_circuit", 64, ""},
      {"region_map_nan", 64, "\"nv\":NaN"},
      {"region_map_inv_logit", 64, ""},
      {"region_map_long_body", 32, ""},
      {"region_map_wide_branch", 64, ""}};
  test_setenv("STANLI_REGION_MAP_CHECK_CLEAN", "1", 1);
  for (bool recompute : {false, true}) {
    for (const auto& f : fixtures) {
      const std::string what =
          std::string(f.stem) + (recompute ? " recompute" : " save");
      if (recompute) test_setenv("STANLI_REGION_MAP_SAVE_LIMIT", "0", 1);
      CompiledModel cm = compile_with(f.stem, f.n, nullptr, "0", f.extra);
      if (recompute) test_unsetenv("STANLI_REGION_MAP_SAVE_LIMIT");
      expect_mapped(what, cm);
      check(map_payload(cm).recompute ==
                (recompute || std::string(f.stem) == "region_map_long_body"),
            what + ": mode as requested");
      Executor ex(cm.graph);
      cm.bind(ex);
      int swept = 0;
      for (int k = 0; k < 8; ++k) {
        Params point;
        for (int64_t i = 0; i < ex.n_params(); ++i)
          point.push_back(1.1 * std::sin(1.9 * k + 2.3 * i + 0.4) +
                          0.3 * (k - 3));
        try {
          evaluate(ex, point);
          ++swept;
        } catch (const std::logic_error& e) {
          const std::string what_failed = e.what();
          check(what_failed.find("region_map_check_clean") == std::string::npos,
                what + ": " + what_failed);
        } catch (const std::exception&) {
        }
      }
      check(swept >= 4,
            what + ": enough points evaluated (" + std::to_string(swept) + ")");
    }
  }
  test_unsetenv("STANLI_REGION_MAP_CHECK_CLEAN");
}

const Ref ref_nested = [](const std::vector<var>& p, const Series& s,
                          std::vector<int>* taken) {
  const var sc = stan::math::exp(p[1]);
  var lp = 0;
  for (size_t i = 0; i < s.y.size(); ++i) {
    const var mu = p[0] * s.x[i];
    const var z = (s.y[i] - mu) / sc;
    const bool tail = s.y[i] - mu < -sc;
    if (taken) taken->push_back(tail);
    lp += tail ? -0.5 * stan::math::square(z) - stan::math::log(sc)
               : stan::math::log1p_exp(-stan::math::square(z) * sc) -
                     stan::math::log(sc);
  }
  return lp;
};

int count_moves(const std::vector<stanli::Program::Instr>& code) {
  int n = 0;
  for (const auto& I : code)
    n += I.code == stanli::Program::MOV || I.code == stanli::Program::MOVR;
  return n;
}

void test_copy_propagation_in_map() {
  const std::string stem = "region_map_nested_udf";
  CompiledModel off = compile_with(stem, 64, "STANLI_NO_COPY_PROP", "1");
  CompiledModel cm = compile_with(stem, 64);
  expect_mapped(stem, cm);
  const auto& before = map_payload(off);
  const auto& after = map_payload(cm);
  check(count_moves(before.code) > 0, stem + ": inlined body has copies");
  check(count_moves(after.code) == 0, stem + ": no copies left in the body");
  check(count_moves(after.prologue) == 0, stem + ": no copies in the prologue");
  check(after.code.size() < before.code.size(), stem + ": body is shorter");
  check(after.adj.code.size() <= before.adj.code.size(),
        stem + ": adjoint is no longer");
  int adjoint_moves = 0;
  for (const auto& A : after.adj.code)
    adjoint_moves += A.code == stanli::Program::MOV;
  check(adjoint_moves == 0, stem + ": no copies in the adjoint");
  run_points(stem, 64, udf_points, ref_nested);

  CompiledModel scalar = compile_with(stem, 64, "STANLI_REGION_MAP_LANES", "0");
  check(map_payload(cm).lanes.active && !map_payload(scalar).lanes.active,
        stem + ": lane and scalar forms");
  Executor lanes_ex(cm.graph), scalar_ex(scalar.graph);
  cm.bind(lanes_ex);
  scalar.bind(scalar_ex);
  for (const Params& point : udf_points) {
    const Eval a = evaluate(lanes_ex, point);
    const Eval b = evaluate(scalar_ex, point);
    check(a.lp == b.lp && a.grad == b.grad,
          stem + ": lanes bitwise equal to the scalar map");
  }
}

}  // namespace region_map_test

int main() {
  stanli::set_packet_math(false);
  using namespace region_map_test;
  test_same_executor_two_points();
  test_copy_propagation_in_map();
  test_size_independent();
  test_refusals();
  test_shapes();
  test_local_accumulator();
  test_integer_ops_map();
  test_long_body_map();
  test_out_of_range();
  test_copied_executors();
  test_constant_folds_into_loop_body();
  test_loop_variant_argument_is_not_folded();
  test_break_inside_unrolled_loop_in_branch();
  test_hoisted_constants_and_saved_state();
  test_recompute_payload();
  test_call_partials_differ_per_iteration(false);
  test_call_partials_differ_per_iteration(true);
  test_logic_points();
  test_logic_compiles_to_jumps();
  test_lane_fixtures();
  test_lse_range_lanes();
  test_inv_logit_lanes();
  test_native_math();
  test_tile_call_parity();
  test_lane_tile_recompute();
  test_lane_seed_and_clean();
  test_lane_adjoint_clear();
  test_lane_adjoint_sequence();
  test_map_selection_follows_data_through_udfs();
  test_lane_flag_clear_segments();
  test_lane_cancellation_bitwise();
  test_lane_exceptions();
  test_lane_duplicate_live_in();
  test_lane_copied_executors_interleaved();
  test_lane_guard_masks();
  test_lane_adjoint_clears();
  test_lane_profile_counters();
  test_hoisted_shared_scalar_mixture();
  test_data_early_return_unrolls();
#ifdef STANLI_TEST_STANC
  test_lane_cogmod();
#endif
  test_mode_policy();
  test_recompute_visits_taken_blocks();
  test_lane_reverse_order();
  test_setenv("STANLI_REGION_MAP_LANES", "0", 1);
  test_large_body_recomputes_by_default();
  test_clear_strategy_follows_mode();
  test_clean_sweep();
  test_unsetenv("STANLI_REGION_MAP_LANES");
  if (failures == 0) std::printf("test_region_map OK\n");
  return failures == 0 ? 0 : 1;
}

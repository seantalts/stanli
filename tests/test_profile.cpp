// The built-in per-op profiler: opt-in accounting of where gradient time
// goes, per opcode, without touching the fast path when off.
#include <stanli/graph.hpp>
#include <stanli/optable.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

static int failures = 0;
static void expect(const char* what, bool ok) {
  if (!ok) {
    ++failures;
    std::printf("FAIL %s\n", what);
  }
}

// The opcode's calls column, or -1 if the report has no row for it. Rows are
// "%-22s %10lld %12lld %12lld %5.1f%% %12lld", so calls is the second
// whitespace-separated field; searching the report for the number anywhere
// would also match the two unbounded nanosecond columns.
static long long calls_for(const std::string& rep, const char* opcode) {
  const std::string name(opcode);
  for (size_t pos = 0; pos < rep.size();) {
    const size_t eol = rep.find('\n', pos);
    const std::string line = rep.substr(pos, eol - pos);
    pos = eol == std::string::npos ? rep.size() : eol + 1;
    const size_t a = line.find_first_not_of(' ');
    if (a == std::string::npos) continue;
    const size_t b = line.find(' ', a);
    if (b == std::string::npos || line.compare(a, b - a, name) != 0) continue;
    return std::strtoll(line.c_str() + b, nullptr, 10);
  }
  return -1;
}

int main() {
  using namespace stanli;

  Graph g;
  const int a = g.add_slot(1, /*is_param=*/true);
  const int b = g.add_slot(1, /*is_param=*/true);
  const int ea = g.add_slot(1, false);
  const int lp = g.add_slot(1, false);
  g.add_op(OP_EXP, {a}, ea);
  g.add_op(OP_ADD_N, {ea, b}, lp);
  g.result_slot = lp;

  Executor ex(std::move(g));
  *ex.param_ptr(a) = 0.3;
  *ex.param_ptr(b) = -1.1;
  double grad[2];

  // Off by default: no rows, and results are unaffected either way.
  expect("empty report when off", ex.profile_report().empty());
  const double v_off = ex.gradient(grad);
  const double da_off = grad[0];

  ex.set_profile(true);
  for (int i = 0; i < 10; ++i) ex.gradient(grad);
  expect("same value profiled", ex.gradient(grad) == v_off);
  expect("same grad profiled", grad[0] == da_off);

  const std::string rep = ex.profile_report();
  expect("report names EXP", rep.find("EXP") != std::string::npos);
  expect("report names ADD_N", rep.find("ADD_N") != std::string::npos);
  // 11 profiled gradient evaluations, one op instance of each opcode.
  expect("counts EXP calls", calls_for(rep, "OP_EXP") == 11);
  expect("counts ADD_N calls", calls_for(rep, "OP_ADD_N") == 11);
  expect("report has totals", rep.find("total") != std::string::npos);

  // Toggling off stops accumulation but keeps the collected numbers.
  ex.set_profile(false);
  const std::string before = ex.profile_report();
  ex.gradient(grad);
  expect("no growth when off", ex.profile_report() == before);

  if (failures == 0) std::printf("test_profile: all ok\n");
  return failures == 0 ? 0 : 1;
}

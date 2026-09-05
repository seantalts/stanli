// Text rendering of the graph: exact layout, determinism, and the property
// the pass dumps depend on -- deleting an op perturbs no other op line.
#include <stanli/graph.hpp>
#include <stanli/graph_print.hpp>
#include <stanli/optable.hpp>

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

static int failures = 0;
static void expect(const char* what, bool ok) {
  if (!ok) {
    ++failures;
    std::printf("FAIL %s\n", what);
  }
}

using namespace stanli;

static Graph build_graph() {
  Graph g;
  const int p = g.add_slot(1, true);      // s0
  const int d = g.add_slot(3, false);     // s1
  const int e = g.add_slot(1, false);     // s2
  const int m = g.add_slot(3, false);     // s3
  const int s = g.add_slot(1, false);     // s4
  const int r = g.add_slot(1, false);     // s5
  const int j = g.add_slot(1, false);     // s6
  g.add_op(OP_EXP, {p}, e);
  g.add_op(OP_MUL, {d, e}, m, {7, 8, 9});
  g.ops.back().variant = 0x03;
  g.add_op(OP_SUM_VEC, {m}, s);
  g.add_op(OP_ADD, {s, e}, r);
  g.ops.back().out2 = j;
  g.result_slot = r;
  return g;
}

static GraphPrintInfo build_info(
    const std::vector<std::pair<int, std::vector<double>>>& fills) {
  GraphPrintInfo info;
  info.roots = {2};
  info.target_terms = {4, 5};
  info.jac_slots = {6};
  info.views = {{"y", 1}, {"mu", 3}};
  info.fills = &fills;
  return info;
}

static const char* kGolden =
    "ops=4 slots=7 result=s5\n"
    "roots: s2\n"
    "target_terms: s4 s5\n"
    "jac_slots: s6\n"
    "views: mu=s3 y=s1\n"
    "fills: s1[3]=0.5,1.5,2.5\n"
    "s2[1] = EXP s0[1,P]\n"
    "s3[3] = MUL.v=0x03 s1[3] s2[1] idata=[7,8,9]\n"
    "s4[1] = SUM_VEC s3[3]\n"
    "s5[1] = ADD s4[1] s2[1] out2=s6\n";

static void test_exact_text() {
  const std::vector<std::pair<int, std::vector<double>>> fills = {
      {1, {0.5, 1.5, 2.5}}};
  std::string out;
  print_graph(out, build_graph(), build_info(fills));
  expect("exact text", out == kGolden);
  if (out != kGolden) std::printf("--- got ---\n%s--- end ---\n", out.c_str());
}

static void test_deterministic() {
  const std::vector<std::pair<int, std::vector<double>>> fills = {
      {1, {0.5, 1.5, 2.5}}};
  const Graph g = build_graph();
  std::string a, b;
  print_graph(a, g, build_info(fills));
  print_graph(b, g, build_info(fills));
  expect("deterministic", a == b && !a.empty());
}

static std::vector<std::string> op_lines(const std::string& text) {
  std::vector<std::string> lines;
  size_t pos = 0;
  while (pos < text.size()) {
    const size_t nl = text.find('\n', pos);
    lines.push_back(text.substr(pos, nl - pos));
    pos = nl + 1;
  }
  std::vector<std::string> ops;
  for (const std::string& l : lines)
    if (!l.empty() && l[0] == 's' && l.find(" = ") != std::string::npos)
      ops.push_back(l);
  return ops;
}

static void test_removal_is_one_line() {
  Graph g = build_graph();
  std::string before;
  print_graph(before, g);
  const std::vector<std::string> a = op_lines(before);

  g.ops.erase(g.ops.begin() + 2);
  std::string after;
  print_graph(after, g);
  const std::vector<std::string> b = op_lines(after);

  expect("one op removed", a.size() == b.size() + 1);
  size_t i = 0, k = 0, dropped = 0;
  for (; i < a.size(); ++i) {
    if (k < b.size() && a[i] == b[k]) {
      ++k;
      continue;
    }
    ++dropped;
  }
  expect("survivors byte-identical and in order", dropped == 1 && k == b.size());
  if (dropped != 1 || k != b.size())
    std::printf("--- before ---\n%s--- after ---\n%s", before.c_str(),
                after.c_str());
}

int main() {
  test_exact_text();
  test_deterministic();
  test_removal_is_one_line();
  if (failures == 0) std::printf("test_graph_print OK\n");
  return failures == 0 ? 0 : 1;
}

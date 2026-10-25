#include "graph_helpers.hpp"

#include <stanli/builtin_registry.hpp>

#include <stan/math.hpp>

#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using stan::math::var;
using stanli::Executor;
using stanli::Graph;

int failures = 0;

void expect(bool ok, const std::string& what) {
  if (ok) return;
  ++failures;
  std::printf("FAIL %s\n", what.c_str());
}

std::vector<double> weights_for(size_t n) {
  std::vector<double> w(n);
  for (size_t k = 0; k < n; ++k) w[k] = 1.0 + 0.75 * static_cast<double>(k * k);
  return w;
}

void check_sort_gradient(const std::string& tag, uint16_t opcode,
                         bool ascending, const std::vector<double>& x) {
  const size_t n = x.size();
  const std::vector<double> w = weights_for(n);

  Graph g;
  const int xs = g.add_slot(static_cast<int64_t>(n), true);
  const int ws = g.add_slot(static_cast<int64_t>(n), false);
  const int sorted = g.add_slot(static_cast<int64_t>(n), false);
  const int lp = g.add_slot(1, false);
  g.add_op(opcode, {xs}, sorted);
  g.add_op(stanli::OP_DOT, {sorted, ws}, lp);
  g.result_slot = lp;
  const stanli::testutil::Fills fills = {{ws, w}};
  const auto got = stanli::testutil::run_grad(
      std::move(g), fills, [&](int64_t i) { return x[static_cast<size_t>(i)]; });

  stan::math::nested_rev_autodiff scope;
  Eigen::Matrix<var, -1, 1> v(static_cast<Eigen::Index>(n));
  for (size_t k = 0; k < n; ++k) v(static_cast<Eigen::Index>(k)) = x[k];
  Eigen::Matrix<var, -1, 1> s =
      ascending ? Eigen::Matrix<var, -1, 1>(stan::math::sort_asc(v))
                : Eigen::Matrix<var, -1, 1>(stan::math::sort_desc(v));
  var total = 0;
  for (size_t k = 0; k < n; ++k) total += s(static_cast<Eigen::Index>(k)) * w[k];
  total.grad();

  expect(got[0] == total.val(), tag + " lp");
  for (size_t k = 0; k < n; ++k)
    expect(got[1 + k] == v(static_cast<Eigen::Index>(k)).adj(),
           tag + " grad " + std::to_string(k));
}

std::vector<double> run_ordering(stanli::BuiltinOrdering kind,
                                 const std::vector<double>& x, int64_t out_len,
                                 const std::vector<double>* position) {
  Graph g;
  const int xs = g.add_slot(static_cast<int64_t>(x.size()), false);
  const int ps = position ? g.add_slot(1, false) : -1;
  const int out = g.add_slot(out_len, false);
  const int lp = g.add_slot(1, false);
  const int op = position ? g.add_op(stanli::OP_ORDERING, {xs, ps}, out)
                          : g.add_op(stanli::OP_ORDERING, {xs}, out);
  g.ops[static_cast<size_t>(op)].variant = static_cast<uint8_t>(kind);
  g.add_op(stanli::OP_SUM_VEC, {out}, lp);
  g.result_slot = lp;
  Executor ex(std::move(g));
  for (size_t k = 0; k < x.size(); ++k) ex.value_ptr(xs)[k] = x[k];
  if (position) ex.value_ptr(ps)[0] = (*position)[0];
  double unused = 0;
  ex.gradient(&unused);
  return std::vector<double>(ex.value_ptr(out), ex.value_ptr(out) + out_len);
}

void check_ordering_values() {
  const std::vector<double> x = {2.5, -1, 2.5, 0, 7, -1};
  const std::vector<int> asc = stan::math::sort_indices<true>(x);
  const std::vector<int> desc = stan::math::sort_indices<false>(x);
  const auto got_asc =
      run_ordering(stanli::BuiltinOrdering::IndicesAsc, x, 6, nullptr);
  const auto got_desc =
      run_ordering(stanli::BuiltinOrdering::IndicesDesc, x, 6, nullptr);
  for (size_t k = 0; k < x.size(); ++k) {
    expect(got_asc[k] == asc[k], "indices asc " + std::to_string(k));
    expect(got_desc[k] == desc[k], "indices desc " + std::to_string(k));
  }
  for (int position = 1; position <= 6; ++position) {
    const std::vector<double> p = {static_cast<double>(position)};
    const auto got = run_ordering(stanli::BuiltinOrdering::Rank, x, 1, &p);
    expect(got[0] == stan::math::rank(x, position),
           "rank " + std::to_string(position));
  }
  const std::vector<double> ints = {3, -1, 3, 0, 7, -1};
  const auto sorted_up =
      run_ordering(stanli::BuiltinOrdering::SortAsc, ints, 6, nullptr);
  const auto sorted_down =
      run_ordering(stanli::BuiltinOrdering::SortDesc, ints, 6, nullptr);
  const std::vector<double> want_up = {-1, -1, 0, 3, 3, 7};
  const std::vector<double> want_down = {7, 3, 3, 0, -1, -1};
  expect(sorted_up == want_up, "int sort asc");
  expect(sorted_down == want_down, "int sort desc");
}

template <class F>
void expect_throws(const std::string& tag, const std::string& needle, F&& f) {
  try {
    f();
  } catch (const std::exception& e) {
    expect(std::string(e.what()).find(needle) != std::string::npos,
           tag + " message: " + e.what());
    return;
  }
  expect(false, tag + " did not throw");
}

void check_errors() {
  const double nan = std::nan("");
  for (uint16_t opcode : {stanli::OP_SORT_ASC, stanli::OP_SORT_DESC}) {
    expect_throws("nan sort", "sort_asc", [&] {
      stanli::testutil::run_op_sum(opcode, 3, {{1.0, nan, 2.0}}, {true});
    });
    expect_throws("nan sort data", "sort_asc", [&] {
      stanli::testutil::run_op_sum(opcode, 3, {{1.0, nan, 2.0}}, {false});
    });
  }
  const std::vector<double> bad = {4};
  expect_throws("rank range", "out of range", [&] {
    run_ordering(stanli::BuiltinOrdering::Rank, {1, 2, 3}, 1, &bad);
  });
  const auto with_nan = run_ordering(stanli::BuiltinOrdering::IndicesAsc,
                                     {1.0, nan, 2.0}, 3, nullptr);
  expect(with_nan.size() == 3, "sort_indices accepts nan");
}

void check_empty() {
  for (uint16_t opcode : {stanli::OP_SORT_ASC, stanli::OP_SORT_DESC}) {
    const auto r = stanli::testutil::run_op_sum(opcode, 0, {{}}, {true});
    expect(r.value == 0.0 && r.grad.empty(), "empty sort");
  }
}

}  // namespace

int main() {
  const std::vector<std::vector<double>> inputs = {
      {0.5}, {2.0, -1.0}, {0.5, -1.2, 2.0, 0.3, 1.1},
      {2.0, 1.0, 2.0, 0.0, 1.0, 2.0}};
  for (const auto& x : inputs) {
    const std::string n = std::to_string(x.size());
    check_sort_gradient("sort_asc n=" + n, stanli::OP_SORT_ASC, true, x);
    check_sort_gradient("sort_desc n=" + n, stanli::OP_SORT_DESC, false, x);
  }
  check_ordering_values();
  check_errors();
  check_empty();
  if (failures == 0) std::printf("test_sort OK\n");
  return failures == 0 ? 0 : 1;
}

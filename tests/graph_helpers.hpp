// Shared helper: build and run a one-op graph, returning value + gradient.
#ifndef STANLI_TESTS_GRAPH_HELPERS_HPP
#define STANLI_TESTS_GRAPH_HELPERS_HPP

#include <stanli/graph.hpp>
#include <stanli/optable.hpp>

#include <utility>
#include <vector>

namespace stanli {
namespace testutil {

// Slot fills for data and constant slots, applied after construction.
using Fills = std::vector<std::pair<int, std::vector<double>>>;

// Executes gradient at fixed params; returns {lp, grads...}. `fill_at(i)`
// picks the evaluation point and stays a per-file callable: a before/after
// pair is only a comparison if both halves run at the same point, and each
// pass test picks its own.
template <class FillAt>
inline std::vector<double> run_grad(Graph g, const Fills& fills,
                                    FillAt fill_at) {
  Executor ex(std::move(g));
  for (const auto& f : fills) {
    double* p = ex.value_ptr(f.first);
    for (size_t j = 0; j < f.second.size(); ++j) p[j] = f.second[j];
  }
  for (int64_t i = 0; i < ex.n_params(); ++i) ex.params_data()[i] = fill_at(i);
  std::vector<double> out(1 + (size_t)ex.n_params());
  out[0] = ex.gradient(out.data() + 1);
  return out;
}

// Chained ADD_N reduction, as lower.cpp's reduce_terms does.
inline void reduce_into_result(Graph& g, const std::vector<int>& terms) {
  int acc = terms[0];
  for (size_t k = 1; k < terms.size(); ++k) {
    const int s = g.add_slot(1, false);
    g.add_op(OP_ADD_N, {acc, terms[k]}, s);
    acc = s;
  }
  g.result_slot = acc;
}

struct RunResult {
  double value;
  std::vector<double> grad;
};

// One op, scalar output at result_slot. vals[i]/params[i] describe inputs.
inline RunResult run_one_op(uint16_t opcode,
                            const std::vector<std::vector<double>>& vals,
                            const std::vector<bool>& params,
                            std::vector<int> idata = {}) {
  Graph g;
  std::vector<int> slots;
  int64_t n_par = 0;
  for (size_t i = 0; i < vals.size(); ++i) {
    slots.push_back(g.add_slot((int64_t)vals[i].size(), params[i]));
    if (params[i]) n_par += (int64_t)vals[i].size();
  }
  const int lp = g.add_slot(1, false);
  Op op;
  op.opcode = opcode;
  op.out = lp;
  op.n_in = 0;
  for (int s : slots) op.in[op.n_in++] = s;
  if (!idata.empty()) {
    g.idata_pool.push_back(std::move(idata));
    op.idata = g.idata_pool.back().data();
    op.n_idata = (int64_t)g.idata_pool.back().size();
  }
  g.ops.push_back(op);
  g.result_slot = lp;

  Executor ex(std::move(g));
  for (size_t i = 0; i < vals.size(); ++i) {
    double* p = ex.value_ptr(slots[i]);
    for (size_t j = 0; j < vals[i].size(); ++j) p[j] = vals[i][j];
  }
  RunResult r;
  r.grad.assign(n_par, 0.0);
  r.value = ex.gradient(r.grad.data());
  return r;
}

// One elementwise op feeding OP_SUM_VEC so vector outputs reduce to a scalar
// lp (sum), matching a var reference of sum(f(args)).
inline RunResult run_op_sum(uint16_t opcode, int64_t out_len,
                            const std::vector<std::vector<double>>& vals,
                            const std::vector<bool>& params) {
  Graph g;
  std::vector<int> slots;
  int64_t n_par = 0;
  for (size_t i = 0; i < vals.size(); ++i) {
    slots.push_back(g.add_slot((int64_t)vals[i].size(), params[i]));
    if (params[i]) n_par += (int64_t)vals[i].size();
  }
  const int out = g.add_slot(out_len, false);
  const int lp = g.add_slot(1, false);
  Op op;
  op.opcode = opcode;
  op.out = out;
  op.n_in = 0;
  for (int s : slots) op.in[op.n_in++] = s;
  g.ops.push_back(op);
  if (out_len == 1) {
    g.add_op(OP_ADD_N, {out}, lp);
  } else {
    g.add_op(OP_SUM_VEC, {out}, lp);
  }
  g.result_slot = lp;

  Executor ex(std::move(g));
  for (size_t i = 0; i < vals.size(); ++i) {
    double* p = ex.value_ptr(slots[i]);
    for (size_t j = 0; j < vals[i].size(); ++j) p[j] = vals[i][j];
  }
  RunResult r;
  r.grad.assign(n_par, 0.0);
  r.value = ex.gradient(r.grad.data());
  return r;
}

}  // namespace testutil
}  // namespace stanli

#endif

#include <stanli/whiten.hpp>

#include <stanli/optable.hpp>

#include "checked_size.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>

namespace stanli {

void WhitenSpec::solve_lower(double* x) const {
  for (int64_t j = 0; j < n; ++j) {
    const int64_t start = col_ptr[(size_t)j];
    const int64_t end = col_ptr[(size_t)j + 1];
    x[j] /= values[(size_t)start];
    const double xj = x[j];
    for (int64_t p = start + 1; p < end; ++p)
      x[row_idx[(size_t)p]] -= values[(size_t)p] * xj;
  }
}

void WhitenSpec::solve_lower_transpose(double* x) const {
  for (int64_t j = n - 1; j >= 0; --j) {
    const int64_t start = col_ptr[(size_t)j];
    const int64_t end = col_ptr[(size_t)j + 1];
    double s = x[j];
    for (int64_t p = start + 1; p < end; ++p)
      s -= values[(size_t)p] * x[row_idx[(size_t)p]];
    x[j] = s / values[(size_t)start];
  }
}

void WhitenSpec::multiply_lower_transpose(const double* w, double* out) const {
  for (int64_t j = 0; j < n; ++j) {
    double s = 0;
    for (int64_t p = col_ptr[(size_t)j]; p < col_ptr[(size_t)j + 1]; ++p)
      s += values[(size_t)p] * w[row_idx[(size_t)p]];
    out[j] = s;
  }
}

void WhitenSpec::solve_c_transpose(double* x, double* acc) const {
  std::fill(acc, acc + k, 0.0);
  for (int64_t j = n - 1; j >= 0; --j) {
    const double* bj = cb.data() + j * k;
    const double* wj = cw.data() + j * k;
    double s = 0;
    for (int64_t r = 0; r < k; ++r) s += bj[r] * acc[r];
    const double y = (x[j] - s) / c[(size_t)j];
    x[j] = y;
    for (int64_t r = 0; r < k; ++r) acc[r] += y * wj[r];
  }
}

void WhitenSpec::solve_c(double* x, double* acc) const {
  std::fill(acc, acc + k, 0.0);
  for (int64_t j = 0; j < n; ++j) {
    const double* bj = cb.data() + j * k;
    const double* wj = cw.data() + j * k;
    double s = 0;
    for (int64_t r = 0; r < k; ++r) s += wj[r] * acc[r];
    const double y = (x[j] - s) / c[(size_t)j];
    x[j] = y;
    for (int64_t r = 0; r < k; ++r) acc[r] += y * bj[r];
  }
}

void WhitenSpec::multiply_c_transpose(double* x, double* acc) const {
  std::fill(acc, acc + k, 0.0);
  for (int64_t j = n - 1; j >= 0; --j) {
    const double* bj = cb.data() + j * k;
    const double* wj = cw.data() + j * k;
    double s = 0;
    for (int64_t r = 0; r < k; ++r) s += bj[r] * acc[r];
    const double xj = x[j];
    x[j] = c[(size_t)j] * xj + s;
    for (int64_t r = 0; r < k; ++r) acc[r] += xj * wj[r];
  }
}

void WhitenSpec::to_q(const double* z, double* out) const {
  std::vector<double> w(z, z + n);
  if (has_c()) {
    std::vector<double> acc((size_t)k);
    solve_c_transpose(w.data(), acc.data());
  }
  solve_lower_transpose(w.data());
  for (int64_t i = 0; i < n; ++i) {
    const auto at = (size_t)perm[(size_t)i];
    out[at] = q_hat[at] + w[(size_t)i];
  }
}

void WhitenSpec::to_z(const double* q, double* out) const {
  std::vector<double> w((size_t)n);
  for (int64_t i = 0; i < n; ++i) {
    const auto at = (size_t)perm[(size_t)i];
    w[(size_t)i] = q[at] - q_hat[at];
  }
  multiply_lower_transpose(w.data(), out);
  if (has_c()) {
    std::vector<double> acc((size_t)k);
    multiply_c_transpose(out, acc.data());
  }
}

std::shared_ptr<const WhitenSpec> make_whiten_spec(
    const double* q_hat, int64_t n, const int64_t* perm, const int64_t* col_ptr,
    const int64_t* row_idx, const double* values, int64_t nnz) {
  return make_whiten_spec(q_hat, n, perm, col_ptr, row_idx, values, nnz, 0,
                          nullptr, nullptr, nullptr);
}

std::shared_ptr<const WhitenSpec> make_whiten_spec(
    const double* q_hat, int64_t n, const int64_t* perm, const int64_t* col_ptr,
    const int64_t* row_idx, const double* values, int64_t nnz, int64_t k,
    const double* c, const double* b, const double* w) {
  if (n < 1) throw std::invalid_argument("whiten needs at least one parameter");
  if (nnz < n)
    throw std::invalid_argument(
        "L has fewer entries than its dimension; every column needs a diagonal "
        "entry");
  if (!q_hat || !perm || !col_ptr || !row_idx || !values)
    throw std::invalid_argument("whiten got a null array");
  auto spec = std::make_shared<WhitenSpec>();
  spec->n = n;
  spec->q_hat.assign(q_hat, q_hat + n);
  for (double v : spec->q_hat)
    if (!std::isfinite(v)) throw std::invalid_argument("q_hat must be finite");
  spec->perm.assign(perm, perm + n);
  std::vector<char> seen((size_t)n, 0);
  for (int64_t p : spec->perm) {
    if (p < 0 || p >= n || seen[(size_t)p])
      throw std::invalid_argument("perm is not a permutation of 0..n-1");
    seen[(size_t)p] = 1;
  }
  spec->col_ptr.assign(col_ptr, col_ptr + n + 1);
  spec->row_idx.assign(row_idx, row_idx + nnz);
  spec->values.assign(values, values + nnz);
  if (spec->col_ptr[0] != 0 || spec->col_ptr[(size_t)n] != nnz)
    throw std::invalid_argument("L has an inconsistent column pointer array");
  for (int64_t j = 0; j < n; ++j) {
    const int64_t start = spec->col_ptr[(size_t)j];
    const int64_t end = spec->col_ptr[(size_t)j + 1];
    const std::string col = "column " + std::to_string(j) + " of L";
    if (end < start || end > nnz)
      throw std::invalid_argument("L has an inconsistent column pointer array");
    if (end == start)
      throw std::invalid_argument(col + " has no diagonal entry");
    if (spec->row_idx[(size_t)start] != j)
      throw std::invalid_argument(
          col +
          " must start with its diagonal entry: L must be lower "
          "triangular with sorted row indices");
    for (int64_t p = start + 1; p < end; ++p) {
      const int64_t r = spec->row_idx[(size_t)p];
      if (r <= spec->row_idx[(size_t)p - 1] || r >= n)
        throw std::invalid_argument(
            col +
            " has an out-of-range or unsorted row index: L must be "
            "lower triangular with sorted row indices");
    }
    const double d = spec->values[(size_t)start];
    if (!(d != 0.0) || !std::isfinite(d))
      throw std::invalid_argument(col + " has a zero or non-finite diagonal");
    for (int64_t p = start + 1; p < end; ++p)
      if (!std::isfinite(spec->values[(size_t)p]))
        throw std::invalid_argument("L has a non-finite entry");
  }
  if (c) {
    if (k < 0) throw std::invalid_argument("the low-rank width is negative");
    if (k > 0 && (!b || !w))
      throw std::invalid_argument("whiten got a null low-rank array");
    const int64_t nk = checked_size(
        n, k, "the low-rank width k is too large for n parameters");
    spec->k = k;
    spec->c.assign(c, c + n);
    spec->cb.assign(b, b + nk);
    spec->cw.assign(w, w + nk);
    for (double v : spec->c)
      if (!(v != 0.0) || !std::isfinite(v))
        throw std::invalid_argument("c has a zero or non-finite entry");
    for (double v : spec->cb)
      if (!std::isfinite(v))
        throw std::invalid_argument("the low-rank factor B is not finite");
    for (double v : spec->cw)
      if (!std::isfinite(v))
        throw std::invalid_argument("the low-rank factor W is not finite");
  }
  return spec;
}

namespace {

const WhitenSpec& spec_of(const KernelCtx& ctx) {
  return *static_cast<const WhitenSpec*>(ctx.udata);
}

void whiten_fwd(KernelCtx& ctx) {
  const WhitenSpec& s = spec_of(ctx);
  double* w = ctx.scratch;
  std::copy_n(ctx.in[0].data, s.n, w);
  if (s.has_c()) s.solve_c_transpose(w, w + s.n);
  s.solve_lower_transpose(w);
  for (int64_t i = 0; i < s.n; ++i) {
    const auto at = (size_t)s.perm[(size_t)i];
    ctx.out.data[at] = s.q_hat[at] + w[i];
  }
}

void whiten_bwd(KernelCtx& ctx) {
  if (!ctx.in_adj[0].data) return;
  const WhitenSpec& s = spec_of(ctx);
  double* w = ctx.scratch;
  for (int64_t i = 0; i < s.n; ++i)
    w[i] = ctx.out_adj_vec.data[s.perm[(size_t)i]];
  s.solve_lower(w);
  if (s.has_c()) s.solve_c(w, w + s.n);
  for (int64_t i = 0; i < s.n; ++i) ctx.in_adj[0].data[i] += w[i];
}

int64_t whiten_scratch(const Op& op, const Slot*) {
  const auto* s = static_cast<const WhitenSpec*>(op.udata);
  return s->n + s->k;
}

}  // namespace

void register_whiten_kernel() {
  register_kernel(OP_AFFINE_WHITEN,
                  Kernel{whiten_fwd, whiten_bwd, whiten_scratch, nullptr});
}

Graph whiten_graph(const Graph& inner, std::shared_ptr<const WhitenSpec> spec) {
  Graph g(inner);
  std::vector<int> params;
  int64_t total = 0;
  for (size_t i = 0; i < g.slots.size(); ++i)
    if (g.slots[i].is_param) {
      params.push_back((int)i);
      total += g.slots[i].len;
    }
  if (total != spec->n)
    throw std::invalid_argument(
        "the model has " + std::to_string(total) +
        " unconstrained parameters but the whitening is for " +
        std::to_string(spec->n));
  const size_t before = g.ops.size();
  const int z = g.add_slot(spec->n, true);
  const int q = g.add_slot(spec->n, false);
  const int whiten = g.add_op(OP_AFFINE_WHITEN, {z}, q);
  g.udata_pool.push_back(std::const_pointer_cast<WhitenSpec>(spec));
  g.ops[(size_t)whiten].udata = spec.get();
  int64_t offset = 0;
  for (int slot : params) {
    const int64_t len = g.slots[(size_t)slot].len;
    g.slots[(size_t)slot].is_param = false;
    if (len > 0) g.add_op(OP_SLICE, {q}, slot, {(int)offset});
    offset += len;
  }
  std::rotate(g.ops.begin(), g.ops.begin() + (std::ptrdiff_t)before,
              g.ops.end());
  return g;
}

}  // namespace stanli

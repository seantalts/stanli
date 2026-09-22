#include <stanli/gp_cov_fusion.hpp>
#include <stanli/optable.hpp>

#include <algorithm>
#include <cstdlib>

namespace stanli {

int fuse_gp_diagonal_updates(Graph& g, const std::vector<int>& roots) {
  const auto candidate = [](const Op& op) {
    // Exp-quad has an upstream matrix callback with distinct output nodes.
    return op.opcode == OP_GP_COV && op.n_in == 3 && op.variant != kGpExpQuad &&
           op.n_idata == 2 && op.idata[0] > 0;
  };
  if (std::getenv("STANLI_NO_GP_DIAGONAL_FUSION") ||
      std::none_of(g.ops.begin(), g.ops.end(), candidate))
    return 0;

  std::vector<int> uses(g.slots.size(), 0), writes(g.slots.size(), 0);
  for (const Op& op : g.ops) {
    for (int k = 0; k < op.n_in; ++k)
      if (op.in[k] >= 0) ++uses[(size_t)op.in[k]];
    if (op.out >= 0) ++writes[(size_t)op.out];
    if (op.out2 >= 0) ++writes[(size_t)op.out2];
  }
  for (int root : roots)
    if (root >= 0) ++uses[(size_t)root];
  if (g.result_slot >= 0) ++uses[(size_t)g.result_slot];

  std::vector<Op> result;
  result.reserve(g.ops.size());
  int fused = 0;
  for (size_t i = 0; i < g.ops.size();) {
    const Op& covariance = g.ops[i];
    const int64_t n = candidate(covariance) ? covariance.idata[0] : 0;
    bool valid = n > 0 && n <= (g.ops.size() - i - 1) / 3;
    int current = covariance.out, jitter = -1;
    for (int64_t d = 0; valid && d < n; ++d) {
      const Op& read = g.ops[i + 1 + 3 * d];
      const Op& add = g.ops[i + 2 + 3 * d];
      const Op& write = g.ops[i + 3 + 3 * d];
      const int64_t diagonal = d * (n + 1);
      valid = current >= 0 && uses[(size_t)current] == 2 &&
              writes[(size_t)current] == 1 && read.opcode == OP_INDEX &&
              read.n_in == 1 && read.in[0] == current && read.n_idata == 1 &&
              read.idata[0] == diagonal && read.out2 < 0 &&
              add.opcode == OP_ADD && add.n_in == 2 && add.in[0] == read.out &&
              add.out2 < 0 && write.opcode == OP_SET_INDEX && write.n_in == 2 &&
              write.in[0] == current && write.in[1] == add.out &&
              write.out != current && write.out2 < 0 && write.n_idata == 1 &&
              write.idata[0] == diagonal;
      if (!valid) break;
      if (jitter < 0) jitter = add.in[1];
      valid = add.in[1] == jitter && g.slots[(size_t)jitter].len == 1 &&
              g.slots[(size_t)read.out].len == 1 &&
              g.slots[(size_t)add.out].len == 1 &&
              uses[(size_t)read.out] == 1 && uses[(size_t)add.out] == 1 &&
              writes[(size_t)read.out] == 1 && writes[(size_t)add.out] == 1;
      current = write.out;
    }
    if (!valid) {
      result.push_back(covariance);
      ++i;
      continue;
    }
    Op combined = covariance;
    combined.in[combined.n_in++] = jitter;
    combined.out = current;
    result.push_back(combined);
    // Every removed intermediate has one writer and only the checked uses.
    for (size_t j = i; j < i + 3 * n; ++j)
      g.slots[(size_t)g.ops[j].out].len = 0;
    i += 1 + 3 * n;
    ++fused;
  }
  if (fused) g.ops = std::move(result);
  return fused;
}

}  // namespace stanli

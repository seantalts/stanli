// Fixed input-range planning for independent callback graphs.
#pragma once
#include <stanli/graph.hpp>
#include <stanli/optable.hpp>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace stanli::detail {
struct Import {
  int64_t parent, local, len;
  bool exclusive = false;
};
using Fills = std::vector<std::pair<int, std::vector<double>>>;

// A source slot has one import, even when several reads alias or overlap.
// Keep its convex hull: no arithmetic, effects, or reverse additions move.
// Unsupported graphs return false before modifying graph, fills, or imports.
inline bool compact_imports(stanli::Graph& graph, Fills& fills,
                            std::vector<Import>& imports,
                            std::vector<int64_t>* source_offsets = nullptr,
                            std::string* refusal = nullptr) {
  using namespace stanli;
  const auto reject = [&](const std::string& reason) {
    if (refusal) *refusal = reason;
    return false;
  };
  if (refusal) *refusal = "unsupported child import range";
  const size_t count = graph.slots.size();
  const auto valid = [count](int s) { return s >= 0 && size_t(s) < count; };
  if (!valid(graph.result_slot)) return false;
  std::vector<char> written(count, false);
  std::vector<int64_t> lo(count), hi(count, 0);
  int64_t parameter_size = 0;
  for (size_t s = 0; s < count; ++s) {
    const auto& slot = graph.slots[s];
    if (slot.len < 0) return false;
    if (slot.is_param) {
      if (slot.len > std::numeric_limits<int64_t>::max() - parameter_size)
        return false;
      parameter_size += slot.len;
    }
    lo[s] = graph.slots[s].len;
  }
  for (const auto& op : graph.ops) {
    const auto* k = find_kernel(op.opcode);
    // BoundCheckSpec contains only immutable text/booleans, never slot IDs.
    // Keep these checks in each child, including nested-reduction grainsize
    // validation. Their full input reads prevent unsafe range trimming.
    const bool transparent_payload = op.opcode == OP_CHECK_LOWER ||
                                     op.opcode == OP_CHECK_UPPER ||
                                     op.opcode == OP_CHECK_MATCHING_DIMS;
    if (!k || !k->forward || k->make_state ||
        (op.udata && !transparent_payload) || op.dyn_extent_in != -1 ||
        op.dyn_lengths || op.dyn_capacity || op.opcode == OP_DYNAMIC_SLICE ||
        op.opcode == OP_INDEX_DYNAMIC || op.opcode == OP_SET_INDEX_DYNAMIC ||
        op.opcode == OP_SET_INDEX_INPLACE ||
        op.opcode == OP_SET_SLICE_INPLACE ||
        op.opcode == OP_SET_SLICE_STRIDED_INPLACE || !valid(op.out) ||
        op.n_in < 0 || op.n_in > 6 || (op.out2 != -1 && !valid(op.out2)))
      return reject(
          std::string("opaque/stateful kernel or dynamic import geometry: ") +
          opcode_name(op.opcode));
    written[op.out] = true;
    if (op.out2 >= 0) written[op.out2] = true;
    for (int i = 0; i < op.n_in; ++i)
      if (!valid(op.in[i])) return false;
  }
  for (const auto& op : graph.ops) {
    for (int i = 0; i < op.n_in; ++i) {
      const int s = op.in[i];
      int64_t begin = 0, end = graph.slots[s].len;
      if (op.n_in == 1 && op.out2 == -1 && op.variant == 0 &&
          (op.opcode == OP_INDEX || op.opcode == OP_SLICE)) {
        if (op.n_idata != 1 || !op.idata) return false;
        const int64_t len = graph.slots[op.out].len;
        begin = op.idata[0];
        if (begin < 0 || begin > end || len > end - begin ||
            (op.opcode == OP_INDEX && len != 1))
          return false;
        if (!len) continue;  // An empty read touches no source element.
        end = begin + len;
      }
      lo[s] = std::min(lo[s], begin);
      hi[s] = std::max(hi[s], end);
    }
  }
  for (size_t s = 0; s < count; ++s) {
    if (written[s] || int(s) == graph.result_slot) {
      lo[s] = 0;
      hi[s] = graph.slots[s].len;
    } else if (hi[s] == 0) {
      lo[s] = 0;
    }
  }
  for (const auto& fill : fills)
    if (!valid(fill.first) ||
        int64_t(fill.second.size()) != graph.slots[fill.first].len)
      return false;

  // Build everything off to the side, including owned replacement immediates.
  Graph candidate(graph);
  Fills next_fills;
  std::vector<Import> next_imports;
  int64_t parent = 0, local = 0;
  for (size_t s = 0; s < count; ++s) {
    const auto& old = graph.slots[s];
    const int64_t len = hi[s] - lo[s];
    candidate.slots[s].len = len;
    if (old.is_param) {
      if (len) next_imports.push_back({parent + lo[s], local, len});
      parent += old.len;
      local += len;
    }
  }
  for (auto& op : candidate.ops) {
    if (op.n_in != 1 || op.out2 != -1 || op.variant != 0 ||
        (op.opcode != OP_INDEX && op.opcode != OP_SLICE))
      continue;
    const int s = op.in[0];
    if (candidate.slots[s].len == graph.slots[s].len) continue;
    const int offset = candidate.slots[op.out].len ? op.idata[0] - lo[s] : 0;
    candidate.idata_pool.push_back({offset});
    op.idata = candidate.idata_pool.back().data();
  }
  for (const auto& fill : fills) {
    const int s = fill.first;
    if (hi[s] > lo[s])
      next_fills.push_back(
          {s, {fill.second.begin() + lo[s], fill.second.begin() + hi[s]}});
  }
  if (source_offsets) *source_offsets = std::move(lo);
  graph = std::move(candidate);
  fills = std::move(next_fills);
  imports = std::move(next_imports);
  return true;
}

// Split only the scatter plan, not the parameter import. Exclusive coordinates
// may be published by their sole worker; overlaps must retain chunk-order sums.
inline std::vector<std::vector<Import>> scatter_plan(
    const std::vector<std::vector<Import>>& imports, size_t parent_size) {
  std::vector<size_t> readers(parent_size, 0);
  for (const auto& child : imports)
    for (const auto& in : child)
      for (int64_t j = 0; j < in.len; ++j) ++readers.at(in.parent + j);
  std::vector<std::vector<Import>> out(imports.size());
  for (size_t k = 0; k < imports.size(); ++k)
    for (const auto& in : imports[k]) {
      int64_t begin = 0;
      while (begin < in.len) {
        const bool exclusive = readers[in.parent + begin] == 1;
        int64_t end = begin + 1;
        while (end < in.len && (readers[in.parent + end] == 1) == exclusive)
          ++end;
        out[k].push_back(
            {in.parent + begin, in.local + begin, end - begin, exclusive});
        begin = end;
      }
    }
  return out;
}
}  // namespace stanli::detail

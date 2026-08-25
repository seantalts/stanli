// Common-subexpression elimination.
//
// The unrolled M*_model family evaluates one Bernoulli term per capture
// occasion per individual, and for most of them the arguments are the same
// slot: Mh_model emits 685 bit-identical BERNOULLI_LPMF ops, Mb_model 786,
// and each one is a full kernel call plus a tape entry on every leapfrog
// step. Reroll leaves them alone (they are target terms with no op consumer),
// so nothing upstream removes the recomputation.
//
// This is textbook local value numbering, made safe for a graph whose slots
// are mutable buffers rather than SSA values:
//
//   - every write bumps a per-slot version, and the key carries the version
//     of each input, so two ops separated by a store to something they read
//     are different computations,
//   - only an op whose output slot is written exactly once in the whole graph
//     can be the survivor, which is what excludes the destructive update
//     chains: a slot a later SET_*_INPLACE mutates has two writers,
//   - effectful and stateful opcodes never merge, and neither does anything
//     carrying an opaque payload the key cannot compare.
//
// Renaming is lazy: ops are in evaluation order, so resolving each op's
// inputs through the map as the single forward pass reaches it is enough,
// and chains collapse in that same pass. Rescanning the tail after each
// merge would be quadratic in the op count.
#include <stanli/cse.hpp>
#include <stanli/optable.hpp>

#include <cstdlib>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace stanli {
namespace {

// Effects are graph semantics: a merged print prints once, a merged check
// throws once, a merged draw returns the same number twice. The set is the
// union of constfold.cpp's and reroll's ops_match, plus the two opcodes
// holding structure this pass cannot compare.
bool never_merge(uint16_t oc) {
  return oc == OP_CHECK_STRUCTURED || oc == OP_CHECK_MATCHING_DIMS ||
         oc == OP_CHECK_LOWER || oc == OP_CHECK_UPPER || oc == OP_CATEGORICAL ||
         oc == OP_RNG || oc == OP_PRINT || oc == OP_REJECT ||
         oc == OP_PROD_VEC || oc == OP_EXTREMA_VEC || oc == OP_ISLAND ||
         oc == OP_ODE;
}

struct Key {
  std::vector<int64_t> w;
  bool operator==(const Key& o) const { return w == o.w; }
};

struct KeyHash {
  size_t operator()(const Key& k) const {
    size_t h = 1469598103934665603ull;
    for (int64_t v : k.w) {
      h ^= static_cast<size_t>(v);
      h *= 1099511628211ull;
    }
    return h;
  }
};

}  // namespace

CseStats cse(Graph& g,
             const std::vector<std::pair<int, std::vector<double>>>& fills,
             std::vector<int>& target_terms,
             const std::vector<int>& extra_roots) {
  CseStats st;
  if (std::getenv("STANLI_NO_CSE")) return st;

  const size_t n_slots = g.slots.size();
  std::vector<int> writes(n_slots, 0);
  for (const Op& op : g.ops) {
    if (op.out >= 0) ++writes[(size_t)op.out];
    if (op.out2 >= 0) ++writes[(size_t)op.out2];
  }

  std::unordered_set<int> keep(extra_roots.begin(), extra_roots.end());
  if (g.result_slot >= 0) keep.insert(g.result_slot);
  // A fill writes its slot at bind time from a buffer sized by the slot
  // length, so a filled slot must keep that length even once dead.
  for (const auto& f : fills) keep.insert(f.first);

  std::vector<int64_t> version(n_slots, 0);
  std::unordered_map<Key, int, KeyHash> table;
  std::unordered_map<int, int> rename;  // removed output -> surviving output
  const auto resolve = [&](int s) {
    auto it = rename.find(s);
    return it == rename.end() ? s : it->second;
  };

  std::vector<char> drop(g.ops.size(), 0);
  Key key;
  for (size_t i = 0; i < g.ops.size(); ++i) {
    Op& op = g.ops[i];
    for (int j = 0; j < op.n_in; ++j) op.in[j] = resolve(op.in[j]);

    bool candidate = op.out >= 0 && op.out2 < 0 && op.udata == nullptr &&
                     !never_merge(op.opcode) && writes[(size_t)op.out] == 1 &&
                     !keep.count(op.out) && !g.slots[(size_t)op.out].is_param;
    for (int j = 0; candidate && j < op.n_in; ++j)
      candidate = op.in[j] != op.out;

    if (candidate) {
      key.w.clear();
      key.w.push_back(op.opcode);
      key.w.push_back(op.variant);
      key.w.push_back(g.slots[(size_t)op.out].len);
      key.w.push_back(op.n_in);
      for (int j = 0; j < op.n_in; ++j) {
        key.w.push_back(op.in[j]);
        key.w.push_back(op.in[j] >= 0 ? version[(size_t)op.in[j]] : 0);
      }
      key.w.push_back(op.n_idata);
      for (int64_t k = 0; k < op.n_idata; ++k) key.w.push_back(op.idata[k]);
      auto ins = table.emplace(key, op.out);
      if (!ins.second) {
        rename.emplace(op.out, ins.first->second);
        g.slots[(size_t)op.out].len = 0;
        drop[i] = 1;
        ++st.ops_removed;
        continue;
      }
    }
    if (op.out >= 0) ++version[(size_t)op.out];
    if (op.out2 >= 0) ++version[(size_t)op.out2];
  }

  if (st.ops_removed == 0) return st;
  for (int& t : target_terms) t = resolve(t);
  std::vector<Op> kept;
  kept.reserve(g.ops.size() - (size_t)st.ops_removed);
  for (size_t i = 0; i < g.ops.size(); ++i)
    if (!drop[i]) kept.push_back(g.ops[i]);
  g.ops = std::move(kept);
  return st;
}

}  // namespace stanli

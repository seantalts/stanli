// Destructive functional updates.
//
// `mu[n] = ...` inside a data-bound loop unrolls into a chain of
// OP_SET_INDEX ops. Each one copies its whole input vector into a fresh
// slot and then pokes one element, so N writes cost O(N^2) time and O(N^2)
// arena. Re-rolling can turn a run into one OP_SET_SLICE(_STRIDED), but a
// chain of those still makes one whole-base copy per slice. Measured on
// radon_county_intercept (N=12,573): 90.5 ms per gradient and 2.58 GB peak
// RSS, against CmdStan's 438 us; Mtbh_model's strided matrix fills are the
// corresponding slice shape.
//
// A write can mutate its input vector directly when nothing later observes
// that vector's pre-write contents. The condition is LAST USE, not single
// use: the read-back `INDEX(mu_n, n)` that immediately follows a write in
// the same lane is an earlier use of the same slot, so a single-use rule
// would refuse exactly the chains that matter. A write qualifies when
//
//   - its input is produced by an op. Fill-backed slots (declared vectors,
//     constants) are written once at bind time, so mutating one would let
//     state leak from one gradient evaluation into the next. The first
//     write of each chain therefore keeps its copy -- one O(N) copy per
//     evaluation, which is the price of the whole chain now.
//   - this op is the last op reading that slot,
//   - neither the input nor the output is a root (read straight from the
//     arena, invisible in the use map) or a parameter, and
//   - the op that produced this version does not reread its output values in
//     reverse (exp does, while a preceding copying store does not), and
//   - no EARLIER reader of that vector needs its values during the reverse
//     sweep. Forward order makes the values those readers see correct, but
//     a backward pass runs after every write has landed: `legacy_bwd_vec_in`
//     (log_sum_exp, softmax, ...) rebuilds its var tape from
//     `ctx.in[0].data` at reverse time, so a destroyed buffer feeds it the
//     wrong numbers. Only ops whose backward moves adjoints and never reads
//     input or output values may precede a destructive write. Found by the
//     corpus A/B: without this, 8 HMM/LDA/mixture models were wrong by up to
//     1.7e+05 relative while every op count looked untouched.
//
// The rewrite makes the op's output slot BE its input slot, so `bind_()`
// gives them one offset with no executor change, and later references to
// the dead output slot are renamed to the input.
#include <stanli/inplace.hpp>
#include <stanli/optable.hpp>

#include <algorithm>
#include <cstdlib>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace stanli {

namespace {

uint16_t inplace_form(uint16_t opcode) {
  switch (opcode) {
    case OP_SET_INDEX:
      return OP_SET_INDEX_INPLACE;
    case OP_SET_SLICE:
      return OP_SET_SLICE_INPLACE;
    case OP_SET_SLICE_STRIDED:
      return OP_SET_SLICE_STRIDED_INPLACE;
    default:
      return 0;
  }
}

bool is_inplace_store(uint16_t opcode) {
  return opcode == OP_SET_INDEX_INPLACE || opcode == OP_SET_SLICE_INPLACE ||
         opcode == OP_SET_SLICE_STRIDED_INPLACE;
}

// In-place kernels deliberately omit the base copy, so accept only the exact
// contracts the copying kernels implement. Besides making the rewrite fail
// closed on hand-built graphs, the division-form strided bound avoids an
// overflow in start + (len - 1) * stride.
bool valid_store_contract(const Graph& g, const Op& op) {
  if (op.n_in != 2 || op.out2 >= 0 || op.variant != 0 || op.udata != nullptr)
    return false;
  if (op.in[0] < 0 || op.in[1] < 0 || op.out < 0 ||
      op.in[0] >= static_cast<int>(g.slots.size()) ||
      op.in[1] >= static_cast<int>(g.slots.size()) ||
      op.out >= static_cast<int>(g.slots.size()))
    return false;
  const bool inplace = is_inplace_store(op.opcode);
  if (inplace) {
    if (op.out != op.in[0] || op.in[1] == op.in[0] || g.slots[op.out].is_param)
      return false;
  } else if (op.out == op.in[0] || op.out == op.in[1] ||
             g.slots[op.out].is_param ||
             g.slots[op.out].len != g.slots[op.in[0]].len) {
    return false;
  }

  const int64_t base_len = g.slots[op.in[0]].len;
  const int64_t rhs_len = g.slots[op.in[1]].len;
  if (base_len < 0 || rhs_len < 0 || op.idata == nullptr) return false;
  if (op.opcode == OP_SET_INDEX || op.opcode == OP_SET_INDEX_INPLACE) {
    return op.n_idata == 1 && rhs_len == 1 && op.idata[0] >= 0 &&
           static_cast<int64_t>(op.idata[0]) < base_len;
  }
  if (op.opcode == OP_SET_SLICE || op.opcode == OP_SET_SLICE_INPLACE) {
    if (op.n_idata != 1 || op.idata[0] < 0) return false;
    const int64_t start = op.idata[0];
    return start <= base_len && rhs_len <= base_len - start;
  }
  if (op.opcode == OP_SET_SLICE_STRIDED ||
      op.opcode == OP_SET_SLICE_STRIDED_INPLACE) {
    if (op.n_idata != 2 || op.idata[0] < 0 || op.idata[1] <= 0) return false;
    const int64_t start = op.idata[0], stride = op.idata[1];
    if (rhs_len == 0) return start <= base_len;
    return start < base_len && rhs_len - 1 <= (base_len - 1 - start) / stride;
  }
  return false;
}

}  // namespace

bool backward_ignores_values(uint16_t opcode) {
  return has_op_trait(opcode, op_trait::kBackwardValueFree);
}

bool backward_ignores_input_values(uint16_t opcode) {
  return backward_ignores_values(opcode);
}

int make_inplace_updates(Graph& g, const std::vector<int>& roots) {
  if (std::getenv("STANLI_NO_INPLACE")) return 0;

  const int n_slots = static_cast<int>(g.slots.size());
  // Last op index that reads each slot.
  std::vector<int> last_use(n_slots, -1);
  for (size_t i = 0; i < g.ops.size(); ++i) {
    for (int j = 0; j < g.ops[i].n_in; ++j)
      last_use[g.ops[i].in[j]] = static_cast<int>(i);
  }
  std::unordered_set<int> root_set(roots.begin(), roots.end());
  if (g.result_slot >= 0) root_set.insert(g.result_slot);

  std::unordered_map<int, int> rename;  // dead output slot -> live slot
  const auto resolve = [&](int s) {
    auto it = rename.find(s);
    return it == rename.end() ? s : it->second;
  };

  // Set once some op that will re-read a slot's values during the reverse
  // sweep has consumed it; from then on that slot may not be destroyed.
  std::vector<char> value_reader(n_slots, 0);
  // A later or pre-existing in-place writer must not make a fill-backed slot
  // look safe on this second, post-reroll pass. Requiring the fresh producer's
  // backward to ignore its output also protects producers such as exp, whose
  // derivative rereads the value a destructive store would replace.
  std::vector<char> output_safe_producer(n_slots, 0);

  int n_rewritten = 0;
  for (size_t i = 0; i < g.ops.size(); ++i) {
    Op& op = g.ops[i];
    // A rewrite earlier in this same scan may have renamed the slot shared by
    // an already-destructive store. Preserve its defining out==in[0]
    // invariant: resolving only the input would leave out naming the dead,
    // zero-length slot. Verify the original alias before canonicalizing so a
    // malformed hand-built op cannot acquire legitimacy from the pass.
    const bool existing_inplace = is_inplace_store(op.opcode);
    const int original_in0 = op.n_in > 0 ? op.in[0] : -1;
    const bool original_inplace_alias =
        existing_inplace && op.out >= 0 && op.out == original_in0;
    for (int j = 0; j < op.n_in; ++j) op.in[j] = resolve(op.in[j]);
    if (original_inplace_alias) op.out = op.in[0];
    const bool valid_existing_inplace =
        !existing_inplace ||
        (original_inplace_alias && valid_store_contract(g, op));
    const auto mark_fresh_output = [&](int out) {
      if (out < 0) return;
      for (int j = 0; j < op.n_in; ++j) {
        if (op.in[j] != out) continue;
        // A known destructive store preserves the safety (or unsafety) of
        // the version it mutates. Any other aliased producer replaces the
        // version with one its own backward may reread, so fail closed.
        const bool known_destructive_store =
            existing_inplace && valid_existing_inplace;
        if (!known_destructive_store) output_safe_producer[out] = 0;
        return;
      }
      output_safe_producer[out] =
          valid_existing_inplace && backward_ignores_values(op.opcode);
    };
    const bool routes_only =
        backward_ignores_values(op.opcode) && valid_existing_inplace;

    const uint16_t inplace_opcode = inplace_form(op.opcode);
    if (inplace_opcode == 0) {
      if (!routes_only)
        for (int j = 0; j < op.n_in; ++j) value_reader[op.in[j]] = 1;
      mark_fresh_output(op.out);
      if (op.out2 >= 0) output_safe_producer[op.out2] = 0;
      continue;
    }

    const int vec = op.in[0];
    // The in-place forward overwrites the base before it could finish reading
    // an aliased RHS; reverse would also route into and clear one buffer.
    const bool eligible = valid_store_contract(g, op) && op.in[1] != vec &&
                          output_safe_producer[vec] && !g.slots[vec].is_param &&
                          !root_set.count(vec) && !root_set.count(op.out) &&
                          last_use[vec] == static_cast<int>(i) &&
                          !value_reader[vec];
    if (!eligible) {
      mark_fresh_output(op.out);
      if (op.out2 >= 0) output_safe_producer[op.out2] = 0;
      continue;
    }
    // The renamed slot inherits the original's last use: a read of any
    // link in the chain is a read of the one buffer they now share.
    rename[op.out] = vec;
    // A later read of the old output is a read of the shared buffer, so
    // the surviving slot inherits it: the next write in the chain must not
    // treat itself as the last use while that read is still pending.
    last_use[vec] = std::max(last_use[vec], last_use[op.out]);
    // The old output slot is now unreachable: no op writes or reads it,
    // and it was neither a root nor a parameter (checked above), so it
    // needs no arena. Without this the chain still costs O(N^2) memory --
    // bind_() sizes the arenas from slot lengths, not from op references.
    g.slots[op.out].len = 0;
    op.opcode = inplace_opcode;
    op.out = vec;  // out and in[0] are now one slot: one buffer, one adjoint
    ++n_rewritten;
  }
  return n_rewritten;
}

int forward_stores_to_loads(Graph& g, const std::vector<int>& roots) {
  if (std::getenv("STANLI_NO_INPLACE")) return 0;

  std::unordered_set<int> root_set(roots.begin(), roots.end());
  if (g.result_slot >= 0) root_set.insert(g.result_slot);

  // Most recent write to each vector, as (op index, element, value slot).
  struct Store {
    size_t op;
    int elem;
    int value;
  };
  std::unordered_map<int, Store> last_store;
  std::unordered_map<int, int> rename;  // load output -> stored value slot
  const auto resolve = [&](int s) {
    auto it = rename.find(s);
    return it == rename.end() ? s : it->second;
  };

  std::vector<char> drop(g.ops.size(), 0);
  int removed = 0;
  for (size_t i = 0; i < g.ops.size(); ++i) {
    Op& op = g.ops[i];
    for (int j = 0; j < op.n_in; ++j) op.in[j] = resolve(op.in[j]);

    if (op.opcode == OP_SET_INDEX || op.opcode == OP_SET_INDEX_INPLACE) {
      // The written vector is op.out (both forms), and the element written
      // is idata[0]. A later write to the same vector supersedes this one.
      last_store[op.out] = Store{i, op.idata[0], op.in[1]};
      continue;
    }
    if (op.opcode == OP_SET_SLICE || op.opcode == OP_SET_SLICE_INPLACE ||
        op.opcode == OP_SET_SLICE_STRIDED ||
        op.opcode == OP_SET_SLICE_STRIDED_INPLACE) {
      // A destructive slice may cover the cached element. Copying forms
      // have a distinct output and therefore erase nothing in ordinary SSA
      // graphs, but spelling all four contracts keeps this correct for a
      // hand-built graph too.
      last_store.erase(op.out);
      continue;
    }
    if (op.opcode == OP_INDEX && op.n_idata == 1) {
      auto it = last_store.find(op.in[0]);
      if (it != last_store.end() && it->second.elem == op.idata[0] &&
          !root_set.count(op.out)) {
        // Reading back exactly what the last write put there.
        rename[op.out] = it->second.value;
        drop[i] = 1;
        ++removed;
        continue;
      }
    }
    // Any other read of a vector leaves its stores observable; a write to
    // some *other* vector cannot invalidate this one, so nothing else
    // needs clearing here -- last_store is keyed by vector.
  }

  // Sweep dead writes: a store whose vector nothing reads afterwards (and
  // which is not a root) computes nothing. Walk backwards so a chain dies
  // from its tail.
  std::vector<char> read_after(g.slots.size(), 0);
  for (size_t k = g.ops.size(); k-- > 0;) {
    if (drop[k]) continue;
    Op& op = g.ops[k];
    const bool is_store =
        op.opcode == OP_SET_INDEX || op.opcode == OP_SET_INDEX_INPLACE;
    if (is_store && !read_after[op.out] && !root_set.count(op.out)) {
      drop[k] = 1;
      ++removed;
      continue;
    }
    for (int j = 0; j < op.n_in; ++j) read_after[op.in[j]] = 1;
  }

  if (removed) {
    std::vector<Op> kept;
    kept.reserve(g.ops.size() - (size_t)removed);
    for (size_t i = 0; i < g.ops.size(); ++i)
      if (!drop[i]) kept.push_back(g.ops[i]);
    g.ops = std::move(kept);
  }
  return removed;
}

int elide_full_extent_stores(Graph& g, const std::vector<int>& roots) {
  if (std::getenv("STANLI_NO_INPLACE")) return 0;

  std::unordered_set<int> root_set(roots.begin(), roots.end());
  if (g.result_slot >= 0) root_set.insert(g.result_slot);

  const size_t n_ops = g.ops.size();
  std::vector<std::vector<size_t>> readers(g.slots.size());
  std::vector<std::vector<size_t>> writers(g.slots.size());
  for (size_t i = 0; i < n_ops; ++i) {
    const Op& op = g.ops[i];
    for (int j = 0; j < op.n_in; ++j)
      if (op.in[j] >= 0) readers[(size_t)op.in[j]].push_back(i);
    if (op.out >= 0) writers[(size_t)op.out].push_back(i);
    if (op.out2 >= 0) writers[(size_t)op.out2].push_back(i);
  }

  const auto covers_destination = [&g](const Op& op) {
    const bool strided = op.opcode == OP_SET_SLICE_STRIDED ||
                         op.opcode == OP_SET_SLICE_STRIDED_INPLACE;
    if (op.opcode != OP_SET_SLICE && op.opcode != OP_SET_SLICE_INPLACE &&
        !strided)
      return false;
    return op.n_in == 2 && op.out >= 0 && op.in[1] >= 0 && op.n_idata >= 1 &&
           op.idata[0] == 0 && (!strided || op.idata[1] == 1) &&
           g.slots[(size_t)op.in[1]].len == g.slots[(size_t)op.out].len;
  };

  std::vector<char> drop(n_ops, 0);
  int removed = 0;
  for (size_t i = 0; i < n_ops; ++i) {
    const Op& op = g.ops[i];
    if (!covers_destination(op)) continue;
    const int dest = op.out, val = op.in[1];
    if (dest == val) continue;
    if (root_set.count(dest) || root_set.count(val)) continue;
    if (g.slots[(size_t)dest].is_param || g.slots[(size_t)val].is_param)
      continue;
    // The destination's adjoints reach the value in one move today and one
    // per reader afterwards. Those sums agree to the bit only while the
    // value's own adjoint starts the region at zero, which a second reader
    // would break.
    const std::vector<size_t>& val_readers = readers[(size_t)val];
    if (val_readers.size() != 1 || val_readers[0] != i) continue;
    const std::vector<size_t>& val_writers = writers[(size_t)val];
    if (!val_writers.empty() && val_writers.back() >= i) continue;

    const std::vector<size_t>& dest_writers = writers[(size_t)dest];
    size_t next_write = n_ops;
    for (size_t w : dest_writers)
      if (w > i) {
        next_write = w;
        break;
      }
    // Whoever writes the destination next must neither read what this store
    // put there nor leave its adjoints for the reverse sweep to carry past
    // this point. A covering destructive store does neither: it only writes
    // forward, and it clears the range it owns in reverse.
    if (next_write < n_ops &&
        !((g.ops[next_write].opcode == OP_SET_SLICE_INPLACE ||
           g.ops[next_write].opcode == OP_SET_SLICE_STRIDED_INPLACE) &&
          g.ops[next_write].out == dest &&
          covers_destination(g.ops[next_write])))
      continue;

    std::vector<size_t> window;
    bool ok = true;
    for (size_t r : readers[(size_t)dest]) {
      if (r <= i || r >= next_write) continue;
      // Island bodies name outer slots in a payload this rename cannot
      // reach.
      if (g.ops[r].opcode == OP_ISLAND) {
        ok = false;
        break;
      }
      window.push_back(r);
    }
    if (!ok) continue;

    for (size_t r : window) {
      Op& reader = g.ops[r];
      for (int j = 0; j < reader.n_in; ++j)
        if (reader.in[j] == dest) reader.in[j] = val;
    }
    drop[i] = 1;
    ++removed;
  }

  if (removed) {
    std::vector<Op> kept;
    kept.reserve(n_ops - (size_t)removed);
    for (size_t i = 0; i < n_ops; ++i)
      if (!drop[i]) kept.push_back(g.ops[i]);
    g.ops = std::move(kept);
  }
  return removed;
}

}  // namespace stanli

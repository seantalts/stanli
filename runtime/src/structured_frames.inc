// Included inside structured_loop.cpp's anonymous namespace. This path uses
// the same recording semantics and kernels, but never retains a whole-loop
// instruction stream or version table.
//
// Instruction headers pack StreamInstr::Kind in the low byte and the total
// word count above it. Remaining words are scalar operands, followed by any
// variable-length bindings. A nonnegative value reference is a local offset;
// -1, -2, ... index external bindings, and frame_null denotes no storage.
// Adjoint references use the same encoding relative to adjoint_base. Saved
// literals and external addresses belong to the frame, never the shared code.
constexpr int64_t frame_null = std::numeric_limits<int64_t>::min();

struct FrameAdjointHash {
  size_t operator()(const FrameAdjoint& a) const {
    size_t h = std::hash<int64_t>{}(a.id);
    h ^= std::hash<int64_t>{}(a.shift) + (h << 6) + (h >> 2);
    h ^= std::hash<int64_t>{}(a.promotion) + (h << 6) + (h >> 2);
    return h;
  }
};

FrameAdjoint frame_adjoint(const LoopState& s, int64_t version,
                           int64_t shift = 0) {
  if (version < 0) return {};
  const auto it = s.frames->promotion_links.find(version);
  if (it != s.frames->promotion_links.end()) return {-1, shift, it->second};
  return {s.versions[static_cast<size_t>(version)].adjoint, shift, -1};
}

double* frame_adjoint_ptr(const FrameAdjoint& a, FrameTape& tape,
                          const StructuredLoop& p, KernelCtx& ctx) {
  const int64_t id = a.promotion >= 0 ? tape.promotions[a.promotion] : a.id;
  double* ptr = resolve_adjoint(id, tape.adjoints.data(), p, ctx);
  return ptr ? ptr + a.shift : nullptr;
}

// Matching consumes the same recorded operands as canonical encoding. Only
// fresh bindings/literals are written; a mismatch leaves the Stream intact.
template <bool Matching = false>
struct FrameEncoder {
  LoopState& s;
  LoopFrame& frame;
  const ArenaSnapshot& snapshot;
  FrameCode code;
  std::unordered_map<const double*, int64_t> external;
  std::unordered_map<FrameAdjoint, int64_t, FrameAdjointHash> external_adj;
  ArrayIntern<int64_t> positions;
  const FrameCode* expected = nullptr;
  size_t cursor = 0;
  size_t backward_cursor = 0;
  bool matched = true;

  FrameEncoder(LoopState& state, LoopFrame& target,
               const ArenaSnapshot& arena, const FrameCode* layout = nullptr)
      : s(state), frame(target), snapshot(arena), expected(layout) {}

  uint64_t peek() {
    if (cursor < expected->words.size()) return expected->words[cursor];
    matched = false;
    return 0;
  }
  template <typename T>
  int64_t bind(std::vector<T>& bindings, const T& value) {
    const int64_t ref = static_cast<int64_t>(peek());
    if (ref >= 0 || ref == frame_null) {
      matched = false;
      return frame_null;
    }
    const uint64_t index = static_cast<uint64_t>(-(ref + 1));
    if (index == bindings.size())
      bindings.push_back(value);
    else if (index > bindings.size() || !(bindings[index] == value))
      matched = false;
    // Distinct external indices may now alias. Each operand still resolves
    // to its current address/identity, so instruction order preserves aliasing.
    return ref;
  }

  int64_t local(const double* ptr) const {
    auto it =
        std::upper_bound(snapshot.ranges.begin(), snapshot.ranges.end(), ptr,
                         [](const double* q, const ArenaSnapshot::Range& r) {
                           return q < r.base;
                         });
    if (it != snapshot.ranges.begin()) {
      --it;
      if (ptr >= it->base && ptr < it->base + it->used)
        return static_cast<int64_t>(it->offset + (ptr - it->base));
    }
    return frame_null;
  }
  int64_t value(const double* ptr, int64_t len = 1) {
    if (!ptr || len == 0) return frame_null;
    const auto offset = local(ptr);
    if (offset != frame_null) return offset;
    if constexpr (Matching)
      return bind(frame.bindings, const_cast<double*>(ptr));
    auto [entry, inserted] = external.emplace(ptr, frame.bindings.size());
    if (inserted) frame.bindings.push_back(const_cast<double*>(ptr));
    return -entry->second - 1;
  }
  int64_t adjoint(int64_t version, int64_t shift = 0) {
    const auto a = frame_adjoint(s, version, shift);
    if (a.promotion < 0) {
      if (a.id == -1) return frame_null;
      if (a.id >= frame.adjoint_base) return a.id - frame.adjoint_base + shift;
    }
    if constexpr (Matching) return bind(frame.adj_bindings, a);
    auto [entry, inserted] = external_adj.emplace(a, frame.adj_bindings.size());
    if (inserted) frame.adj_bindings.push_back(a);
    return -entry->second - 1;
  }
  size_t literal(double v) {
    frame.literals.push_back(v);
    return frame.literals.size() - 1;
  }
  void word(int64_t v) {
    if constexpr (Matching) {
      matched &= peek() == static_cast<uint64_t>(v);
      ++cursor;
    } else {
      code.words.push_back(static_cast<uint64_t>(v));
    }
  }

  template <typename Int>
  int64_t index_positions(const std::vector<Int>& pool, size_t offset,
                          size_t count, int64_t shift) {
    if constexpr (Matching) {
      const uint64_t at = peek();
      if (at > expected->positions.size() ||
          count > expected->positions.size() - at) {
        matched = false;
        return 0;
      }
      for (size_t k = 0; k < count; ++k)
        matched &= pool[offset + k] - shift == expected->positions[at + k];
      return static_cast<int64_t>(at);
    } else {
      std::vector<int64_t> pos(count);
      for (size_t k = 0; k < count; ++k) pos[k] = pool[offset + k] - shift;
      return positions.intern(pos.data(), pos.size());
    }
  }

  bool encode(const Stream& st) {
    for (const auto& instruction : st.program) {
      if (instruction.kind == StreamInstr::Tgt) continue;
      const size_t start = Matching ? cursor : code.words.size();
      if (start > std::numeric_limits<uint32_t>::max())
        throw std::length_error("structured frame program overflow");
      uint64_t header = 0;
      if constexpr (Matching) {
        header = peek();
        if (!matched || (header & 255) != instruction.kind) return false;
        ++cursor;
      } else {
        word(instruction.kind);
      }
      bool backward = false;
      switch (instruction.kind) {
        case StreamInstr::AdjointCall:
        case StreamInstr::Call: {
          const auto& f = st.calls[instruction.index];
          const auto& n = *s.sites[f.site];
          const auto& op = s.p.body.ops[n.op];
          word(f.site);
          word(f.flags);
          word(f.n_in);
          size_t at = f.ptr_offset;
          const auto len_of = [&](int slot, int bit) {
            return (op.dyn_lengths & (1u << bit)) ? op.dyn_capacity
                                                  : s.p.body.slots[slot].len;
          };
          for (int k = 0; k < f.n_in; ++k)
            word(value(st.call_ptrs[at++], len_of(op.in[k], k)));
          word(value(st.call_ptrs[at++], len_of(op.out, 6)));
          if (f.flags & kFrozenCallHasOut2)
            word(value(st.call_ptrs[at++], s.p.body.slots[op.out2].len));
          word(value(st.call_ptrs[at], n.kernel_scratch));
          backward = (f.flags & kFrozenCallActive) != 0;
          if (backward) {
            const size_t count =
                f.n_in + 1 + ((f.flags & kFrozenCallHasOut2) != 0);
            for (size_t k = 0; k < count; ++k)
              word(adjoint(st.call_adj_version[f.adj_offset + k]));
          }
          break;
        }
        case StreamInstr::InPlace: {
          const auto& f = st.inplaces[instruction.index];
          const int64_t shift = f.pos_count ? st.inplace_pos[f.pos_offset] : 0;
          word(f.pos_count);
          word(index_positions(st.inplace_pos, f.pos_offset, f.pos_count, shift));
          word(f.old_offset);
          word(value(f.base + shift, st.inplace_base_len[instruction.index]));
          word(value(f.rhs, f.pos_count));
          word(adjoint(st.inplace_adj_version[2 * instruction.index], shift));
          word(adjoint(st.inplace_adj_version[2 * instruction.index + 1]));
          word(f.sel_count);
          for (size_t k = 0; k < f.sel_count; ++k) {
            word(value(st.inplace_sel_ptr[f.sel_offset + k]));
            word(literal(st.inplace_sel_snapshot[f.sel_offset + k]));
          }
          backward = true;
          break;
        }
        case StreamInstr::Copy: {
          const auto& f = st.copies[instruction.index];
          word(f.len);
          word(value(f.src, f.len));
          word(value(f.dst, f.len));
          word(adjoint(st.copy_adj_version[2 * instruction.index]));
          word(adjoint(st.copy_adj_version[2 * instruction.index + 1]));
          backward = true;
          break;
        }
        case StreamInstr::Gather: {
          const auto& f = st.gathers[instruction.index];
          const int64_t shift = f.pos_count ? st.gather_pos[f.pos_offset] : 0;
          word(f.pos_count);
          word(index_positions(st.gather_pos, f.pos_offset, f.pos_count, shift));
          word(value(f.src + shift, st.gather_src_len[instruction.index]));
          word(value(f.dst, f.pos_count));
          backward = (f.flags & kFrozenGatherActive) != 0;
          word(backward ? adjoint(st.gather_adj_version[f.adj_offset], shift)
                        : frame_null);
          word(backward ? adjoint(st.gather_adj_version[f.adj_offset + 1])
                        : frame_null);
          break;
        }
        case StreamInstr::GuardIf: {
          const auto& f = st.guards_if[instruction.index];
          word(value(f.a));
          word(f.decision);
          break;
        }
        case StreamInstr::GuardFor: {
          const auto& f = st.guards_for[instruction.index];
          word(value(f.a));
          word(value(f.b));
          word(literal(f.va));
          word(literal(f.vb));
          break;
        }
        case StreamInstr::Set: {
          const auto& f = st.sets[instruction.index];
          word(value(f.ptr));
          word(literal(f.value));
          break;
        }
        case StreamInstr::Seg:
          throw std::logic_error(
              "segments are not eligible for frame recording");
        case StreamInstr::Tgt:
          break;
      }
      if constexpr (Matching) {
        if (!matched || (header >> 8) != cursor - start) return false;
        if (backward) {
          if (backward_cursor == expected->backward.size() ||
              expected->backward[backward_cursor++] != start)
            return false;
        } else if (backward_cursor < expected->backward.size() &&
                   expected->backward[backward_cursor] == start) {
          return false;
        }
      } else {
        code.words[start] |= static_cast<uint64_t>(code.words.size() - start)
                             << 8;
        if (backward) code.backward.push_back(static_cast<uint32_t>(start));
      }
    }
    if constexpr (Matching) {
      return matched && cursor == expected->words.size() &&
             backward_cursor == expected->backward.size();
    } else {
      code.positions = std::move(positions.data);
      return true;
    }
  }
};

// Optional differential proof check. Canonical encoding may use different
// external indices or interned position offsets, so compare resolved operands
// and backward order, rather than requiring identical representation bytes.
void check_frame_layout(LoopState& s, const Stream& st,
                         const ArenaSnapshot& snapshot, const LoopFrame& frame) {
  LoopFrame oracle;
  oracle.adjoint_base = frame.adjoint_base;
  FrameEncoder<false> encoder{s, oracle, snapshot};
  encoder.encode(st);
  const auto& a = *frame.code;
  const auto& b = encoder.code;
  const auto require = [](bool ok) {
    if (!ok) throw std::logic_error("structured frame layout proof mismatch");
  };
  require(a.words.size() == b.words.size() && a.backward == b.backward);
  const auto value = [&](const LoopFrame& f, uint64_t raw) -> const double* {
    const int64_t ref = static_cast<int64_t>(raw);
    if (ref == frame_null) return nullptr;
    if (ref >= 0) {
      require(static_cast<size_t>(ref) < frame.values.size());
      return frame.values.data() + ref;
    }
    require(static_cast<uint64_t>(-(ref + 1)) < f.bindings.size());
    return f.bindings[-ref - 1];
  };
  const auto adjoint = [&](const LoopFrame& f, uint64_t raw) {
    const int64_t ref = static_cast<int64_t>(raw);
    if (ref == frame_null) return FrameAdjoint{};
    if (ref >= 0) return FrameAdjoint{f.adjoint_base + ref, 0, -1};
    require(static_cast<uint64_t>(-(ref + 1)) < f.adj_bindings.size());
    auto result = f.adj_bindings[-ref - 1];
    if (result.promotion < 0 && result.id >= 0) {
      result.id += result.shift;
      result.shift = 0;
    }
    return result;
  };
  const auto v = [&](uint64_t x, uint64_t y) {
    require(value(frame, x) == value(oracle, y));
  };
  const auto d = [&](uint64_t x, uint64_t y) {
    require(adjoint(frame, x) == adjoint(oracle, y));
  };
  const auto literal = [&](uint64_t x, uint64_t y) {
    require(x < frame.literals.size() && y < oracle.literals.size());
    require(std::memcmp(&frame.literals[x], &oracle.literals[y],
                        sizeof(double)) == 0);
  };
  const auto positions = [&](uint64_t x, uint64_t y, uint64_t count) {
    require(x <= a.positions.size() && count <= a.positions.size() - x &&
            y <= b.positions.size() && count <= b.positions.size() - y);
    for (size_t k = 0; k < count; ++k)
      require(a.positions[x + k] == b.positions[y + k]);
  };
  for (size_t pc = 0; pc < a.words.size();) {
    require(a.words[pc] == b.words[pc]);
    const auto header = a.words[pc];
    const auto* x = a.words.data() + pc + 1;
    const auto* y = b.words.data() + pc + 1;
    pc += header >> 8;
    switch (header & 255) {
      case StreamInstr::AdjointCall:
      case StreamInstr::Call: {
        require(x[0] == y[0] && x[1] == y[1] && x[2] == y[2]);
        size_t at = 3;
        const size_t operands = x[2] + 1 + ((x[1] & kFrozenCallHasOut2) != 0);
        for (size_t k = 0; k < operands + 1; ++k, ++at) v(x[at], y[at]);
        if (x[1] & kFrozenCallActive)
          for (size_t k = 0; k < operands; ++k, ++at) d(x[at], y[at]);
        break;
      }
      case StreamInstr::InPlace:
        require(x[0] == y[0] && x[2] == y[2] && x[7] == y[7]);
        positions(x[1], y[1], x[0]);
        v(x[3], y[3]); v(x[4], y[4]);
        d(x[5], y[5]); d(x[6], y[6]);
        for (size_t k = 0; k < x[7]; ++k) {
          v(x[8 + 2 * k], y[8 + 2 * k]);
          literal(x[9 + 2 * k], y[9 + 2 * k]);
        }
        break;
      case StreamInstr::Copy:
        require(x[0] == y[0]);
        v(x[1], y[1]); v(x[2], y[2]);
        d(x[3], y[3]); d(x[4], y[4]);
        break;
      case StreamInstr::Gather:
        require(x[0] == y[0]);
        positions(x[1], y[1], x[0]);
        v(x[2], y[2]); v(x[3], y[3]);
        d(x[4], y[4]); d(x[5], y[5]);
        break;
      case StreamInstr::GuardIf:
        v(x[0], y[0]); require(x[1] == y[1]); break;
      case StreamInstr::GuardFor:
        v(x[0], y[0]); v(x[1], y[1]);
        literal(x[2], y[2]); literal(x[3], y[3]); break;
      case StreamInstr::Set:
        v(x[0], y[0]); literal(x[1], y[1]); break;
      default:
        require(false);
    }
  }
}

void seal_frame(LoopState& s) {
  FrameTape& tape = *s.frames;
  Stream& st = *s.building;
  tape.version_peak = std::max(tape.version_peak, s.versions.size());
  tape.recording_cells = std::max(tape.recording_cells, s.arena.used());

  // Retain only versions still reachable through a binding or a memo cell.
  // Historical derivative identities are offsets, not these working handles.
  if (!tape.sealing) tape.sealing = std::make_unique<FrameSealScratch>();
  auto& scratch = *tape.sealing;
  auto& live_ids = scratch.live_ids;
  auto& remap = scratch.remap;
  auto& lengths = scratch.lengths;
  live_ids.clear();
  remap.assign(s.versions.size(), -1);
  lengths.assign(s.versions.size(), 0);
  const auto live = [&](int64_t id, int64_t len) {
    if (id < 0) return;
    lengths[id] = std::max(lengths[id], len);
    if (remap[id] < 0) {
      remap[id] = static_cast<int64_t>(live_ids.size());
      live_ids.push_back(id);
    }
  };
  for (size_t slot = 0; slot < s.bindings.size(); ++slot)
    live(s.bindings[slot], s.p.body.slots[slot].len);
  for (size_t site = 0; site < s.sites.size(); ++site) {
    const auto& node = *s.sites[site];
    // After a guard fails, an untaken arm can still have a memo handle from
    // the discarded recording. Its generation is invalid in this recording;
    // do not treat that old handle as a reachable current version.
    if (node.storage != Node::Transient && s.node_generation[site] < 0) {
      s.node_version[site] = s.node_version2[site] = -1;
    }
    const auto& op = s.p.body.ops[node.op];
    live(s.node_version[site], s.p.body.slots[op.out].len);
    if (op.out2 >= 0) live(s.node_version2[site], s.p.body.slots[op.out2].len);
    for (int k = 0; k < op.n_in; ++k)
      if (op.dyn_lengths & (1u << k))
        live(s.bindings[op.in[k]], op.dyn_capacity);
    if (op.dyn_lengths & (1u << 6)) {
      live(s.bindings[op.out], op.dyn_capacity);
      live(s.node_version[site], op.dyn_capacity);
    }
  }
  for (int64_t id : s.loop_version) live(id, 1);
  for (int64_t id : live_ids) {
    if (s.versions[id].owner >= 0 && s.versions[id].adjoint == -1 &&
        !s.frames->promotion_links.count(id)) {
      tape.promotion_links.emplace(id, tape.promotions.size());
      tape.promotions.push_back(-1);
    }
  }

  auto& ranges = scratch.ranges;
  ranges.clear();
  collect_live_ranges(s, st, ranges);
  for (int64_t id : live_ids)
    if (lengths[id] && s.versions[id].value)
      ranges.emplace_back(s.versions[id].value, lengths[id]);
  for (size_t k = 0; k < tape.outputs.size(); ++k)
    if (s.p.body.slots[s.p.outputs[k]].len)
      ranges.emplace_back(tape.outputs[k], s.p.body.slots[s.p.outputs[k]].len);
  const auto allocated = capture_arena_ranges(s.arena);
  ranges.erase(std::remove_if(
                   ranges.begin(), ranges.end(),
                   [&](const auto& r) { return !allocated.contains(r.first); }),
               ranges.end());
  std::sort(ranges.begin(), ranges.end());
  auto& snapshot = scratch.snapshot;
  snapshot.ranges.clear();
  size_t total = 0;
  for (size_t i = 0; i < ranges.size();) {
    const double* base = ranges[i].first;
    const double* end = base + ranges[i].second;
    size_t j = i + 1;
    while (j < ranges.size() && ranges[j].first <= end) {
      end = std::max(end, ranges[j].first + ranges[j].second);
      ++j;
    }
    const size_t len = end - base;
    snapshot.ranges.push_back({base, len, total});
    total += len;
    i = j;
  }
  auto frame = std::make_unique<LoopFrame>();
  frame->values.resize(total);
  for (const auto& r : snapshot.ranges)
    std::copy_n(r.base, r.used, frame->values.data() + r.offset);
  frame->adjoint_base = tape.adjoint_base;
  frame->old = std::move(st.inplace_old);
  FrameEncoder<false> encoder{s, *frame, snapshot};
  if (s.frame_layout && !scratch.layouts.empty()) {
    ++tape.layout_attempts;
    for (const auto& layout : scratch.layouts) {
      if (layout.instructions != st.program.size()) continue;
      FrameEncoder<true> matcher{s, *frame, snapshot, layout.code.get()};
      if (matcher.encode(st)) {
        frame->code = layout.code;
        ++tape.layout_matches;
        if (s.check_frame_layout) check_frame_layout(s, st, snapshot, *frame);
        break;
      }
      frame->bindings.clear();
      frame->adj_bindings.clear();
      frame->literals.clear();
    }
  }
  if (!frame->code) {
    encoder.encode(st);
    size_t hash = 0;
    const auto hash_words = [&](const auto& words) {
      for (auto word : words)
        hash ^= std::hash<uint64_t>{}(word) + 0x9e3779b97f4a7c15ULL +
                (hash << 6) + (hash >> 2);
    };
    hash_words(encoder.code.words);
    hash_words(encoder.code.positions);
    auto& bucket = tape.codes[hash];
    for (const auto& code : bucket)
      if (code->words == encoder.code.words &&
          code->positions == encoder.code.positions) {
        frame->code = code;
        break;
      }
    if (!frame->code) {
      frame->code = std::make_shared<FrameCode>(std::move(encoder.code));
      bucket.push_back(frame->code);
    }
  }
  if (s.frame_layout) {
    auto& layouts = scratch.layouts;
    const auto found = std::find_if(layouts.begin(), layouts.end(),
                                   [&](const auto& layout) {
                                     return layout.code == frame->code;
                                   });
    if (found != layouts.end()) layouts.erase(found);
    // A bounded cache controls comparison cost. Every cache entry still
    // requires the full certificate; eviction only selects the slow fallback.
    if (layouts.size() == 4) layouts.pop_back();
    layouts.insert(layouts.begin(), {frame->code, st.program.size()});
  }
  const auto relocate = [&](const double* ptr) -> double* {
    const auto ref = encoder.local(ptr);
    return ref >= 0 ? frame->values.data() + ref : const_cast<double*>(ptr);
  };
  for (const auto& in : st.imports) {
    auto copy = in;
    copy.dst = in.len ? relocate(in.dst) : nullptr;
    tape.imports.push_back(copy);
  }
  for (int64_t id : s.target_refs) {
    tape.targets.push_back(relocate(s.versions[id].value));
    tape.target_adjoints.push_back(frame_adjoint(s, id));
  }
  s.target_refs.clear();
  for (auto& ptr : tape.outputs) ptr = relocate(ptr);

  auto& versions = scratch.versions;
  versions.clear();
  decltype(s.constant_cells) cells;
  decltype(tape.promotion_links) promotions;
  for (int64_t id : live_ids) {
    auto version = s.versions[id];
    version.value = lengths[id] ? relocate(version.value) : nullptr;
    versions.push_back(version);
    if (auto it = s.constant_cells.find(id); it != s.constant_cells.end())
      cells.emplace(remap[id], std::move(it->second));
    if (auto it = tape.promotion_links.find(id);
        it != tape.promotion_links.end())
      promotions.emplace(remap[id], it->second);
  }
  const auto rebind = [&](auto& refs) {
    for (auto& id : refs)
      if (id >= 0) id = remap[id];
  };
  rebind(s.bindings);
  rebind(s.node_version);
  rebind(s.node_version2);
  rebind(s.loop_version);
  s.versions.clear();
  for (size_t i = 0; i < versions.size(); ++i) {
    s.versions.push_back(versions[i]);
  }
  s.constant_cells = std::move(cells);
  tape.promotion_links = std::move(promotions);
  tape.adjoint_base = s.adjoint_size;
  tape.frames.push_back(std::move(frame));
  s.arena.clear();
  s.records.clear();
  st.clear_frame_recording();
}

struct FrameExecution {
  LoopState& s;
  KernelCtx& outer;
  LoopFrame& frame;
  double* value(uint64_t raw) const {
    const int64_t ref = static_cast<int64_t>(raw);
    if (ref == frame_null) return nullptr;
    return ref >= 0 ? frame.values.data() + ref : frame.bindings[-ref - 1];
  }
  double* adjoint(uint64_t raw) const {
    const int64_t ref = static_cast<int64_t>(raw);
    if (ref == frame_null) return nullptr;
    return ref >= 0 ? s.frames->adjoints.data() + frame.adjoint_base + ref
                    : frame_adjoint_ptr(frame.adj_bindings[-ref - 1], *s.frames,
                                        s.p, outer);
  }
  void call(const uint64_t* w, bool reverse) {
    const size_t site = w[0], flags = w[1], n_in = w[2];
    w += 3;
    KernelCtx& c = s.ctx[site];
    c.n_in = static_cast<int>(n_in);
    for (size_t k = 0; k < n_in; ++k) c.in[k].data = value(*w++);
    c.out.data = value(*w++);
    if (flags & kFrozenCallHasOut2) c.out2.data = value(*w++);
    c.scratch = value(*w++);
    if (c.dyn_lengths) apply_dynamic_length(c);
    if (!reverse) {
      s.sites[site]->forward(c);
      return;
    }
    for (size_t k = 0; k < n_in; ++k) c.in_adj[k].data = adjoint(*w++);
    c.out_adj_vec.data = adjoint(*w++);
    if (flags & kFrozenCallReusePrimal) c.scratch = nullptr;
    if (flags & kFrozenCallHasOut2) {
      double* a = adjoint(*w++);
      c.out2_adj = a ? *a : 0.0;
    }
    if (c.out.len == 1 && c.out_adj_vec.data) c.out_adj = c.out_adj_vec.data[0];
    s.sites[site]->backward(c);
  }
  bool forward() {
    const auto& code = *frame.code;
    for (size_t pc = 0; pc < code.words.size();) {
      const uint64_t header = code.words[pc];
      const uint64_t* w = code.words.data() + pc + 1;
      pc += header >> 8;
      switch (header & 255) {
        case StreamInstr::AdjointCall:
          break;
        case StreamInstr::Call:
          call(w, false);
          break;
        case StreamInstr::InPlace: {
          for (size_t k = 0; k < w[7]; ++k)
            if (*value(w[8 + 2 * k]) != frame.literals[w[9 + 2 * k]])
              return false;
          const int64_t* pos = code.positions.data() + w[1];
          double* old = frame.old.data() + w[2];
          double* base = value(w[3]);
          const double* rhs = value(w[4]);
          for (size_t k = 0; k < w[0]; ++k) {
            old[k] = base[pos[k]];
            base[pos[k]] = rhs[k];
          }
          break;
        }
        case StreamInstr::Copy:
          std::copy_n(value(w[1]), w[0], value(w[2]));
          break;
        case StreamInstr::Gather: {
          const int64_t* pos = code.positions.data() + w[1];
          const double* src = value(w[2]);
          double* dst = value(w[3]);
          for (size_t k = 0; k < w[0]; ++k) dst[k] = src[pos[k]];
          break;
        }
        case StreamInstr::GuardIf:
          if ((*value(w[0]) != 0.0) != (w[1] != 0)) return false;
          break;
        case StreamInstr::GuardFor:
          if (*value(w[0]) != frame.literals[w[2]] ||
              *value(w[1]) != frame.literals[w[3]])
            return false;
          break;
        case StreamInstr::Set:
          *value(w[0]) = frame.literals[w[1]];
          break;
        default:
          throw std::logic_error("invalid structured frame instruction");
      }
    }
    return true;
  }
  void backward() {
    const auto& code = *frame.code;
    for (size_t i = code.backward.size(); i-- > 0;) {
      const auto at = code.backward[i];
      const uint64_t* w = code.words.data() + at + 1;
      switch (code.words[at] & 255) {
        case StreamInstr::AdjointCall:
        case StreamInstr::Call:
          call(w, true);
          break;
        case StreamInstr::InPlace: {
          const int64_t* pos = code.positions.data() + w[1];
          const double* old = frame.old.data() + w[2];
          double* base = value(w[3]);
          double* a = adjoint(w[5]);
          double* rhs = adjoint(w[6]);
          for (size_t k = w[0]; k-- > 0;) {
            if (a) {
              if (rhs) rhs[k] += a[pos[k]];
              a[pos[k]] = 0;
            }
            base[pos[k]] = old[k];
          }
          break;
        }
        case StreamInstr::Copy: {
          double* from = adjoint(w[3]);
          double* to = adjoint(w[4]);
          if (from && to)
            for (size_t k = 0; k < w[0]; ++k) from[k] += to[k];
          break;
        }
        case StreamInstr::Gather: {
          const int64_t* pos = code.positions.data() + w[1];
          double* src = adjoint(w[4]);
          double* dst = adjoint(w[5]);
          if (src && dst)
            for (size_t k = 0; k < w[0]; ++k) src[pos[k]] += dst[k];
          break;
        }
        default:
          throw std::logic_error(
              "invalid structured frame reverse instruction");
      }
    }
  }
};

bool LoopState::clone_from(const KernelState& source) {
  const auto* src = dynamic_cast<const LoopState*>(&source);
  if (!frame_clone || no_replay || !(frame_mode || frame_auto) || !src ||
      &p != &src->p || !src->frame_clone_ready || src->building ||
      !src->frames || !src->frames->ready)
    return false;

  // Build privately. Only immutable FrameCode is shared. Contexts come from
  // the fresh LoopState constructor; its next forward binds new outer inputs
  // and evaluation resources and validates the inherited guards.
  const auto& old = *src->frames;
  auto next = std::make_unique<FrameTape>();
  std::vector<double> next_workspace = src->workspace;
  struct Range {
    uintptr_t first, end;
    double* destination;
  };
  std::vector<Range> ranges;
  const auto add = [&](const std::vector<double>& from,
                       std::vector<double>& to) {
    if (from.empty()) return;
    const auto first = reinterpret_cast<uintptr_t>(from.data());
    ranges.push_back({first, first + from.size() * sizeof(double), to.data()});
  };
  add(src->workspace, next_workspace);
  next->frames.reserve(old.frames.size());
  for (const auto& frame : old.frames) {
    auto copy = std::make_unique<LoopFrame>(*frame);
    add(frame->values, copy->values);
    add(frame->old, copy->old);
    add(frame->literals, copy->literals);
    next->frames.push_back(std::move(copy));
  }
  std::sort(ranges.begin(), ranges.end(),
            [](const Range& a, const Range& b) { return a.first < b.first; });
  bool closed = true;
  const auto relocate = [&](const double* ptr, int64_t len) -> double* {
    if (len == 0) return nullptr;
    if (ptr && len > 0) {
      const auto address = reinterpret_cast<uintptr_t>(ptr);
      auto it = std::upper_bound(
          ranges.begin(), ranges.end(), address,
          [](uintptr_t p, const Range& r) { return p < r.first; });
      if (it != ranges.begin()) {
        --it;
        const auto bytes = address - it->first;
        if (address < it->end && bytes % sizeof(double) == 0 &&
            static_cast<uint64_t>(len) <= (it->end - address) / sizeof(double))
          return it->destination + bytes / sizeof(double);
      }
    }
    closed = false;
    return nullptr;
  };
  for (auto& frame : next->frames)
    for (auto& ptr : frame->bindings) ptr = relocate(ptr, 1);
  next->imports = old.imports;
  for (auto& in : next->imports) in.dst = relocate(in.dst, in.len);
  next->outputs = old.outputs;
  for (size_t k = 0; k < next->outputs.size(); ++k)
    next->outputs[k] = relocate(next->outputs[k], p.body.slots[p.outputs[k]].len);
  next->targets = old.targets;
  for (auto& ptr : next->targets) ptr = relocate(ptr, 1);
  if (!closed) return false;

  next->codes = old.codes;
  next->output_adjoints = old.output_adjoints;
  next->target_adjoints = old.target_adjoints;
  next->promotions = old.promotions;
  next->promotion_links = old.promotion_links;
  next->adjoint_base = old.adjoint_base;
  next->adjoints.resize(old.adjoints.size());
  next->target_work.resize(old.targets.size());
  next->ready = true;
  workspace.swap(next_workspace);
  frames = std::move(next);
  reverse_ready = false;
  frame_clone_ready = true;
  if (diagnostics)
    emit_diagnostic("stanli_structured frame_clone: frames=" +
                    std::to_string(frames->frames.size()));
  return true;
}

void frame_outputs(LoopState& s, KernelCtx& ctx) {
  FrameTape& tape = *s.frames;
  int64_t at = 0;
  for (size_t k = 0; k < tape.outputs.size(); ++k) {
    const auto len = s.p.body.slots[s.p.outputs[k]].len;
    std::copy_n(tape.outputs[k], len, ctx.out.data + at);
    at += len;
  }
  if (s.p.has_target) {
    tape.target_work.resize(tape.targets.size());
    for (size_t k = 0; k < tape.targets.size(); ++k)
      tape.target_work[k] = *tape.targets[k];
    ctx.out.data[at] = reduce_target(tape.target_work);
  }
}

void finish_frames(LoopState& s, KernelCtx& ctx) {
  FrameTape& tape = *s.frames;
  for (int slot : s.p.outputs) {
    const auto id = s.bindings[slot];
    tape.outputs.push_back(s.versions[id].value);
    tape.output_adjoints.push_back(frame_adjoint(s, id));
  }
  seal_frame(s);
  tape.adjoints.resize(s.adjoint_size);
  tape.ready = true;
  frame_outputs(s, ctx);
  s.building.reset();
  tape.sealing.reset();
  s.versions.reset();
  decltype(s.constant_cells){}.swap(s.constant_cells);
  s.arena = BlockArena{};
  if (s.diagnostics) {
    size_t programs = 0, code_words = 0, cells = 0, bindings = 0;
    for (const auto& bucket : tape.codes)
      for (const auto& code : bucket.second) {
        ++programs;
        code_words += code->words.size();
      }
    for (const auto& frame : tape.frames) {
      cells += frame->values.size() + frame->old.size();
      bindings += frame->bindings.size() + frame->adj_bindings.size();
    }
    emit_diagnostic("stanli_structured frames: frames=" +
                    std::to_string(tape.frames.size()) +
                    " programs=" + std::to_string(programs) +
                    " code_words=" + std::to_string(code_words) +
                    " cells=" + std::to_string(cells) +
                    " bindings=" + std::to_string(bindings) +
                    " layout_attempts=" + std::to_string(tape.layout_attempts) +
                    " layout_matches=" + std::to_string(tape.layout_matches) +
                    " peak_versions=" + std::to_string(tape.version_peak) +
                    " recording_cells=" + std::to_string(tape.recording_cells));
  }
}

bool frames_forward(LoopState& s, KernelCtx& ctx) {
  for (auto& c : s.ctx) c.eval_state = ctx.eval_state;
  for (const auto& in : s.frames->imports) {
    const double* src = ctx.in[in.input].data + in.offset;
    if (in.data_only) {
      if (!std::equal(src, src + in.len, in.dst)) return false;
    } else {
      std::copy_n(src, in.len, in.dst);
    }
  }
  for (auto& frame : s.frames->frames)
    if (!FrameExecution{s, ctx, *frame}.forward()) return false;
  frame_outputs(s, ctx);
  return true;
}

void frames_backward(LoopState& s, KernelCtx& ctx) {
  FrameTape& tape = *s.frames;
  std::fill(tape.adjoints.begin(), tape.adjoints.end(), 0.0);
  int64_t at = 0;
  for (size_t k = 0; k < tape.outputs.size(); ++k) {
    const auto len = s.p.body.slots[s.p.outputs[k]].len;
    if (double* a = frame_adjoint_ptr(tape.output_adjoints[k], tape, s.p, ctx))
      for (int64_t j = 0; j < len; ++j) a[j] += ctx.out_adj_vec.data[at + j];
    at += len;
  }
  if (s.p.has_target)
    for (const auto& a : tape.target_adjoints)
      if (double* ptr = frame_adjoint_ptr(a, tape, s.p, ctx))
        *ptr += ctx.out_adj_vec.data[at];
  for (size_t k = tape.frames.size(); k-- > 0;)
    FrameExecution{s, ctx, *tape.frames[k]}.backward();
}

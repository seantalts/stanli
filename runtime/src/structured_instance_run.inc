// Frame instances. A later trip whose predicted template has a matching
// recorder state reruns the template's data plan instead of recording. It
// copies the template frame with a data patch and relocated bindings, sets
// the recorder state the template's seal produced, relocated, and then runs
// the frame forward to evaluate its parameter kernels and check its guards.
// A plan mismatch or exception records the trip normally; a guard mismatch
// records the whole evaluation again without instances.

struct Relocation {
  std::vector<int64_t> frames, promotions;
  std::unordered_map<int64_t, int64_t> adjoints;
  int64_t adjoint_base = 0;
  size_t promotions_base = 0;
};

bool Execution::compiled_recording() {
  if (recording_enabled < 0) {
    const char* option = std::getenv("STANLI_STRUCTURED_COMPILED_RECORDING");
    recording_enabled = !option || option[0] != '0';
  }
  return recording_enabled != 0;
}

void Execution::observe() {
  observed = true;
  if (capture) capture->refused = true;
}

FrameLoc Execution::locate(const double* ptr) {
  if (!ptr) return {};
  if (in_workspace(ptr)) return {FrameLoc::Workspace, ptr - s.workspace.data()};
  auto& in = *instances;
  const auto& frames = s.frames->frames;
  for (; in.indexed < frames.size(); ++in.indexed)
    if (!frames[in.indexed]->values.empty())
      in.by_address.emplace(frames[in.indexed]->values.data(), in.indexed);
  auto it = in.by_address.upper_bound(ptr);
  if (it != in.by_address.begin()) {
    --it;
    const auto& values = frames[it->second]->values;
    if (ptr < values.data() + values.size())
      return {static_cast<int64_t>(it->second), ptr - values.data()};
  }
  return {FrameLoc::Absolute,
          static_cast<int64_t>(reinterpret_cast<intptr_t>(ptr))};
}

void Execution::image_state(StateImage& out, int64_t local) {
  const FrameTape& tape = *s.frames;
  out.versions.resize(s.versions.size());
  for (size_t i = 0; i < s.versions.size(); ++i) {
    const Version& v = s.versions[i];
    FrameLoc at = locate(v.value);
    if (at.frame >= 0 && at.frame == local) at.frame = FrameLoc::Local;
    out.versions[i] = {at, v.adjoint, v.owner, v.constant};
  }
  out.bindings = s.bindings;
  out.node_version = s.node_version;
  out.node_version2 = s.node_version2;
  out.loop_version = s.loop_version;
  out.flag_keys.clear();
  for (const auto& entry : s.constant_cells)
    out.flag_keys.push_back(entry.first);
  std::sort(out.flag_keys.begin(), out.flag_keys.end());
  out.links.assign(tape.promotion_links.begin(), tape.promotion_links.end());
  std::sort(out.links.begin(), out.links.end());
  out.generations = generations();
  out.loop_generation = s.loop_generation;
  out.node_generation = s.node_generation;
  out.promotions = tape.promotions.size();
  out.targets = tape.targets.size();
  out.adjoint_size = s.adjoint_size;
  out.effects = s.effects;
}

std::vector<uint8_t> Execution::generations() const {
  std::vector<uint8_t> out(s.sites.size(), 0);
  for (size_t site = 0; site < s.sites.size(); ++site) {
    const int loop = s.sites[site]->invariant_loop;
    if (loop < 0) continue;
    const int64_t node = s.node_generation[site];
    out[site] = node < 0 ? 1 : node == s.loop_generation[loop] ? 3 : 2;
  }
  return out;
}

void Execution::begin_trip() {
  if (!instances) return;
  instances->placed_valid = false;
  capture = nullptr;
  instances->capture.reset();
  if (!instances->enabled || observed || s.frames->frames.size() < 2 ||
      !instances->successor.count(context(0)) || predicted_current())
    return;
  instances->capture = std::make_unique<TripCapture>();
  capture = instances->capture.get();
  capture->stream = s.building.get();
  image_state(capture->pre, FrameLoc::Null);
}

void Execution::end_loop() {
  if (!instances) return;
  capture = nullptr;
  instances->capture.reset();
}

void Execution::end_trip(Flow flow) {
  size_t instructions = 0;
  if (capture) {
    instructions = s.building->program.size();
    const BlockArena& arena = s.arena;
    for (size_t b = 0; b <= arena.cursor && b < arena.blocks.size(); ++b)
      capture->blocks.emplace_back(arena.blocks[b].data.data(),
                                   arena.blocks[b].capacity);
    for (const auto& entry : s.constant_cells)
      capture->keys.push_back(entry.first);
  }
  seal_frame(s);
  if (capture) {
    adopt(flow, instructions);
    capture = nullptr;
    instances->capture.reset();
  }
  note_frame();
}

// Whether the template this trip was predicted to repeat is still usable, so
// a fresh capture would replace it with an equivalent one.
bool Execution::predicted_current() const {
  const auto& frames = s.frames->frames;
  if (frames.size() < 2) return false;
  const auto next = instances->successor.find(context(0));
  if (next == instances->successor.end()) return false;
  const auto found =
      instances->templates.find({frames.back()->code.get(), next->second});
  return found != instances->templates.end() && !found->second->stale;
}

FrameInstances::Context Execution::context(size_t back) const {
  const auto& frames = s.frames->frames;
  const size_t at = frames.size() - 1 - back;
  return {frames[at - 1]->code.get(), frames[at]->code.get()};
}

void Execution::note_frame() {
  if (instances && s.frames->frames.size() >= 3)
    instances->successor[context(1)] = s.frames->frames.back()->code.get();
}

namespace {
int64_t operand_length(const StructuredLoop& p, const Op& op, int slot,
                       int bit) {
  return (op.dyn_lengths & (1u << bit)) ? op.dyn_capacity
                                        : p.body.slots[slot].len;
}
}  // namespace

// Calls f(raw, len, positions, write) for each value operand of a frame
// program in execution order. A positioned operand covers base + positions.
template <class F>
void frame_values(const LoopState& s, const FrameCode& code, F&& visit) {
  const auto& p = s.p;
  int64_t instruction = -1;
  const auto f = [&](uint64_t raw, int64_t len, const int64_t* pos,
                     bool write) { visit(raw, len, pos, write, instruction); };
  for (size_t pc = 0; pc < code.words.size();) {
    const uint64_t header = code.words[pc];
    const uint64_t* w = code.words.data() + pc + 1;
    pc += header >> 8;
    ++instruction;
    switch (header & 255) {
      case StreamInstr::AdjointCall:
      case StreamInstr::Call: {
        const Node& n = *s.sites[w[0]];
        const Op& op = p.body.ops[n.op];
        const uint64_t* at = w + 3;
        for (int k = 0; k < op.n_in; ++k)
          f(*at++, operand_length(p, op, op.in[k], k), nullptr, false);
        const bool call = (header & 255) == StreamInstr::Call;
        f(*at++, operand_length(p, op, op.out, 6), nullptr, call);
        if (w[1] & kFrozenCallHasOut2)
          f(*at++, p.body.slots[op.out2].len, nullptr, call);
        f(*at, n.kernel_scratch, nullptr, call);
        break;
      }
      case StreamInstr::InPlace: {
        const int64_t* pos = code.positions.data() + w[1];
        for (size_t k = 0; k < w[7]; ++k) f(w[8 + 2 * k], 1, nullptr, false);
        f(w[4], static_cast<int64_t>(w[0]), nullptr, false);
        f(w[3], static_cast<int64_t>(w[0]), pos, false);
        f(w[3], static_cast<int64_t>(w[0]), pos, true);
        break;
      }
      case StreamInstr::Copy:
        f(w[1], static_cast<int64_t>(w[0]), nullptr, false);
        f(w[2], static_cast<int64_t>(w[0]), nullptr, true);
        break;
      case StreamInstr::Gather:
        f(w[2], static_cast<int64_t>(w[0]), code.positions.data() + w[1],
          false);
        f(w[3], static_cast<int64_t>(w[0]), nullptr, true);
        break;
      case StreamInstr::GuardIf:
        f(w[0], 1, nullptr, false);
        break;
      case StreamInstr::GuardFor:
        f(w[0], 1, nullptr, false);
        f(w[1], 1, nullptr, false);
        break;
      case StreamInstr::Set:
        f(w[0], 1, nullptr, true);
        break;
      default:
        break;
    }
  }
}

void Execution::adopt(Flow flow, size_t instructions) {
  auto& in = *instances;
  TripCapture& c = *capture;
  FrameTape& tape = *s.frames;
  const LoopFrame& frame = *tape.frames.back();
  const size_t index = tape.frames.size() - 1;
  const auto refuse = [&]() { ++in.refused; };
  if (c.refused || !frame.literals.empty()) return refuse();

  auto t = std::make_unique<FrameTemplate>();
  t->frame = &frame;
  t->index = index;
  t->instructions = instructions;
  t->flow = flow;
  std::vector<size_t> block_image;
  for (const auto& block : c.blocks) {
    block_image.push_back(t->image_size);
    t->image_size += block.second;
  }
  const auto in_arena = [&](const double* ptr, size_t& offset) {
    for (size_t b = 0; b < c.blocks.size(); ++b)
      if (ptr >= c.blocks[b].first &&
          ptr < c.blocks[b].first + c.blocks[b].second) {
        offset = block_image[b] + static_cast<size_t>(ptr - c.blocks[b].first);
        return true;
      }
    return false;
  };

  t->bases = {
      {FrameLoc::Workspace, 0}, {FrameLoc::Local, 0}, {FrameLoc::Scratch, 0}};
  t->ops = std::move(c.ops);
  std::vector<int64_t> lengths(c.ptrs.size(), -1);
  for (const PlanOp& op : t->ops)
    plan_operands(op, [&](uint32_t r, int64_t len, Use) { lengths[r] = len; });
  std::map<std::pair<int64_t, int64_t>, int32_t> known;
  t->refs.reserve(c.ptrs.size());
  for (size_t i = 0; i < c.ptrs.size(); ++i) {
    const double* ptr = c.ptrs[i];
    PlanRef ref;
    size_t offset;
    if (!ptr) {
    } else if (lengths[i] == 0) {
      ref = {0, 0};
    } else if (in_arena(ptr, offset)) {
      if (offset > static_cast<size_t>(std::numeric_limits<int32_t>::max()))
        return refuse();
      ref = {2, static_cast<int32_t>(offset)};
    } else {
      FrameLoc at = locate(ptr);
      if (at.offset > std::numeric_limits<int32_t>::max() &&
          at.frame != FrameLoc::Absolute)
        return refuse();
      if (at.frame == FrameLoc::Workspace) {
        ref = {0, static_cast<int32_t>(at.offset)};
      } else {
        if (at.frame >= static_cast<int64_t>(index)) return refuse();
        const auto key = at.frame == FrameLoc::Absolute
                             ? std::make_pair(at.frame, at.offset)
                             : std::make_pair(at.frame, int64_t{0});
        const auto [entry, fresh] =
            known.emplace(key, static_cast<int32_t>(t->bases.size()));
        if (fresh) t->bases.push_back({key.first, key.second});
        ref = {entry->second, at.frame == FrameLoc::Absolute
                                  ? 0
                                  : static_cast<int32_t>(at.offset)};
      }
    }
    t->refs.push_back(ref);
  }
  t->seen = std::move(c.seen);
  t->values = frame.values;
  t->workspace = s.workspace;
  t->positions = std::move(c.positions);
  t->positional = std::move(c.positional);
  t->key_flags = std::move(c.key_flags);

  struct Range {
    size_t image, frame, len;
  };
  std::vector<Range> ranges;
  for (const auto& r : tape.sealing->snapshot.ranges) {
    size_t offset;
    if (!in_arena(r.base, offset)) return refuse();
    ranges.push_back({offset, r.offset, r.used});
  }
  // Arena operands the frame keeps refer to its values; the rest are scratch.
  std::sort(ranges.begin(), ranges.end(),
            [](const auto& a, const auto& b) { return a.image < b.image; });
  t->image_size = 0;
  std::vector<uint8_t> moved(t->refs.size(), 0);
  bool split = false;
  const auto keep = [&](uint32_t r, int64_t len) {
    PlanRef& ref = t->refs[r];
    if (ref.base != 2 || moved[r]) return;
    moved[r] = 1;
    const size_t at = static_cast<size_t>(ref.offset);
    const size_t end = at + static_cast<size_t>(std::max<int64_t>(len, 1));
    auto range = std::upper_bound(
        ranges.begin(), ranges.end(), at,
        [](size_t x, const auto& range) { return x < range.image; });
    if (range != ranges.begin() && at < (range - 1)->image + (range - 1)->len) {
      --range;
      split = split || end > range->image + range->len;
      ref = {1, static_cast<int32_t>(range->frame + (at - range->image))};
      return;
    }
    split = split || (range != ranges.end() && end > range->image);
    t->image_size = std::max(t->image_size, end);
  };
  for (const PlanOp& op : t->ops)
    plan_operands(op, [&](uint32_t r, int64_t len, Use) { keep(r, len); });
  for (uint32_t r = 0; r < t->refs.size(); ++r) keep(r, 1);
  if (split) return refuse();
  for (double* ptr : frame.bindings) {
    const FrameLoc at = locate(ptr);
    if (at.frame >= static_cast<int64_t>(index) || at.frame == FrameLoc::Null)
      return refuse();
    t->bindings.push_back(at);
  }

  // Pair each logged gather and in-place write with its frame instruction.
  t->fixed_slots.assign(frame.bindings.size(), 0);
  t->fixed_adj_slots.assign(frame.adj_bindings.size(), 0);
  size_t positioned = 0;
  std::vector<size_t> order;
  for (size_t i = 0; i < t->ops.size(); ++i)
    if (t->ops[i].kind == PlanOp::Gather ||
        (t->ops[i].kind == PlanOp::Write && !(t->ops[i].mode & kWriteConst)))
      order.push_back(static_cast<size_t>(t->ops[i].c));
  const auto mark_fixed = [](std::vector<uint8_t>& slots, uint64_t raw) {
    const int64_t ref = static_cast<int64_t>(raw);
    if (ref < 0 && ref != frame_null) slots[static_cast<size_t>(-ref - 1)] = 1;
  };
  const auto& code = *frame.code;
  for (size_t pc = 0; pc < code.words.size();) {
    const uint64_t header = code.words[pc];
    const uint64_t* w = code.words.data() + pc + 1;
    const size_t words = header >> 8;
    const auto kind = header & 255;
    if (kind == StreamInstr::Gather || kind == StreamInstr::InPlace) {
      if (positioned == order.size()) return refuse();
      Positional& entry = t->positional[order[positioned++]];
      const int64_t value =
          static_cast<int64_t>(w[kind == StreamInstr::Gather ? 2 : 3]);
      const int64_t adjoint =
          static_cast<int64_t>(w[kind == StreamInstr::Gather ? 4 : 5]);
      entry.fixed = value >= 0 || adjoint >= 0;
      entry.slot = value < 0 && value != frame_null ? -value - 1 : -1;
      entry.adj_slot = adjoint < 0 && adjoint != frame_null ? -adjoint - 1 : -1;
      if (kind == StreamInstr::Gather) {
        mark_fixed(t->fixed_slots, w[3]);
        mark_fixed(t->fixed_adj_slots, w[5]);
      } else {
        mark_fixed(t->fixed_slots, w[4]);
        mark_fixed(t->fixed_adj_slots, w[6]);
        for (size_t k = 0; k < w[7]; ++k)
          mark_fixed(t->fixed_slots, w[8 + 2 * k]);
      }
    } else if (kind == StreamInstr::Call || kind == StreamInstr::AdjointCall) {
      const Node& n = *s.sites[w[0]];
      const Op& op = s.p.body.ops[n.op];
      const size_t operands = op.n_in + 1 + ((w[1] & kFrozenCallHasOut2) != 0);
      for (size_t k = 0; k < operands + 1; ++k)
        mark_fixed(t->fixed_slots, w[3 + k]);
      if (w[1] & kFrozenCallActive)
        for (size_t k = 0; k < operands; ++k)
          mark_fixed(t->fixed_adj_slots, w[4 + operands + k]);
    } else if (kind == StreamInstr::Copy) {
      mark_fixed(t->fixed_slots, w[1]);
      mark_fixed(t->fixed_slots, w[2]);
      mark_fixed(t->fixed_adj_slots, w[3]);
      mark_fixed(t->fixed_adj_slots, w[4]);
    } else if (kind == StreamInstr::GuardIf || kind == StreamInstr::Set) {
      mark_fixed(t->fixed_slots, w[0]);
    } else if (kind == StreamInstr::GuardFor) {
      mark_fixed(t->fixed_slots, w[0]);
      mark_fixed(t->fixed_slots, w[1]);
    }
    pc += words;
  }
  if (positioned != order.size()) return refuse();

  // Frame code may read only workspace cells it wrote itself. A cell both
  // programs write keeps the value of its last writer in recording order.
  std::vector<int64_t> code_last(s.workspace.size(), -1),
      plan_last(s.workspace.size(), -1);
  bool foreign = false;
  frame_values(s, code,
               [&](uint64_t raw, int64_t len, const int64_t* pos, bool write,
                   int64_t instruction) {
                 const int64_t ref = static_cast<int64_t>(raw);
                 if (ref >= 0 || ref == frame_null || len <= 0) return;
                 const FrameLoc& at =
                     t->bindings[static_cast<size_t>(-ref - 1)];
                 if (at.frame != FrameLoc::Workspace) return;
                 for (int64_t k = 0; k < len; ++k) {
                   const size_t cell =
                       static_cast<size_t>(at.offset + (pos ? pos[k] : k));
                   if (write)
                     code_last[cell] = instruction;
                   else if (code_last[cell] < 0)
                     foreign = true;
                 }
               });
  if (foreign) return refuse();
  for (auto& op : t->ops) {
    if (op.kind == PlanOp::Write) {
      if (t->refs[op.refs].base == 0) return refuse();
      if (t->refs[op.refs].base <= 2) op.mode |= kWriteLocal;
    }
    const auto* spec = op.kind == PlanOp::Write || op.kind == PlanOp::Gather ||
                               op.kind == PlanOp::Cells ||
                               (op.kind == PlanOp::Kernel &&
                                s.sites[op.site]->forward == index_forward &&
                                !s.ctx[op.site].dyn_lengths)
                           ? static_cast<const DynamicIndexSpec*>(
                                 s.p.body.ops[s.sites[op.site]->op].udata)
                           : nullptr;
    if (spec && static_selection(*spec)) op.mode |= kPrepared;
    plan_operands(op, [&](uint32_t r, int64_t len, Use use) {
      const PlanRef& ref = t->refs[r];
      if (use == Use::Write && ref.base == 0)
        for (int64_t k = 0; k < len; ++k)
          plan_last[static_cast<size_t>(ref.offset + k)] = op.clock;
    });
  }
  for (size_t cell = 0; cell < plan_last.size(); ++cell)
    if (plan_last[cell] > code_last[cell])
      t->ws_plan.push_back(static_cast<uint32_t>(cell));

  t->pre = std::move(c.pre);
  image_state(t->post, static_cast<int64_t>(index));
  const auto& remap = tape.sealing->remap;
  for (int64_t key : c.keys)
    if (remap[static_cast<size_t>(key)] >= 0)
      t->flag_remap.emplace_back(key, remap[static_cast<size_t>(key)]);
  if (t->flag_remap.size() != t->post.flag_keys.size()) return refuse();
  t->promotion_writes = std::move(c.promotions);
  for (size_t k = t->pre.targets; k < tape.targets.size(); ++k) {
    FrameLoc at = locate(tape.targets[k]);
    if (at.frame == static_cast<int64_t>(index)) at.frame = FrameLoc::Local;
    t->targets.emplace_back(at, tape.target_adjoints[k]);
  }
  for (const auto& [key, version] : t->flag_remap)
    t->final_flags[key] = s.constant_cells.at(version);
  plan_accesses(*t);
  std::vector<const Access*> outside;
  for (const Access& a : t->accesses)
    if (a.use != Use::Read && a.first == a.last && a.ref >= 0 &&
        t->refs[a.ref].base > 2)
      outside.push_back(&a);
  for (const Access& a : t->accesses) {
    if (a.use != Use::Read || a.first != a.last || a.ref < 0 || a.len < 2 ||
        a.len > (int64_t{1} << 16) || t->array_at.count(a.ref))
      continue;
    const PlanRef& ref = t->refs[a.ref];
    if (ref.base < 3 || a.len > 4096 ||
        std::any_of(outside.begin(), outside.end(), [&](const Access* w) {
          const PlanRef& to = t->refs[w->ref];
          return to.base == ref.base && to.offset < ref.offset + a.len &&
                 ref.offset < to.offset + w->len;
        }))
      continue;
    const FrameLoc& base = t->bases[static_cast<size_t>(ref.base)];
    const double* from =
        base.frame == FrameLoc::Absolute
            ? reinterpret_cast<const double*>(
                  static_cast<intptr_t>(base.offset))
            : tape.frames[static_cast<size_t>(base.frame)]->values.data();
    t->array_at[a.ref] = {t->array_values.size(), a.len};
    t->array_values.insert(t->array_values.end(), from + ref.offset,
                           from + ref.offset + a.len);
  }
  auto& kept = in.templates[context(0)];
  t->varying.assign(t->refs.size(), 0);
  if (kept && kept->refs.size() == t->refs.size()) t->varying = kept->varying;
  plan_schedule(*t);
  kept = std::move(t);
}

// Visits each operand an op reads, writes, or may write at positions it
// computes, with its length.
template <class F>
void Execution::plan_operands(const PlanOp& op, F&& f) const {
  uint32_t r = op.refs;
  const auto inputs = [&](int first) {
    const Op& o = p.body.ops[s.sites[op.site]->op];
    for (int k = first; k < o.n_in; ++k)
      f(r + static_cast<uint32_t>(k), p.body.slots[o.in[k]].len, Use::Read);
  };
  switch (op.kind) {
    case PlanOp::Kernel: {
      const Node& n = *s.sites[op.site];
      const Op& o = p.body.ops[n.op];
      for (int k = 0; k < o.n_in; ++k)
        f(r++, operand_length(p, o, o.in[k], k), Use::Read);
      f(r++, operand_length(p, o, o.out, 6), Use::Write);
      if (o.out2 >= 0) f(r++, p.body.slots[o.out2].len, Use::Write);
      f(r, n.kernel_scratch, Use::Write);
      break;
    }
    case PlanOp::Index: {
      const Op& o = p.body.ops[s.sites[op.site]->op];
      inputs(0);
      f(r + static_cast<uint32_t>(o.n_in), 1, Use::Write);
      break;
    }
    case PlanOp::Decide:
      f(r, 1, Use::Read);
      break;
    case PlanOp::Bounds:
      f(r, 1, Use::Read);
      f(r + 1, 1, Use::Read);
      break;
    case PlanOp::Iterate:
    case PlanOp::OuterIterate:
      f(r, 1, Use::Write);
      break;
    case PlanOp::Copy:
      f(r, op.a, Use::Read);
      f(r + 1, op.a, Use::Write);
      break;
    case PlanOp::Write:
      f(r, p.body.slots[p.body.ops[s.sites[op.site]->op].in[0]].len,
        Use::Touch);
      inputs(1);
      break;
    case PlanOp::Gather:
    case PlanOp::Cells:
      inputs(1);
      break;
    case PlanOp::Memo: {
      if (op.expect) break;
      const auto& program = *reinterpret_cast<const RecordingProgram*>(op.b);
      const auto& candidate = program.candidates[program.code[op.a].candidate];
      for (int slot : candidate.raw) f(r++, p.body.slots[slot].len, Use::Read);
      for (const Node* node : candidate.invariants) {
        const Op& o = p.body.ops[node->op];
        for (int k = 0; k < o.n_in; ++k)
          f(r++, p.body.slots[o.in[k]].len, Use::Read);
      }
      break;
    }
    default:
      break;
  }
}

// Lists each op's accesses once per template. Workspace and arena storage is
// split into segments between the bounds of all operand ranges; each flag
// vector the plan touches is one more unit after them. Other storage is
// outside the trip and has no unit.
void Execution::plan_accesses(FrameTemplate& t) const {
  const int64_t workspace = static_cast<int64_t>(s.workspace.size());
  const int64_t frame = static_cast<int64_t>(t.values.size());
  const auto start = [&](const PlanRef& ref) -> int64_t {
    if (ref.base == 0) return ref.offset;
    if (ref.base == 1) return workspace + ref.offset;
    if (ref.base == 2) return workspace + frame + ref.offset;
    return -1;
  };
  std::vector<uint8_t> marks(
      static_cast<size_t>(workspace + frame) + t.image_size + 1, 0);
  for (const PlanOp& op : t.ops)
    plan_operands(op, [&](uint32_t r, int64_t len, Use) {
      const int64_t at = start(t.refs[r]);
      if (at < 0 || len <= 0) return;
      marks[static_cast<size_t>(at)] = 1;
      marks[static_cast<size_t>(at + len)] = 1;
    });
  t.bounds.clear();
  for (size_t k = 0; k < marks.size(); ++k)
    if (marks[k]) t.bounds.push_back(static_cast<int64_t>(k));
  const uint32_t segments =
      t.bounds.empty() ? 0 : static_cast<uint32_t>(t.bounds.size() - 1);
  std::unordered_map<int64_t, uint32_t> flags;
  t.flag_units.clear();
  const auto flag = [&](int64_t version) {
    const auto [entry, fresh] = flags.emplace(
        version, segments + static_cast<uint32_t>(t.flag_units.size()));
    if (fresh) t.flag_units.push_back(version);
    return entry->second;
  };
  t.accesses.clear();
  t.access_at.assign(t.ops.size() + 1, 0);
  for (size_t i = 0; i < t.ops.size(); ++i) {
    const PlanOp& op = t.ops[i];
    t.access_at[i] = static_cast<uint32_t>(t.accesses.size());
    int rhs = -1;
    if (op.kind == PlanOp::Write)
      rhs = require_index_input_layout(
                *static_cast<const DynamicIndexSpec*>(
                    p.body.ops[s.sites[op.site]->op].udata),
                true)
                .rhs;
    plan_operands(op, [&](uint32_t r, int64_t len, Use use) {
      Access access{0, 0, use, false, static_cast<int32_t>(r), len};
      access.rhs = rhs >= 0 && r == op.refs + static_cast<uint32_t>(rhs);
      const int64_t at = start(t.refs[r]);
      if (at >= 0 && len > 0) {
        const auto first =
            std::lower_bound(t.bounds.begin(), t.bounds.end(), at);
        const auto last = std::lower_bound(first, t.bounds.end(), at + len);
        access.first = static_cast<uint32_t>(first - t.bounds.begin());
        access.last = static_cast<uint32_t>(last - t.bounds.begin());
      }
      t.accesses.push_back(access);
    });
    const auto flag_access = [&](int64_t version, Use use) {
      const uint32_t unit = flag(version);
      t.accesses.push_back({unit, unit + 1, use, false, -1, 0});
    };
    if (op.kind == PlanOp::FlagsOnes || op.kind == PlanOp::FlagsZeros)
      flag_access(op.a, Use::Write);
    if (op.kind == PlanOp::FlagsCopy) {
      flag_access(op.b, Use::Read);
      flag_access(op.a, Use::Write);
    }
    if (op.kind == PlanOp::Write && (op.mode & kWriteCells))
      flag_access(op.a, Use::Touch);
    if (op.kind == PlanOp::Write && (op.mode & kWriteRhsCells))
      flag_access(op.b, Use::Read);
    if (op.kind == PlanOp::Cells) flag_access(op.a, Use::Read);
  }
  t.access_at[t.ops.size()] = static_cast<uint32_t>(t.accesses.size());
}

// An op whose inputs repeat the template's has the template's results. Such
// an op runs only when a later running op reads what it writes; storage and
// flags it writes last get the template's final values instead. Inputs from
// before the trip are compared with the template's first.
void Execution::plan_schedule(FrameTemplate& t) const {
  if (t.access_at.empty()) plan_accesses(t);
  const int64_t workspace = static_cast<int64_t>(s.workspace.size());
  const size_t segments = t.bounds.empty() ? 0 : t.bounds.size() - 1;
  const size_t units = segments + t.flag_units.size();
  const auto accesses = [&](size_t i) {
    return std::make_pair(t.accesses.data() + t.access_at[i],
                          t.accesses.data() + t.access_at[i + 1]);
  };
  std::vector<std::vector<std::pair<int64_t, int64_t>>> written(t.bases.size());
  const auto initial = [&](const PlanRef& ref, int64_t len) {
    if (t.bases[static_cast<size_t>(ref.base)].frame != 0) return false;
    for (const auto& [from, size] : s.frames->initial)
      if (ref.offset >= from && ref.offset + len <= from + size) return true;
    return false;
  };
  t.reads_initial = false;
  const auto untouched = [&](const PlanRef& ref, int64_t len) {
    for (const auto& [from, to] : written[static_cast<size_t>(ref.base)])
      if (ref.offset < to && ref.offset + len > from) return false;
    return true;
  };
  enum : uint8_t { Unwritten, Repeated, Varying };
  std::vector<uint8_t> state(units, Unwritten), loop(p.loop_count, Varying);
  std::vector<uint8_t> repeated(t.ops.size(), 0);
  std::map<std::pair<int32_t, int64_t>, uint32_t> checks;
  std::vector<uint32_t> pending, arrays;
  for (size_t i = 0; i < t.ops.size(); ++i) {
    const PlanOp& op = t.ops[i];
    bool same = false;
    switch (op.kind) {
      case PlanOp::Kernel:
        same = !s.ctx[op.site].dyn_lengths;
        break;
      case PlanOp::Write:
        same = (op.mode & kWriteLocal) != 0;
        break;
      case PlanOp::Iterate:
        same = loop[op.site] == Repeated;
        break;
      case PlanOp::Decide:
      case PlanOp::Bounds:
      case PlanOp::Gather:
      case PlanOp::Cells:
      case PlanOp::Copy:
      case PlanOp::FlagsOnes:
      case PlanOp::FlagsZeros:
      case PlanOp::FlagsCopy:
        same = true;
        break;
      default:
        break;
    }
    const bool relevant_rhs =
        op.kind != PlanOp::Write ||
        (op.mode & (kWriteConst | kWriteRhsConst | kWriteRhsCells));
    pending.clear();
    arrays.clear();
    const auto [begin, end] = accesses(i);
    for (const Access* a = begin; same && a != end; ++a) {
      if (a->use == Use::Write || (a->rhs && !relevant_rhs)) continue;
      if (a->first < a->last) {
        for (uint32_t k = a->first; k < a->last; ++k)
          same = same && state[k] == Repeated;
        if (same || a->use == Use::Touch || state[a->first] != Unwritten)
          continue;
        same = a->last - a->first == 1 && a->len == 1 &&
               t.refs[a->ref].base == 0 && !t.varying[a->ref];
        if (same) pending.push_back(a->ref);
        continue;
      }
      if (a->ref < 0 || a->len == 0) continue;
      const PlanRef& ref = t.refs[a->ref];
      if (ref.base < 0) continue;
      if (a->use != Use::Touch && initial(ref, a->len)) {
        t.reads_initial = true;
      } else if (a->use == Use::Touch || !untouched(ref, a->len) ||
                 t.varying[a->ref]) {
        same = false;
      } else if (a->len == 1) {
        pending.push_back(a->ref);
      } else {
        same = t.array_at.count(static_cast<uint32_t>(a->ref)) != 0;
        if (same) arrays.push_back(static_cast<uint32_t>(a->ref));
      }
    }
    if (same) {
      for (uint32_t r : pending)
        checks.emplace(std::make_pair(t.refs[r].base, t.refs[r].offset), r);
      for (uint32_t r : arrays)
        checks.emplace(
            std::make_pair(-1 - t.refs[r].base,
                           t.refs[r].offset * 65536 + t.array_at.at(r).second),
            r);
    }
    repeated[i] = same;
    if (op.kind == PlanOp::Bounds) loop[op.site] = same ? Repeated : Varying;
    for (const Access* a = begin; a != end; ++a) {
      if (a->use == Use::Read) continue;
      if (a->first < a->last) {
        std::fill(state.begin() + a->first, state.begin() + a->last,
                  same ? Repeated : Varying);
      } else if (a->ref >= 0 && t.refs[a->ref].base >= 0) {
        const PlanRef& ref = t.refs[a->ref];
        written[static_cast<size_t>(ref.base)].emplace_back(
            ref.offset, ref.offset + a->len);
      }
    }
  }
  std::vector<uint8_t> needed(units, 0), loop_needed(p.loop_count, 0),
      runs(t.ops.size(), 0);
  for (size_t i = t.ops.size(); i-- > 0;) {
    const PlanOp& op = t.ops[i];
    const auto [begin, end] = accesses(i);
    bool run =
        !repeated[i] || (op.kind == PlanOp::Bounds && loop_needed[op.site]);
    for (const Access* a = begin; !run && a != end; ++a)
      if (a->use != Use::Read)
        run = std::any_of(needed.begin() + a->first, needed.begin() + a->last,
                          [](uint8_t v) { return v != 0; });
    runs[i] = run;
    if (!run) continue;
    for (const Access* a = begin; a != end; ++a)
      if (a->use == Use::Write)
        std::fill(needed.begin() + a->first, needed.begin() + a->last, 0);
    if (op.kind == PlanOp::Bounds) loop_needed[op.site] = 0;
    for (const Access* a = begin; a != end; ++a)
      if (a->use != Use::Write)
        std::fill(needed.begin() + a->first, needed.begin() + a->last, 1);
    if (op.kind == PlanOp::Iterate) loop_needed[op.site] = 1;
  }
  std::vector<int64_t> writer(units, -1);
  t.schedule.clear();
  for (size_t i = 0; i < t.ops.size(); ++i) {
    if (runs[i]) t.schedule.push_back(static_cast<uint32_t>(i));
    const auto [begin, end] = accesses(i);
    for (const Access* a = begin; a != end; ++a)
      if (a->use != Use::Read)
        std::fill(writer.begin() + a->first, writer.begin() + a->last,
                  static_cast<int64_t>(i));
  }
  std::map<std::pair<int32_t, int32_t>, double> known;
  for (const auto& [r, value] : t.checks)
    known.emplace(std::make_pair(t.refs[r].base, t.refs[r].offset), value);
  t.checks.clear();
  t.array_checks.clear();
  for (const auto& [key, r] : checks) {
    if (key.first < 0) {
      t.array_checks.push_back(r);
      continue;
    }
    const auto value = known.find({t.refs[r].base, t.refs[r].offset});
    t.checks.emplace_back(r, value != known.end() ? value->second : t.seen[r]);
  }
  std::vector<uint8_t> restore(units, 0);
  for (size_t k = 0; k < units; ++k)
    restore[k] = writer[k] >= 0 && !runs[static_cast<size_t>(writer[k])];
  const auto restored = [&](int64_t at) {
    const auto it = std::upper_bound(t.bounds.begin(), t.bounds.end(), at);
    return it != t.bounds.begin() && it != t.bounds.end() &&
           restore[static_cast<size_t>(it - t.bounds.begin() - 1)];
  };
  t.ws_restore.clear();
  for (int64_t k = 0; k < workspace; ++k)
    if (restored(k))
      t.ws_restore.emplace_back(static_cast<uint32_t>(k),
                                t.workspace[static_cast<size_t>(k)]);
  t.frame_restore.clear();
  for (size_t k = 0; k < t.values.size(); ++k)
    if (restored(workspace + static_cast<int64_t>(k)))
      t.frame_restore.push_back(static_cast<uint32_t>(k));
  t.flag_restore.clear();
  for (size_t k = 0; k < t.flag_units.size(); ++k)
    if (restore[segments + k]) t.flag_restore.push_back(t.flag_units[k]);
  std::vector<double>().swap(t.seen);
  std::vector<Access>().swap(t.accesses);
  std::vector<uint32_t>().swap(t.access_at);
  std::vector<int64_t>().swap(t.bounds);
  std::vector<int64_t>().swap(t.flag_units);
}

bool Execution::admit(const FrameTemplate& t, Relocation& r) {
  const StateImage& pre = t.pre;
  const FrameTape& tape = *s.frames;
  if (s.versions.size() != pre.versions.size() || s.bindings != pre.bindings ||
      s.node_version != pre.node_version ||
      s.node_version2 != pre.node_version2 ||
      s.loop_version != pre.loop_version ||
      s.constant_cells.size() != pre.flag_keys.size() ||
      tape.promotion_links.size() != pre.links.size())
    return false;
  r.frames.assign(t.index, -1);
  r.adjoints.clear();
  for (size_t i = 0; i < pre.versions.size(); ++i) {
    const VersionImage& a = pre.versions[i];
    const Version& v = s.versions[i];
    const auto& in = *instances;
    const FrameLoc at = in.placed_valid ? in.placed[i] : locate(v.value);
    if (v.owner != a.owner || v.constant != a.constant) return false;
    if (a.value.frame >= 0) {
      if (at.frame < 0 || at.offset != a.value.offset) return false;
      int64_t& mapped = r.frames[static_cast<size_t>(a.value.frame)];
      if (mapped >= 0 && mapped != at.frame) return false;
      mapped = at.frame;
    } else if (!(at == a.value)) {
      return false;
    }
    if (a.adjoint >= 0) {
      if (v.adjoint < 0) return false;
      const auto [entry, fresh] = r.adjoints.emplace(a.adjoint, v.adjoint);
      if (!fresh && entry->second != v.adjoint) return false;
    } else if (v.adjoint != a.adjoint) {
      return false;
    }
  }
  for (int64_t key : pre.flag_keys)
    if (!s.constant_cells.count(key)) return false;
  r.promotions.assign(pre.promotions, -1);
  for (const auto& [version, index] : pre.links) {
    const auto found = tape.promotion_links.find(version);
    if (found == tape.promotion_links.end()) return false;
    r.promotions[static_cast<size_t>(index)] = found->second;
  }
  if (generations() != pre.generations) return false;
  r.adjoint_base = tape.adjoint_base;
  r.promotions_base = tape.promotions.size();
  return true;
}

double* Execution::place(const Relocation& r, const FrameLoc& at,
                         LoopFrame& local, bool& ok) const {
  switch (at.frame) {
    case FrameLoc::Null:
      return nullptr;
    case FrameLoc::Workspace:
      return s.workspace.data() + at.offset;
    case FrameLoc::Absolute:
      return reinterpret_cast<double*>(static_cast<intptr_t>(at.offset));
    case FrameLoc::Local:
      return local.values.data() + at.offset;
    default: {
      const int64_t frame = r.frames[static_cast<size_t>(at.frame)];
      if (frame < 0) {
        ok = false;
        return nullptr;
      }
      return s.frames->frames[static_cast<size_t>(frame)]->values.data() +
             at.offset;
    }
  }
}

int64_t relocate_adjoint(const FrameTemplate& t, const Relocation& r,
                         int64_t id, bool& ok) {
  if (id < 0) return id;
  if (id >= t.frame->adjoint_base)
    return id - t.frame->adjoint_base + r.adjoint_base;
  const auto found = r.adjoints.find(id);
  if (found == r.adjoints.end()) {
    ok = false;
    return -1;
  }
  return found->second;
}

int64_t relocate_promotion(const FrameTemplate& t, const Relocation& r,
                           int64_t index, bool& ok) {
  if (index < 0) return index;
  if (static_cast<size_t>(index) >= t.pre.promotions)
    return static_cast<int64_t>(r.promotions_base) + index -
           static_cast<int64_t>(t.pre.promotions);
  const int64_t mapped = r.promotions[static_cast<size_t>(index)];
  if (mapped < 0) ok = false;
  return mapped;
}

FrameAdjoint relocate_frame_adjoint(const FrameTemplate& t, const Relocation& r,
                                    FrameAdjoint a, bool& ok) {
  if (a.promotion >= 0)
    a.promotion = relocate_promotion(t, r, a.promotion, ok);
  else
    a.id = relocate_adjoint(t, r, a.id, ok);
  return a;
}

bool Execution::run_plan(FrameTemplate& t, const Relocation& r,
                         LoopFrame& frame, double outer_value) {
  auto& in = *instances;
  in.image.resize(t.image_size);
  in.bases.resize(t.bases.size());
  for (size_t b = 0; b < t.bases.size(); ++b) {
    const FrameLoc& base = t.bases[b];
    if (base.frame == FrameLoc::Workspace) {
      in.bases[b] = s.workspace.data();
    } else if (base.frame == FrameLoc::Local) {
      in.bases[b] = frame.values.data();
    } else if (base.frame == FrameLoc::Scratch) {
      in.bases[b] = in.image.data();
    } else if (base.frame == FrameLoc::Absolute) {
      in.bases[b] =
          reinterpret_cast<double*>(static_cast<intptr_t>(base.offset));
    } else {
      const int64_t frame = r.frames[static_cast<size_t>(base.frame)];
      if (frame < 0) return false;
      in.bases[b] = s.frames->frames[static_cast<size_t>(frame)]->values.data();
    }
  }
  const auto ref = [&](uint32_t i) -> double* {
    const PlanRef& at = t.refs[i];
    return at.base < 0 ? nullptr
                       : in.bases[static_cast<size_t>(at.base)] + at.offset;
  };
  const bool checking = !in.written.empty();
  const auto wrote = [&](const double* ptr, int64_t len) {
    if (!checking || !ptr || ptr < frame.values.data() ||
        ptr >= frame.values.data() + frame.values.size())
      return;
    std::fill_n(in.written.begin() + (ptr - frame.values.data()), len, 1);
  };
  in.registers.resize(p.loop_count);
  in.shifts.assign(t.positional.size(), 0);
  in.flags = s.constant_cells;
  const auto bind = [&](KernelCtx& c, const Op& op, uint32_t at) {
    c.n_in = op.n_in;
    for (int k = 0; k < op.n_in; ++k)
      c.in[k].data = ref(at + static_cast<uint32_t>(k));
    return at + static_cast<uint32_t>(op.n_in);
  };
  const auto same_positions = [&](size_t index) {
    const Positional& expected = t.positional[index];
    if (in.scratch.size() != expected.count) return false;
    const int64_t* pos = t.positions.data() + expected.offset;
    for (size_t k = 0; k < expected.count; ++k)
      if (in.scratch[k] - in.scratch[0] != pos[k] - pos[0]) return false;
    in.shifts[index] = expected.count ? in.scratch[0] - pos[0] : 0;
    return true;
  };
  std::set<std::pair<int32_t, int32_t>> changed;
  const auto vary = [&](uint32_t i) {
    changed.emplace(t.refs[i].base, t.refs[i].offset);
  };
  for (const auto& [i, value] : t.checks)
    if (std::memcmp(ref(i), &value, sizeof(double)) != 0) vary(i);
  for (uint32_t i : t.array_checks) {
    const auto& [at, len] = t.array_at.at(i);
    if (std::memcmp(ref(i), t.array_values.data() + at,
                    static_cast<size_t>(len) * sizeof(double)) != 0)
      vary(i);
  }
  bool full = !changed.empty();
  if (full && ++t.reschedules > 4)
    for (const auto& check : t.checks) vary(check.first);
  if (full) {
    for (size_t k = 0; k < t.refs.size(); ++k)
      if (changed.count({t.refs[k].base, t.refs[k].offset})) t.varying[k] = 1;
    plan_schedule(t);
  }
  if (t.reads_initial && r.frames[0] != 0) full = true;
  const size_t count = full ? t.ops.size() : t.schedule.size();
  for (size_t step = 0; step < count; ++step) {
    const PlanOp& op = t.ops[full ? step : t.schedule[step]];
    switch (op.kind) {
      case PlanOp::Kernel: {
        const Node& n = *s.sites[op.site];
        const Op& o = p.body.ops[n.op];
        KernelCtx& c = s.ctx[op.site];
        uint32_t at = bind(c, o, op.refs);
        c.out.data = ref(at++);
        if (o.out2 >= 0) c.out2.data = ref(at++);
        c.scratch = ref(at);
        if (c.dyn_lengths) apply_dynamic_length(c);
        if (op.mode & kPrepared) {
          std::fill_n(c.out.data, c.out.len, 0.0);
          prepared_positions(*static_cast<const DynamicIndexSpec*>(o.udata), c,
                             [&](int64_t i, int64_t from) {
                               c.out.data[i] = c.in[0].data[from];
                             });
        } else {
          n.forward(c);
        }
        wrote(c.out.data, operand_length(p, o, o.out, 6));
        if (o.out2 >= 0) wrote(c.out2.data, p.body.slots[o.out2].len);
        wrote(c.scratch, n.kernel_scratch);
        break;
      }
      case PlanOp::Index: {
        const Node& n = *s.sites[op.site];
        const Op& o = p.body.ops[n.op];
        KernelCtx& c = s.ctx[op.site];
        c.out.data = ref(bind(c, o, op.refs));
        prepared_index_read(*static_cast<const DynamicIndexSpec*>(o.udata), c);
        wrote(c.out.data, 1);
        break;
      }
      case PlanOp::Decide:
        if ((ref(op.refs)[0] != 0.0) != op.expect) return false;
        break;
      case PlanOp::Bounds: {
        const double lo = ref(op.refs)[0], hi = ref(op.refs + 1)[0];
        if (!std::isfinite(lo) || !std::isfinite(hi) || std::trunc(lo) != lo ||
            std::trunc(hi) != hi || lo < std::numeric_limits<int32_t>::min() ||
            hi < std::numeric_limits<int32_t>::min() ||
            lo > std::numeric_limits<int32_t>::max() ||
            hi > std::numeric_limits<int32_t>::max())
          return false;
        if ((hi >= lo ? static_cast<int64_t>(hi - lo) + 1 : 0) != op.a)
          return false;
        in.registers[op.site] = lo;
        break;
      }
      case PlanOp::Iterate:
        *ref(op.refs) = in.registers[op.site] + static_cast<double>(op.a);
        wrote(ref(op.refs), 1);
        break;
      case PlanOp::OuterIterate:
        *ref(op.refs) = outer_value;
        wrote(ref(op.refs), 1);
        break;
      case PlanOp::Copy:
        std::copy_n(ref(op.refs), op.a, ref(op.refs + 1));
        wrote(ref(op.refs + 1), op.a);
        break;
      case PlanOp::Write: {
        const Node& n = *s.sites[op.site];
        const Op& o = p.body.ops[n.op];
        KernelCtx& c = s.ctx[op.site];
        bind(c, o, op.refs);
        const auto& spec = *static_cast<const DynamicIndexSpec*>(o.udata);
        const IndexInputLayout layout = require_index_input_layout(spec, true);
        double* values = c.in[0].data;
        const double* rhs = c.in[layout.rhs].data;
        c.out.data = values;
        std::vector<uint8_t>* cells = nullptr;
        const std::vector<uint8_t>* rhs_cells = nullptr;
        if (op.mode & kWriteCells) {
          const auto found = in.flags.find(op.a);
          if (found == in.flags.end()) return false;
          cells = &found->second;
        }
        if (op.mode & kWriteRhsCells) {
          const auto found = in.flags.find(op.b);
          if (found == in.flags.end()) return false;
          rhs_cells = &found->second;
        }
        auto* undo = op.mode & kWriteLocal   ? nullptr
                     : op.mode & kWriteConst ? &in.undo
                                             : &in.revert;
        const bool rhs_constant = op.mode & kWriteRhsConst;
        in.scratch.clear();
        const auto write = [&](int64_t i, int64_t at) {
          if (undo) undo->emplace_back(values + at, values[at]);
          values[at] = rhs[i];
          wrote(values + at, 1);
          if (cells)
            (*cells)[static_cast<size_t>(at)] =
                rhs_constant ||
                (rhs_cells && (*rhs_cells)[static_cast<size_t>(i)]);
          in.scratch.push_back(at);
        };
        if (op.mode & kPrepared)
          prepared_positions(spec, c, write);
        else
          index_positions(spec, c, true, write);
        if (!(op.mode & kWriteConst) &&
            !same_positions(static_cast<size_t>(op.c)))
          return false;
        break;
      }
      case PlanOp::Gather: {
        const Node& n = *s.sites[op.site];
        const Op& o = p.body.ops[n.op];
        KernelCtx& c = s.ctx[op.site];
        bind(c, o, op.refs);
        in.scratch.clear();
        const auto& spec = *static_cast<const DynamicIndexSpec*>(o.udata);
        const auto add = [&](int64_t, int64_t at) { in.scratch.push_back(at); };
        if (op.mode & kPrepared)
          prepared_positions(spec, c, add);
        else
          index_positions(spec, c, false, add);
        if (!same_positions(static_cast<size_t>(op.c))) return false;
        break;
      }
      case PlanOp::Cells: {
        const Node& n = *s.sites[op.site];
        const Op& o = p.body.ops[n.op];
        KernelCtx& c = s.ctx[op.site];
        bind(c, o, op.refs);
        const auto found = in.flags.find(op.a);
        if (found == in.flags.end()) return false;
        const auto& spec = *static_cast<const DynamicIndexSpec*>(o.udata);
        bool constant = true;
        if (op.mode & kPrepared) {
          int64_t selected = 0;
          prepared_positions(spec, c, [&](int64_t, int64_t at) {
            ++selected;
            constant = constant && found->second[static_cast<size_t>(at)];
          });
          constant = constant && selected == c.out.len;
        } else {
          constant = selected_cells_constant(found->second, spec, c);
        }
        if (constant != op.expect) return false;
        break;
      }
      case PlanOp::FlagsOnes:
      case PlanOp::FlagsZeros:
        in.flags.emplace(op.a,
                         std::vector<uint8_t>(static_cast<size_t>(op.b),
                                              op.kind == PlanOp::FlagsOnes));
        break;
      case PlanOp::FlagsCopy: {
        const auto found = in.flags.find(op.b);
        if (found == in.flags.end()) return false;
        std::vector<uint8_t> copy = found->second;
        in.flags.emplace(op.a, std::move(copy));
        break;
      }
      case PlanOp::Memo: {
        auto& program = *reinterpret_cast<RecordingProgram*>(op.b);
        const size_t pc = static_cast<size_t>(op.a);
        auto& candidate = program.candidates[program.code[pc].candidate];
        if (candidate.disabled != op.expect) return false;
        if (candidate.disabled) break;
        in.memo_undo.push_back({&candidate, candidate.hits, candidate.skipped,
                                candidate.events, candidate.disabled});
        uint32_t at = op.refs;
        size_t flag = op.site;
        const bool keyed = memo_key(program, candidate, [&](int) {
          return std::make_pair(const_cast<const double*>(ref(at++)),
                                t.key_flags[flag++] != 0);
        });
        bool trace;
        uint64_t hash;
        const DataLoopMemo* chosen =
            memo_select(program, pc, keyed, trace, hash);
        if (trace || chosen != reinterpret_cast<const DataLoopMemo*>(op.c))
          return false;
        break;
      }
    }
  }
  if (!full) {
    for (const auto& [cell, value] : t.ws_restore) s.workspace[cell] = value;
    for (uint32_t at : t.frame_restore) {
      frame.values[at] = t.values[at];
      wrote(frame.values.data() + at, 1);
    }
    for (int64_t version : t.flag_restore) {
      const auto found = t.final_flags.find(version);
      if (found != t.final_flags.end()) in.flags[version] = found->second;
    }
  }
  for (const auto& entry : t.flag_remap)
    if (!in.flags.count(entry.first)) return false;
  return true;
}

// Everything a trip can change, for the instance checker.
struct RecorderSnapshot {
  std::vector<Version> versions;
  std::vector<int64_t> bindings, node_version, node_version2, loop_version;
  std::unordered_map<int64_t, std::vector<uint8_t>> constant_cells;
  std::unordered_map<int64_t, int64_t> links;
  std::vector<int64_t> promotions, loop_generation, node_generation;
  int64_t adjoint_size = 0, adjoint_base = 0;
  uint64_t effects = 0;
  std::vector<double> workspace;
  std::vector<const double*> targets;
  std::vector<FrameAdjoint> target_adjoints;
  std::vector<std::vector<double>> values, old;
  std::vector<FrameSealScratch::Layout> layouts;
  std::vector<std::array<size_t, 8>> memo;
  size_t frames = 0;
};

void Execution::snapshot(RecorderSnapshot& out) const {
  const FrameTape& tape = *s.frames;
  out.versions.assign(s.versions.data(), s.versions.data() + s.versions.size());
  out.bindings = s.bindings;
  out.node_version = s.node_version;
  out.node_version2 = s.node_version2;
  out.loop_version = s.loop_version;
  out.constant_cells = s.constant_cells;
  out.links = tape.promotion_links;
  out.promotions = tape.promotions;
  out.loop_generation = s.loop_generation;
  out.node_generation = s.node_generation;
  out.adjoint_size = s.adjoint_size;
  out.adjoint_base = tape.adjoint_base;
  out.effects = s.effects;
  out.workspace = s.workspace;
  out.targets = tape.targets;
  out.target_adjoints = tape.target_adjoints;
  out.values.clear();
  out.old.clear();
  for (const auto& frame : tape.frames) {
    out.values.push_back(frame->values);
    out.old.push_back(frame->old);
  }
  out.layouts = tape.sealing->layouts;
  out.memo.clear();
  for (const auto& program : recording)
    if (program)
      for (const auto& c : program->candidates)
        out.memo.push_back({c.hits, c.misses, c.aborted, c.skipped, c.events,
                            c.stored, c.words, c.disabled});
  out.frames = tape.frames.size();
}

void Execution::restore(const RecorderSnapshot& from) {
  FrameTape& tape = *s.frames;
  auto& in = *instances;
  in.placed_valid = false;
  while (tape.frames.size() > from.frames) {
    in.by_address.erase(tape.frames.back()->values.data());
    tape.frames.pop_back();
  }
  in.indexed = std::min(in.indexed, tape.frames.size());
  s.versions.clear();
  for (const Version& v : from.versions) s.versions.push_back(v);
  s.bindings = from.bindings;
  s.node_version = from.node_version;
  s.node_version2 = from.node_version2;
  s.loop_version = from.loop_version;
  s.constant_cells = from.constant_cells;
  tape.promotion_links = from.links;
  tape.promotions = from.promotions;
  s.loop_generation = from.loop_generation;
  s.node_generation = from.node_generation;
  s.adjoint_size = from.adjoint_size;
  tape.adjoint_base = from.adjoint_base;
  s.effects = from.effects;
  s.workspace = from.workspace;
  tape.targets = from.targets;
  tape.target_adjoints = from.target_adjoints;
  for (size_t k = 0; k < from.frames; ++k) {
    tape.frames[k]->values = from.values[k];
    tape.frames[k]->old = from.old[k];
  }
  tape.sealing->layouts = from.layouts;
  size_t at = 0;
  for (const auto& program : recording)
    if (program)
      for (auto& c : program->candidates) {
        const auto& m = from.memo[at++];
        c.hits = m[0];
        c.misses = m[1];
        c.aborted = m[2];
        c.skipped = m[3];
        c.events = m[4];
        c.stored = m[5];
        c.words = m[6];
        c.disabled = m[7] != 0;
      }
}

// Records the instantiated trip again from the same state and requires the
// same frame and recorder state. Cells neither program writes nor reads are
// excluded: a recording leaves arena garbage there.
void Execution::check_instance(const RecorderSnapshot& before,
                               const std::function<Flow()>& record, Flow flow) {
  FrameTape& tape = *s.frames;
  RecorderSnapshot instance;
  snapshot(instance);
  instances->by_address.erase(tape.frames.back()->values.data());
  const std::unique_ptr<LoopFrame> kept = std::move(tape.frames.back());
  tape.frames.pop_back();
  const LoopFrame& copy = *kept;
  const auto plan_cells = instances->written;
  restore(before);
  const Flow recorded = record();
  RecorderSnapshot normal;
  snapshot(normal);
  const LoopFrame& frame = *tape.frames.back();
  note_frame();
  const auto require = [](bool ok, const char* what) {
    if (!ok)
      throw std::logic_error(std::string("structured frame instance check: ") +
                             what);
  };
  const auto same = [](const std::vector<double>& a,
                       const std::vector<double>& b) {
    return a.size() == b.size() &&
           std::memcmp(a.data(), b.data(), a.size() * sizeof(double)) == 0;
  };
  require(recorded == flow, "flow");
  require(frame.values.size() == copy.values.size() &&
              equivalent_frames(frame, copy),
          "frame program");
  require(same(frame.old, copy.old), "old values");
  std::vector<uint8_t> used(frame.values.size(), 0);
  frame_values(
      s, *frame.code,
      [&](uint64_t raw, int64_t len, const int64_t* pos, bool, int64_t) {
        const int64_t ref = static_cast<int64_t>(raw);
        if (ref < 0) return;
        for (int64_t k = 0; k < len; ++k)
          used[static_cast<size_t>(ref + (pos ? pos[k] : k))] = 1;
      });
  for (size_t k = 0; k < used.size(); ++k)
    if ((used[k] || plan_cells[k]) &&
        std::memcmp(&frame.values[k], &copy.values[k], sizeof(double)) != 0)
      throw std::logic_error("structured frame instance check: frame value " +
                             std::to_string(k) + " of " +
                             std::to_string(used.size()) + ": " +
                             std::to_string(copy.values[k]) + " recorded " +
                             std::to_string(frame.values[k]) + " frame " +
                             std::to_string(tape.frames.size() - 1));
  const auto local = [&](const LoopFrame& f, const double* ptr) {
    return ptr >= f.values.data() && ptr < f.values.data() + f.values.size()
               ? ptr - f.values.data()
               : -1;
  };
  const auto same_ptr = [&](const double* a, const double* b) {
    const auto x = local(copy, a), y = local(frame, b);
    return x >= 0 || y >= 0 ? x == y : a == b;
  };
  require(instance.versions.size() == normal.versions.size(), "versions");
  for (size_t i = 0; i < normal.versions.size(); ++i) {
    const Version& a = instance.versions[i];
    const Version& b = normal.versions[i];
    if (!(same_ptr(a.value, b.value) && a.adjoint == b.adjoint &&
          a.owner == b.owner && a.constant == b.constant))
      throw std::logic_error(
          "structured frame instance check: version " + std::to_string(i) +
          " value " + std::to_string(local(copy, a.value)) + "/" +
          std::to_string(local(frame, b.value)) + " adjoint " +
          std::to_string(a.adjoint) + "/" + std::to_string(b.adjoint) +
          " owner " + std::to_string(a.owner) + "/" + std::to_string(b.owner) +
          " constant " + std::to_string(a.constant) + "/" +
          std::to_string(b.constant));
  }
  require(instance.bindings == normal.bindings &&
              instance.node_version == normal.node_version &&
              instance.node_version2 == normal.node_version2 &&
              instance.loop_version == normal.loop_version,
          "version bindings");
  require(instance.constant_cells == normal.constant_cells, "constant cells");
  require(instance.links == normal.links &&
              instance.promotions == normal.promotions,
          "promotions");
  require(instance.loop_generation == normal.loop_generation &&
              instance.node_generation == normal.node_generation &&
              instance.effects == normal.effects,
          "generations");
  require(instance.adjoint_size == normal.adjoint_size &&
              instance.adjoint_base == normal.adjoint_base,
          "adjoints");
  require(same(instance.workspace, normal.workspace), "workspace");
  require(instance.targets.size() == normal.targets.size() &&
              instance.target_adjoints == normal.target_adjoints,
          "targets");
  for (size_t k = 0; k < normal.targets.size(); ++k)
    require(same_ptr(instance.targets[k], normal.targets[k]), "target");
  for (size_t k = 0; k + 1 < normal.frames; ++k)
    require(same(instance.values[k], normal.values[k]) &&
                same(instance.old[k], normal.old[k]),
            "earlier frames");
  require(instance.memo == normal.memo, "memo counters");
}

bool Execution::instance_trip(double outer_value, Flow& flow,
                              const std::function<Flow()>& record) {
  if (!instancing || observed || !compiled_recording()) return false;
  if (!instances) instances = std::make_unique<FrameInstances>();
  auto& in = *instances;
  FrameTape& tape = *s.frames;
  if (!in.enabled || tape.frames.size() < 2) return false;
  const auto next = in.successor.find(context(0));
  if (next == in.successor.end()) return false;
  const auto found =
      in.templates.find({tape.frames.back()->code.get(), next->second});
  if (found == in.templates.end()) return false;
  FrameTemplate& t = *found->second;
  const auto failed = [&]() {
    ++in.fallback;
    if (in.fallback > 64 && in.fallback > 4 * in.instantiated)
      in.enabled = false;
    return false;
  };
  Relocation r;
  if (!admit(t, r)) {
    t.stale = true;
    return failed();
  }
  std::unique_ptr<RecorderSnapshot> before;
  if (s.check_frame_instance) {
    before = std::make_unique<RecorderSnapshot>();
    snapshot(*before);
    in.written.assign(t.frame->values.size(), 0);
  }
  auto frame = std::make_unique<LoopFrame>();
  frame->code = t.frame->code;
  frame->adjoint_base = tape.adjoint_base;
  frame->values = t.values;
  frame->old.assign(t.frame->old.size(), 0.0);
  bool ok = true;
  for (const FrameLoc& at : t.bindings)
    frame->bindings.push_back(place(r, at, *frame, ok));
  for (const FrameAdjoint& a : t.frame->adj_bindings)
    frame->adj_bindings.push_back(relocate_frame_adjoint(t, r, a, ok));
  if (!ok) return failed();

  in.workspace = s.workspace;
  in.undo.clear();
  in.revert.clear();
  in.memo_undo.clear();
  try {
    ok = run_plan(t, r, *frame, outer_value);
  } catch (const std::exception&) {
    ok = false;
  }
  if (ok) ok = shift_bindings(t, *frame);
  if (!ok) {
    std::copy(in.workspace.begin(), in.workspace.end(), s.workspace.begin());
    for (size_t k = in.revert.size(); k-- > 0;)
      *in.revert[k].first = in.revert[k].second;
    for (size_t k = in.undo.size(); k-- > 0;)
      *in.undo[k].first = in.undo[k].second;
    for (size_t k = in.memo_undo.size(); k-- > 0;) {
      const MemoCounts& m = in.memo_undo[k];
      m.candidate->hits = m.hits;
      m.candidate->skipped = m.skipped;
      m.candidate->events = m.events;
      m.candidate->disabled = m.disabled;
    }
    t.stale = true;
    return failed();
  }
  for (size_t k = in.revert.size(); k-- > 0;)
    *in.revert[k].first = in.revert[k].second;
  LoopFrame& added = *frame;
  commit(t, r, std::move(frame));
  forward_instance(t, added);
  flow = static_cast<Flow>(t.flow);
  ++in.instantiated;
  if (before) check_instance(*before, record, flow);
  return true;
}

bool Execution::shift_bindings(const FrameTemplate& t, LoopFrame& frame) {
  auto& in = *instances;
  std::vector<double*> moved(frame.bindings.size(), nullptr);
  std::vector<int64_t> moved_adj(frame.adj_bindings.size(), 0);
  std::vector<uint8_t> seen(frame.bindings.size(), 0),
      seen_adj(frame.adj_bindings.size(), 0);
  for (size_t j = 0; j < t.positional.size(); ++j) {
    const Positional& entry = t.positional[j];
    const int64_t delta = in.shifts[j];
    if (delta == 0) {
      if (entry.slot >= 0) {
        if (seen[entry.slot] && moved[entry.slot] != frame.bindings[entry.slot])
          return false;
        seen[entry.slot] = 1;
        moved[entry.slot] = frame.bindings[entry.slot];
      }
      if (entry.adj_slot >= 0) {
        if (seen_adj[entry.adj_slot] && moved_adj[entry.adj_slot] != 0)
          return false;
        seen_adj[entry.adj_slot] = 1;
      }
      continue;
    }
    if (entry.fixed) return false;
    if (entry.slot >= 0) {
      const size_t slot = static_cast<size_t>(entry.slot);
      if (t.fixed_slots[slot]) return false;
      double* ptr = frame.bindings[slot] + delta;
      if (seen[slot] && moved[slot] != ptr) return false;
      seen[slot] = 1;
      moved[slot] = ptr;
    }
    if (entry.adj_slot >= 0) {
      const size_t slot = static_cast<size_t>(entry.adj_slot);
      if (t.fixed_adj_slots[slot]) return false;
      if (seen_adj[slot] && moved_adj[slot] != delta) return false;
      seen_adj[slot] = 1;
      moved_adj[slot] = delta;
    }
  }
  for (size_t slot = 0; slot < moved.size(); ++slot)
    if (seen[slot]) frame.bindings[slot] = moved[slot];
  for (size_t slot = 0; slot < moved_adj.size(); ++slot)
    frame.adj_bindings[slot].shift += moved_adj[slot];
  return true;
}

void Execution::commit(const FrameTemplate& t, const Relocation& r,
                       std::unique_ptr<LoopFrame> frame) {
  auto& in = *instances;
  FrameTape& tape = *s.frames;
  LoopFrame& f = *frame;
  bool ok = true;
  s.versions.clear();
  in.placed.clear();
  for (const VersionImage& v : t.post.versions) {
    s.versions.push_back(Version{place(r, v.value, f, ok),
                                 relocate_adjoint(t, r, v.adjoint, ok), v.owner,
                                 v.constant});
    FrameLoc at = v.value;
    if (at.frame == FrameLoc::Local)
      at.frame = static_cast<int64_t>(tape.frames.size());
    else if (at.frame >= 0)
      at.frame = r.frames[static_cast<size_t>(at.frame)];
    in.placed.push_back(at);
  }
  in.placed_valid = true;
  s.bindings = t.post.bindings;
  s.node_version = t.post.node_version;
  s.node_version2 = t.post.node_version2;
  s.loop_version = t.post.loop_version;
  decltype(s.constant_cells) cells;
  for (const auto& [key, version] : t.flag_remap)
    cells.emplace(version, std::move(in.flags[key]));
  s.constant_cells = std::move(cells);
  const size_t added = t.post.promotions - t.pre.promotions;
  tape.promotions.resize(tape.promotions.size() + added, -1);
  for (const auto& [index, adjoint] : t.promotion_writes)
    tape.promotions[static_cast<size_t>(relocate_promotion(t, r, index, ok))] =
        relocate_adjoint(t, r, adjoint, ok);
  tape.promotion_links.clear();
  for (const auto& [version, index] : t.post.links)
    tape.promotion_links.emplace(version, relocate_promotion(t, r, index, ok));
  s.adjoint_size = r.adjoint_base + t.post.adjoint_size - t.frame->adjoint_base;
  tape.adjoint_base = s.adjoint_size;
  s.effects += t.post.effects - t.pre.effects;
  for (size_t l = 0; l < s.loop_generation.size(); ++l)
    s.loop_generation[l] +=
        t.post.loop_generation[l] - t.pre.loop_generation[l];
  for (size_t site = 0; site < s.sites.size(); ++site) {
    const int loop = s.sites[site]->invariant_loop;
    if (loop < 0 || t.post.node_generation[site] == t.pre.node_generation[site])
      continue;
    s.node_generation[site] =
        s.loop_generation[loop] -
        (t.post.loop_generation[loop] - t.post.node_generation[site]);
  }
  for (const auto& [at, adjoint] : t.targets) {
    tape.targets.push_back(place(r, at, f, ok));
    tape.target_adjoints.push_back(relocate_frame_adjoint(t, r, adjoint, ok));
  }
  if (!ok)
    throw std::logic_error("structured frame instance lost a relocation");
  if (s.frame_layout) {
    auto& layouts = tape.sealing->layouts;
    const auto found =
        std::find_if(layouts.begin(), layouts.end(),
                     [&](const auto& layout) { return layout.code == f.code; });
    if (found != layouts.end()) layouts.erase(found);
    if (layouts.size() == 4) layouts.pop_back();
    layouts.insert(layouts.begin(), {f.code, t.instructions});
  }
  tape.frames.push_back(std::move(frame));
  note_frame();
}

// Workspace cells the plan wrote last keep the plan's values.
void Execution::forward_instance(const FrameTemplate& t, LoopFrame& frame) {
  auto& kept = instances->kept;
  kept.clear();
  for (uint32_t cell : t.ws_plan) kept.emplace_back(cell, s.workspace[cell]);
  if (!FrameExecution{s, outer, frame}.forward()) throw FrameInstanceRestart{};
  for (const auto& [cell, value] : kept) s.workspace[cell] = value;
}

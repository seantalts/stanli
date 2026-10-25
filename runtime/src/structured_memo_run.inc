// Replay and tracing of memoized data loops (see structured_memo.inc).

void Execution::trace_stepped(RecordingProgram& program, DataLoopTrace& trace,
                              int slot) {
  if (!program.memo_stepped[slot]) trace.stepped.push_back(slot);
  program.memo_stepped[slot] = 1;
}

void Execution::trace_inputs(RecordingProgram& program, DataLoopTrace& trace,
                             const Op& op) {
  for (int k = 0; k < op.n_in; ++k)
    if (program.memo_stepped[op.in[k]]) trace.ok = false;
}

void Execution::trace_condition(RecordingProgram& program, DataLoopTrace& trace,
                                int slot) {
  if (!is_const(s.bindings[slot]) || program.memo_stepped[slot])
    trace.ok = false;
}

void Execution::trace_bump(DataLoopTrace& trace, int loop) {
  const auto& loops = trace.candidate->bump_loops;
  if (std::find(loops.begin(), loops.end(), loop) == loops.end()) return;
  trace.memo->events.push_back({DataLoopMemo::Bump, loop, 0, 0});
}

void Execution::trace_bound(RecordingProgram& program, DataLoopTrace& trace,
                            int slot) {
  if (!program.memo_bound[slot]) trace.bound.push_back(slot);
  program.memo_bound[slot] = 1;
  program.memo_stepped[slot] = 0;
}

void Execution::trace_unbound(RecordingProgram& program, int slot) {
  program.memo_bound[slot] = 0;
}

void Execution::trace_cells(RecordingProgram& program, DataLoopTrace& trace,
                            const Node& n, int slot) {
  program.memo_stepped[slot] = 0;
  if (n.storage != Node::Transient) {
    trace_bound(program, trace, slot);
    return;
  }
  const int64_t length = p.body.slots[slot].len;
  for (int64_t i = 0; i < length; ++i) {
    uint8_t& cell = program.memo_cells[static_cast<size_t>(n.workspace + i)];
    if (!cell) {
      cell = 1;
      trace.cells.push_back(static_cast<uint32_t>(n.workspace + i));
    }
  }
  if (trace.seen.insert(&n).second) trace.written.push_back(&n);
}

void Execution::trace_capture(RecordingProgram& program, DataLoopTrace& trace,
                              int slot, int64_t length) {
  auto& memo = *trace.memo;
  if (program.memo_bound[slot] == 1) {
    const double* ptr = value(slot);
    memo.events.push_back({DataLoopMemo::Slot, slot,
                           static_cast<uint32_t>(memo.pool.size()),
                           static_cast<uint32_t>(length)});
    memo.pool.insert(memo.pool.end(), ptr, ptr + length);
    program.memo_bound[slot] = 2;
    return;
  }
  if (program.memo_bound[slot] == 2) return;
  const double* ptr = value(slot);
  if (!in_workspace(ptr) || !is_const(s.bindings[slot])) return;
  const size_t first = static_cast<size_t>(ptr - s.workspace.data());
  for (size_t i = 0; i < static_cast<size_t>(length);) {
    if (!program.memo_cells[first + i]) {
      ++i;
      continue;
    }
    size_t j = i;
    while (j < static_cast<size_t>(length) && program.memo_cells[first + j])
      ++j;
    memo.events.push_back({DataLoopMemo::Cells, static_cast<int32_t>(first + i),
                           static_cast<uint32_t>(memo.pool.size()),
                           static_cast<uint32_t>(j - i)});
    memo.pool.insert(memo.pool.end(), ptr + i, ptr + j);
    i = j;
  }
}

void Execution::trace_kernel(RecordingProgram& program, DataLoopTrace& trace,
                             size_t pc) {
  const Node& n = *program.code[pc].node;
  const Op& op = p.body.ops[n.op];
  auto& memo = *trace.memo;
  bool pure = program.pure_candidate[pc] != 0;
  for (int k = 0; pure && k < op.n_in; ++k) {
    if (!is_const(s.bindings[op.in[k]])) {
      pure = false;
    } else if (program.memo_stepped[op.in[k]]) {
      trace.ok = false;
      pure = false;
    }
  }
  const uint64_t effects_before = s.effects;
  if (pure) {
    const size_t logged = s.building->program.size();
    const int64_t adjoints = s.adjoint_size;
    execute_kernel(n);
    if (s.building->program.size() != logged || s.adjoint_size != adjoints ||
        !is_const(s.bindings[op.out])) {
      trace.ok = false;
      trace_unbound(program, op.out);
      return;
    }
    trace_bound(program, trace, op.out);
    return;
  }
  for (int k = 0; k < op.n_in; ++k) {
    const int slot = op.in[k];
    const int64_t length =
        (op.dyn_lengths & (1u << k)) ? op.dyn_capacity : p.body.slots[slot].len;
    trace_capture(program, trace, slot, length);
  }
  memo.events.push_back({DataLoopMemo::Step, static_cast<int32_t>(pc), 0, 0});
  if (memo.events.size() > kMemoMaxEvents) trace.ok = false;
  execute_kernel(n);
  trace.step_effects += s.effects - effects_before;
  if (n.storage == Node::InPlace) {
    trace_unbound(program, op.in[0]);
    trace_stepped(program, trace, op.in[0]);
  }
  trace_unbound(program, op.out);
  trace_stepped(program, trace, op.out);
  if (op.out2 >= 0) {
    trace_unbound(program, op.out2);
    trace_stepped(program, trace, op.out2);
  }
}

void Execution::trace_alias(RecordingProgram& program, DataLoopTrace& trace,
                            size_t pc) {
  const Node& n = *program.code[pc].node;
  const int64_t version = s.bindings[n.src];
  bool pure = program.pure_candidate[pc] && is_const(version);
  if (pure && (program.memo_stepped[n.src] ||
               in_workspace(s.versions[static_cast<size_t>(version)].value))) {
    trace.ok = false;
    pure = false;
  }
  if (pure) {
    execute_alias(n);
    trace_bound(program, trace, n.dst);
    return;
  }
  trace_capture(program, trace, n.src, p.body.slots[n.src].len);
  trace.memo->events.push_back(
      {DataLoopMemo::Step, static_cast<int32_t>(pc), 0, 0});
  const uint64_t effects_before = s.effects;
  execute_alias(n);
  trace.step_effects += s.effects - effects_before;
  trace_unbound(program, n.dst);
  trace_stepped(program, trace, n.dst);
}

void Execution::trace_target(RecordingProgram& program, DataLoopTrace& trace,
                             size_t pc) {
  const Node& n = *program.code[pc].node;
  trace_capture(program, trace, n.src, p.body.slots[n.src].len);
  trace.memo->events.push_back(
      {DataLoopMemo::Step, static_cast<int32_t>(pc), 0, 0});
  const uint64_t effects_before = s.effects;
  execute_target(n);
  trace.step_effects += s.effects - effects_before;
}

void Execution::run_step(RecordingProgram& program, size_t pc) {
  const Node& n = *program.code[pc].node;
  switch (program.code[pc].kind) {
    case RecordingProgram::Kernel:
      execute_kernel(n);
      break;
    case RecordingProgram::Alias:
      execute_alias(n);
      break;
    case RecordingProgram::Target:
      execute_target(n);
      break;
    default:
      throw std::logic_error("invalid memoized recording step");
  }
}

void Execution::memo_replay(RecordingProgram& program,
                            const DataLoopMemo& memo) {
  const auto apply = [&](const DataLoopMemo::Event& e) {
    switch (e.kind) {
      case DataLoopMemo::Cells:
        std::copy_n(memo.pool.data() + e.offset, e.length,
                    s.workspace.data() + e.arg);
        break;
      case DataLoopMemo::Slot: {
        double* cell = s.arena.allocate(e.length);
        std::copy_n(memo.pool.data() + e.offset, e.length, cell);
        s.bindings[e.arg] = make_version(cell, -1, true);
        break;
      }
      case DataLoopMemo::Bump:
        ++s.loop_generation[e.arg];
        break;
      case DataLoopMemo::Step:
        run_step(program, static_cast<size_t>(e.arg));
        break;
    }
  };
  for (const auto& e : memo.events) apply(e);
  for (const auto& e : memo.finals) apply(e);
  s.effects += memo.skipped_effects;
}

bool Execution::memo_key(RecordingProgram& program,
                         const DataLoopCandidate& candidate) {
  auto& key = program.memo_key;
  key.clear();
  const auto bits = [](double x) {
    uint64_t word;
    std::memcpy(&word, &x, sizeof word);
    return word;
  };
  for (int slot : candidate.raw) {
    const int64_t version = s.bindings[slot];
    key.push_back(is_const(version));
    const double* ptr = s.versions[static_cast<size_t>(version)].value;
    const int64_t length = p.body.slots[slot].len;
    for (int64_t i = 0; i < length; ++i) key.push_back(bits(ptr[i]));
  }
  for (const Node* node : candidate.invariants) {
    const Op& op = p.body.ops[node->op];
    KernelCtx context = s.ctx[node->site];
    for (int k = 0; k < op.n_in; ++k) {
      const int64_t version = s.bindings[op.in[k]];
      key.push_back(is_const(version));
      context.in[k].data = s.versions[static_cast<size_t>(version)].value;
    }
    const int64_t length = p.body.slots[op.out].len;
    auto& scratch = program.memo_scratch;
    scratch.assign(static_cast<size_t>(length + node->kernel_scratch), 0.0);
    context.out.data = scratch.data();
    context.scratch = scratch.data() + length;
    try {
      node->forward(context);
    } catch (const std::exception&) {
      return false;
    }
    for (int64_t i = 0; i < length; ++i) key.push_back(bits(scratch[i]));
  }
  return true;
}

size_t Execution::memo_enter(RecordingProgram& program, size_t pc) {
  auto& candidate = program.candidates[program.code[pc].candidate];
  if (candidate.disabled) return no_memo;
  const auto retire = [&]() {
    const size_t refused = candidate.misses + candidate.skipped;
    if (candidate.hits + refused >= 32 && candidate.hits < 2 * refused)
      candidate.disabled = true;
  };
  if (!memo_key(program, candidate)) {
    ++candidate.skipped;
    retire();
    return no_memo;
  }
  const auto& key = program.memo_key;
  uint64_t hash = 1469598103934665603ULL;
  for (uint64_t word : key) hash = (hash ^ word) * 1099511628211ULL;
  const auto known = candidate.memos.find(hash);
  if (known != candidate.memos.end())
    for (const auto& memo : known->second) {
      if (memo->key != key) continue;
      if (!memo->usable) {
        ++candidate.skipped;
        retire();
        return no_memo;
      }
      memo_replay(program, *memo);
      ++candidate.hits;
      candidate.events += memo->events.size();
      return candidate.exit;
    }
  if (candidate.stored >= kMemoMaxMemos ||
      candidate.pool_doubles >= kMemoMaxCandidatePool) {
    ++candidate.skipped;
    retire();
    return no_memo;
  }
  if (program.memo_bound.empty()) {
    program.memo_bound.assign(p.body.slots.size(), 0);
    program.memo_stepped.assign(p.body.slots.size(), 0);
    program.memo_cells.assign(s.workspace.size(), 0);
  }

  auto memo = std::make_unique<DataLoopMemo>();
  memo->key = key;
  DataLoopTrace trace;
  trace.memo = memo.get();
  trace.candidate = &candidate;
  trace.effects_start = s.effects;
  struct Cleanup {
    RecordingProgram& program;
    DataLoopTrace& trace;
    ~Cleanup() {
      for (int slot : trace.bound) program.memo_bound[slot] = 0;
      for (int slot : trace.stepped) program.memo_stepped[slot] = 0;
      for (uint32_t cell : trace.cells) program.memo_cells[cell] = 0;
    }
  } cleanup{program, trace};
  ++candidate.misses;
  const size_t next = dispatch<true>(program, pc, candidate.exit, &trace);
  if (next != candidate.exit || memo->events.size() > kMemoMaxEvents ||
      memo->pool.size() > kMemoMaxPool)
    trace.ok = false;
  if (trace.executed < program.memo_min_work) {
    candidate.disabled = true;
    trace.ok = false;
  }
  if (trace.executed < program.memo_min_ratio * (memo->events.size() + 1))
    trace.ok = false;
  if (trace.ok) {
    for (int slot : trace.bound)
      if (program.memo_bound[slot] == 1 && candidate.live_out[slot]) {
        const double* ptr = value(slot);
        const int64_t length = p.body.slots[slot].len;
        memo->finals.push_back({DataLoopMemo::Slot, slot,
                                static_cast<uint32_t>(memo->pool.size()),
                                static_cast<uint32_t>(length)});
        memo->pool.insert(memo->pool.end(), ptr, ptr + length);
      }
    for (const Node* n : trace.written) {
      const int slot =
          n->kind == Node::For ? n->iterator : p.body.ops[n->op].out;
      if (!candidate.live_out[slot]) continue;
      const int64_t length = p.body.slots[slot].len;
      memo->finals.push_back({DataLoopMemo::Cells,
                              static_cast<int32_t>(n->workspace),
                              static_cast<uint32_t>(memo->pool.size()),
                              static_cast<uint32_t>(length)});
      memo->pool.insert(memo->pool.end(), s.workspace.data() + n->workspace,
                        s.workspace.data() + n->workspace + length);
    }
    memo->skipped_effects =
        (s.effects - trace.effects_start) - trace.step_effects;
    memo->usable = true;
  } else {
    ++candidate.aborted;
    memo->events.clear();
    memo->pool.clear();
  }
  candidate.pool_doubles += memo->pool.size();
  ++candidate.stored;
  candidate.memos[hash].push_back(std::move(memo));
  retire();
  return next;
}

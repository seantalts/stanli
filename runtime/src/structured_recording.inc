// A temporary recording program, built only after frame admission. Explicit
// jumps replace the control-tree walk; proven data calls bypass recording
// bookkeeping while preserving every kernel, validation and control guard.
struct RecordingProgram {
  enum Kind {
    Leaf,
    Kernel,
    Alias,
    Target,
    DataKernel,
    DataIndex,
    DataBranchFresh,
    DataBranch,
    Branch,
    ForEnter,
    ForNext,
    DataForNext,
    WhileEnter,
    WhileTest,
    Jump,
    ExitJump
  };
  struct Instruction {
    Kind kind;
    const Node* node;
    // Jump destination for control; successful publication for data calls.
    size_t jump = 0;
  };
  struct Cursor {
    double lower = 0;
    int64_t count = 0, iteration = 0;
    bool constant = false;
  };
  struct Exits {
    std::vector<size_t> breaks, continues;
  };
  static constexpr size_t exit_break = std::numeric_limits<size_t>::max();
  static constexpr size_t exit_continue = exit_break - 1;
  void compile_data(LoopState& s);
  std::vector<Instruction> code;
  std::vector<Cursor> cursors;

  size_t emit(Kind kind, const Node* node = nullptr, size_t jump = 0) {
    code.push_back({kind, node, jump});
    return code.size() - 1;
  }
  void patch(const std::vector<size_t>& sites, size_t target) {
    for (size_t site : sites) code[site].jump = target;
  }
  void lower(const Node& n, Exits& exits) {
    switch (n.kind) {
      case Node::Sequence:
        for (const auto& child : n.children) lower(child, exits);
        break;
      case Node::If: {
        const size_t branch = emit(Branch, &n);
        lower(n.children[0], exits);
        const size_t done = emit(Jump);
        code[branch].jump = code.size();
        lower(n.children[1], exits);
        code[done].jump = code.size();
        break;
      }
      case Node::For: {
        const size_t start = emit(ForEnter, &n);
        Exits body;
        lower(n.children[0], body);
        const size_t next = emit(ForNext, &n, start + 1);
        code[start].jump = code.size();
        patch(body.breaks, code.size());
        patch(body.continues, next);
        break;
      }
      case Node::While: {
        emit(WhileEnter, &n);
        const size_t condition = code.size();
        Exits condition_exits;
        lower(n.children[0], condition_exits);
        const size_t test = emit(WhileTest, &n);
        Exits body;
        lower(n.children[1], body);
        emit(Jump, nullptr, condition);
        code[test].jump = code.size();
        patch(condition_exits.breaks, code.size());
        // The tree evaluator consumes continue in a while condition, then
        // reads its condition cell. It does not restart that condition.
        patch(condition_exits.continues, test);
        patch(body.breaks, code.size());
        patch(body.continues, condition);
        break;
      }
      case Node::Break:
        exits.breaks.push_back(emit(ExitJump));
        break;
      case Node::Continue:
        exits.continues.push_back(emit(ExitJump));
        break;
      case Node::KernelCall:
        emit(Kernel, &n);
        break;
      case Node::Alias:
        emit(Alias, &n);
        break;
      case Node::Target:
        emit(Target, &n);
        break;
      default:
        emit(Leaf, &n);
        break;
    }
  }
  RecordingProgram(const Node& root, LoopState& s) : cursors(s.p.loop_count) {
    Exits outer;
    lower(root, outer);
    patch(outer.breaks, exit_break);
    patch(outer.continues, exit_continue);
    const char* option = std::getenv("STANLI_STRUCTURED_DATA_RECORDING");
    if (!option || option[0] != '0') compile_data(s);
  }
};

void RecordingProgram::compile_data(LoopState& s) {
  const auto& p = s.p;
  if (!p.segments.empty()) return;
  // Constancy here is conditional on the recorder's existing guards. No
  // control flow or kernel is removed, reordered, or hoisted. This is not
  // loop invariance or proof that parameter-controlled assignments commute.
  std::vector<uint8_t> varying(p.body.slots.size(), 0);
  std::vector<uint8_t> mutable_output(p.body.slots.size(), 0);
  for (const auto& in : p.imports)
    if (!in.data_only) varying[in.slot] = 1;
  const auto inspect = [&](const auto& self, const Node& n) -> void {
    if (n.kind == Node::Alias) mutable_output[n.dst] = 1;
    if (n.kind == Node::For) mutable_output[n.iterator] = 1;
    if (n.kind == Node::KernelCall && n.storage == Node::InPlace)
      mutable_output[p.body.ops[n.op].in[0]] = 1;
    for (const auto& child : n.children) self(self, child);
  };
  inspect(inspect, p.root);
  bool changed = true;
  const auto mark = [&](int slot) {
    if (slot >= 0 && !varying[slot]) varying[slot] = changed = true;
  };
  const auto propagate = [&](const auto& self, const Node& n) -> void {
    if (n.kind == Node::Alias && varying[n.src]) mark(n.dst);
    if (n.kind == Node::For && (varying[n.lower] || varying[n.upper]))
      mark(n.iterator);
    if (n.kind == Node::KernelCall) {
      const auto& op = p.body.ops[n.op];
      const auto* kernel = find_kernel(op.opcode);
      bool any =
          is_effectful_op(op.opcode) || !kernel || kernel->forward != n.forward;
      for (int k = 0; k < op.n_in; ++k) any |= varying[op.in[k]] != 0;
      if (any) {
        mark(op.out);
        mark(op.out2);
        if (n.storage == Node::InPlace) mark(op.in[0]);
      }
    }
    for (const auto& child : n.children) self(self, child);
  };
  while (changed) {
    changed = false;
    propagate(propagate, p.root);
  }
  const char* index_option = std::getenv("STANLI_STRUCTURED_PREPARED_INDEX");
  const bool prepare_indices = !index_option || index_option[0] != '0';
  const char* iterator_option =
      std::getenv("STANLI_STRUCTURED_PREPARED_ITERATOR");
  if (!iterator_option || iterator_option[0] != '0') {
    // A stable iterator binding requires one For writer in the whole region.
    // The original ForEnter publishes it; later iterations only change its
    // workspace value. Data-valued bounds remain guarded by enclosing control.
    std::vector<int> writer(p.body.slots.size(), -1);
    const auto mark_writer = [&](int slot, int loop) {
      if (slot >= 0) writer[slot] = writer[slot] == -1 ? loop : -2;
    };
    const auto inspect_writers = [&](const auto& self, const Node& n) -> void {
      if (n.kind == Node::For) mark_writer(n.iterator, n.loop_index);
      if (n.kind == Node::Alias) mark_writer(n.dst, -2);
      if (n.kind == Node::KernelCall) {
        const auto& op = p.body.ops[n.op];
        mark_writer(op.out, -2);
        mark_writer(op.out2, -2);
        if (n.storage == Node::InPlace) mark_writer(op.in[0], -2);
      }
      for (const auto& child : n.children) self(self, child);
    };
    inspect_writers(inspect_writers, p.root);
    for (auto& instruction : code) {
      if (instruction.kind != ForNext) continue;
      const auto& n = *instruction.node;
      if (n.storage == Node::Transient && writer[n.iterator] == n.loop_index &&
          !varying[n.lower] && !varying[n.upper])
        instruction.kind = DataForNext;
    }
  }
  size_t kernels = 0;
  for (auto& instruction : code) {
    if (instruction.kind != Kernel) continue;
    const Node& n = *instruction.node;
    const auto& op = p.body.ops[n.op];
    if (n.storage != Node::Transient || n.invariant_loop >= 0 || n.active ||
        op.out2 >= 0 || op.dyn_lengths || mutable_output[op.out] ||
        varying[op.out])
      continue;
    bool self_read = false;
    for (int k = 0; k < op.n_in; ++k) self_read |= op.in[k] == op.out;
    if (self_read) continue;
    // These stable workspace pointers belong only to this recording.
    // Frame replay binds its own call pointers before invoking a kernel.
    auto& context = s.ctx[n.site];
    context.out.data = s.workspace.data() + n.workspace;
    context.scratch = context.out.data + p.body.slots[op.out].len;
    instruction.kind = DataKernel;
    // A point-read instruction caches proof of fixed geometry, not values or
    // addresses. Eligibility is nonthrowing; the first executed call still
    // runs the original kernel, including every descriptor check.
    if (prepare_indices && n.forward == index_forward && op.udata &&
        p.body.slots[op.out].len == 1) {
      const auto& spec = *static_cast<const DynamicIndexSpec*>(op.udata);
      if (spec.selected_size == 1 &&
          std::all_of(spec.axes.begin(), spec.axes.end(), [](const auto& a) {
            return a.kind == DynamicIndexSpec::Axis::Single && a.count == 1 &&
                   a.count_input_offset < 0 && a.extent_input_offset < 0;
          }))
        instruction.kind = DataIndex;
    }
    ++kernels;
  }
  // Both instructions still execute in their original order. Keep the branch
  // in the program because another jump may enter it directly; skip its
  // dispatch only when arriving through this proven data-only producer.
  // A separate initial kind tracks publication without overloading the target.
  for (size_t i = 0; i + 1 < code.size(); ++i) {
    auto& instruction = code[i];
    const auto& branch = code[i + 1];
    if (instruction.kind == DataKernel &&
        (branch.kind == Branch || branch.kind == WhileTest) &&
        p.body.ops[instruction.node->op].out == branch.node->condition) {
      instruction.kind = DataBranchFresh;
      instruction.jump = branch.jump;
    }
  }
  if (s.diagnostics)
    emit_diagnostic("stanli_structured data_recording: kernels=" +
                    std::to_string(kernels));
}

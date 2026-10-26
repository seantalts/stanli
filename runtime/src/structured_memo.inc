// Static eligibility and key construction for memoized data loops.
void RecordingProgram::compile_memo(LoopState& s) {
  const auto& p = s.p;
  const size_t slot_count = p.body.slots.size();
  const size_t size = code.size();

  pure_candidate.assign(size, 0);
  for (size_t pc = 0; pc < size; ++pc) {
    const auto& instruction = code[pc];
    if (instruction.kind == Kernel) {
      const Node& n = *instruction.node;
      const auto& op = p.body.ops[n.op];
      const auto* kernel = find_kernel(op.opcode);
      bool ok = n.storage != Node::InPlace && !n.active &&
                !is_effectful_op(op.opcode) && kernel &&
                kernel->forward == n.forward && op.out2 < 0 &&
                !op.dyn_lengths &&
                !(n.invariant_loop >= 0 && n.storage != Node::Transient);
      for (int k = 0; ok && k < op.n_in; ++k) ok = !varying[op.in[k]];
      pure_candidate[pc] = ok;
    } else if (instruction.kind == Alias) {
      pure_candidate[pc] = !varying[instruction.node->src];
    }
  }

  std::vector<uint8_t> written(slot_count, 0);
  std::vector<uint32_t> reads(slot_count, 0);
  const auto scan = [&](const auto& self, const Node& n) -> void {
    switch (n.kind) {
      case Node::KernelCall: {
        const auto& op = p.body.ops[n.op];
        for (int k = 0; k < op.n_in; ++k) ++reads[op.in[k]];
        if (n.storage == Node::InPlace && op.n_in > 0) written[op.in[0]] = 1;
        written[op.out] = 1;
        if (op.out2 >= 0) written[op.out2] = 1;
        break;
      }
      case Node::Alias:
        ++reads[n.src];
        written[n.dst] = 1;
        break;
      case Node::Target:
        ++reads[n.src];
        break;
      case Node::If:
      case Node::While:
        ++reads[n.condition];
        break;
      case Node::For:
        ++reads[n.lower];
        ++reads[n.upper];
        written[n.iterator] = 1;
        break;
      default:
        break;
    }
    for (const auto& child : n.children) self(self, child);
  };
  scan(scan, p.root);
  for (int slot : p.outputs) ++reads[slot];

  const auto all_reads = [&](size_t pc, auto&& f) {
    const auto& instruction = code[pc];
    switch (instruction.kind) {
      case DataKernel:
      case DataIndex:
      case DataBranchFresh:
      case DataBranch:
      case Kernel: {
        const auto& op = p.body.ops[instruction.node->op];
        for (int k = 0; k < op.n_in; ++k) f(op.in[k]);
        break;
      }
      case Alias:
      case Target:
        f(instruction.node->src);
        break;
      case Branch:
      case WhileTest:
        f(instruction.node->condition);
        break;
      case ForEnter:
        f(instruction.node->lower);
        f(instruction.node->upper);
        break;
      default:
        break;
    }
  };
  const auto pure_reads = [&](size_t pc, auto&& f) {
    const auto kind = code[pc].kind;
    if (kind == Kernel && !pure_candidate[pc]) return;
    if (kind == Alias && !pure_candidate[pc]) return;
    if (kind == Target) return;
    all_reads(pc, f);
  };
  const auto pure_writes = [&](size_t pc, auto&& f) {
    const auto& instruction = code[pc];
    switch (instruction.kind) {
      case DataKernel:
      case DataIndex:
      case DataBranchFresh:
      case DataBranch:
        f(p.body.ops[instruction.node->op].out);
        break;
      case Kernel:
        if (pure_candidate[pc]) f(p.body.ops[instruction.node->op].out);
        break;
      case Alias:
        if (pure_candidate[pc]) f(instruction.node->dst);
        break;
      case ForEnter:
      case ForNext:
      case DataForNext:
        f(instruction.node->iterator);
        break;
      default:
        break;
    }
  };
  const auto step_writes = [&](size_t pc, auto&& f) {
    const auto& instruction = code[pc];
    if (instruction.kind == Kernel && !pure_candidate[pc]) {
      const Node& n = *instruction.node;
      const auto& op = p.body.ops[n.op];
      if (n.storage == Node::InPlace) f(op.in[0]);
      f(op.out);
      if (op.out2 >= 0) f(op.out2);
    } else if (instruction.kind == Alias && !pure_candidate[pc]) {
      f(instruction.node->dst);
    }
  };
  const auto successors = [&](size_t pc, auto&& f) {
    const auto& instruction = code[pc];
    switch (instruction.kind) {
      case Jump:
      case ExitJump:
        f(instruction.jump, false);
        break;
      case DataBranchFresh:
      case DataBranch:
        f(pc + 2, false);
        f(instruction.jump, false);
        break;
      case Branch:
      case WhileTest:
      case ForNext:
      case DataForNext:
        f(pc + 1, false);
        f(instruction.jump, false);
        break;
      case ForEnter:
        f(pc + 1, true);
        f(instruction.jump, false);
        break;
      default:
        f(pc + 1, false);
        break;
    }
  };

  struct Region {
    size_t begin, exit;
  };
  std::vector<Region> regions;
  for (size_t pc = 0; pc < size; ++pc) {
    if (code[pc].kind == ForEnter) {
      regions.push_back({pc, code[pc].jump});
    } else if (code[pc].kind == WhileEnter) {
      for (size_t t = pc + 1; t < size; ++t)
        if (code[t].kind == WhileTest && code[t].node == code[pc].node) {
          regions.push_back({pc, code[t].jump});
          break;
        }
    }
  }

  std::vector<uint8_t> wall(slot_count, 0), exposed(slot_count, 0),
      carried(slot_count, 0), covered(slot_count, 0);
  std::vector<int32_t> compact(slot_count, -1);
  std::vector<uint32_t> reads_inside(slot_count, 0);
  std::vector<std::vector<uint32_t>> incoming(size + 1);
  for (size_t pc = 0; pc < size; ++pc)
    successors(pc, [&](size_t to, bool) {
      if (to <= size) incoming[to].push_back(static_cast<uint32_t>(pc));
    });
  constexpr size_t max_region = 20000;
  constexpr size_t max_key_cells = 4096;
  constexpr size_t max_dataflow_words = size_t{1} << 22;
  for (const Region& region : regions) {
    const size_t b = region.begin, e = region.exit;
    if (e <= b || e > size || e - b > max_region) continue;
    bool refuse = false;
    for (size_t pc = b + 1; pc < e && !refuse; ++pc)
      for (uint32_t from : incoming[pc])
        if (from < b || from >= e) refuse = true;
    for (size_t pc = b; pc < e && !refuse; ++pc) {
      successors(pc, [&](size_t to, bool) {
        if (to < b || to > e) refuse = true;
      });
      const auto& instruction = code[pc];
      const Node* n = instruction.node;
      switch (instruction.kind) {
        case Leaf:
          refuse = true;
          break;
        case Branch:
        case WhileTest:
          refuse |= varying[n->condition] != 0;
          break;
        case ForEnter:
          refuse |= varying[n->lower] || varying[n->upper];
          break;
        default:
          break;
      }
    }
    if (refuse) continue;

    std::vector<int> pure_written, touched;
    const auto reset = [&]() {
      for (int slot : touched) {
        wall[slot] = exposed[slot] = carried[slot] = covered[slot] = 0;
        compact[slot] = -1;
        reads_inside[slot] = 0;
      }
    };
    for (size_t pc = b; pc < e; ++pc) {
      all_reads(pc, [&](int slot) { touched.push_back(slot); });
      pure_writes(pc, [&](int slot) {
        wall[slot] = 1;
        touched.push_back(slot);
        if (compact[slot] < 0) {
          compact[slot] = static_cast<int32_t>(pure_written.size());
          pure_written.push_back(slot);
        }
      });
      step_writes(pc, [&](int slot) {
        wall[slot] = 1;
        touched.push_back(slot);
      });
    }
    const size_t words = (pure_written.size() + 63) / 64;
    const size_t length = e - b;
    if (words * length > max_dataflow_words) {
      reset();
      continue;
    }

    std::vector<uint64_t> def_in(length * words, ~uint64_t{0});
    std::fill_n(def_in.begin(), words, uint64_t{0});
    std::vector<std::vector<std::pair<size_t, bool>>> preds(length);
    for (size_t pc = b; pc < e; ++pc)
      successors(pc, [&](size_t to, bool defines) {
        if (to < e && to > b) preds[to - b].push_back({pc, defines});
      });
    std::vector<uint64_t> out(words), merged(words);
    bool changed = true;
    while (changed) {
      changed = false;
      for (size_t pc = b + 1; pc < e; ++pc) {
        std::fill(merged.begin(), merged.end(), ~uint64_t{0});
        for (const auto& edge : preds[pc - b]) {
          const size_t from = edge.first;
          const bool defines = edge.second;
          std::copy_n(def_in.begin() + (from - b) * words, words, out.begin());
          pure_writes(from, [&](int slot) {
            const bool iterator = code[from].kind == ForEnter;
            if (iterator && !defines) return;
            const int32_t at = compact[slot];
            out[at / 64] |= uint64_t{1} << (at % 64);
          });
          for (size_t w = 0; w < words; ++w) merged[w] &= out[w];
        }
        uint64_t* row = def_in.data() + (pc - b) * words;
        if (!std::equal(merged.begin(), merged.end(), row)) {
          std::copy(merged.begin(), merged.end(), row);
          changed = true;
        }
      }
    }

    std::unordered_map<int, std::vector<size_t>> readers;
    for (size_t pc = b; pc < e; ++pc) {
      const uint64_t* row = def_in.data() + (pc - b) * words;
      const auto undefined = [&](int slot) {
        const int32_t at = compact[slot];
        return at < 0 || !((row[at / 64] >> (at % 64)) & 1);
      };
      all_reads(pc, [&](int slot) {
        if (undefined(slot)) carried[slot] = 1;
      });
      pure_reads(pc, [&](int slot) {
        readers[slot].push_back(pc);
        if (undefined(slot)) exposed[slot] = 1;
      });
    }

    std::vector<uint8_t> member(length, 0);
    for (size_t pc = b; pc < e; ++pc) {
      const auto kind = code[pc].kind;
      const bool kernel = kind == DataKernel || kind == DataIndex ||
                          kind == DataBranchFresh || kind == DataBranch ||
                          (kind == Kernel && pure_candidate[pc]);
      if (!kernel) continue;
      const auto& op = p.body.ops[code[pc].node->op];
      if (op.n_in < 1 || op.out2 >= 0 || op.dyn_lengths) continue;
      bool ok = true;
      for (int k = 0; k < op.n_in; ++k) ok = ok && !wall[op.in[k]];
      member[pc - b] = ok;
    }
    for (bool stable = false; !stable;) {
      stable = true;
      for (size_t pc = b; pc < e; ++pc) {
        if (!member[pc - b]) continue;
        const auto& op = p.body.ops[code[pc].node->op];
        for (int k = 0; k < op.n_in && member[pc - b]; ++k) {
          if (!written[op.in[k]]) continue;
          for (size_t reader : readers[op.in[k]])
            if (!member[reader - b]) {
              member[pc - b] = 0;
              stable = false;
              break;
            }
        }
      }
    }

    DataLoopCandidate candidate;
    candidate.begin = b;
    candidate.exit = e;
    for (size_t pc = b; pc < e; ++pc) {
      if (!member[pc - b]) continue;
      const Node& n = *code[pc].node;
      const auto& op = p.body.ops[n.op];
      bool mutable_input = false;
      for (int k = 0; k < op.n_in; ++k)
        if (written[op.in[k]]) {
          mutable_input = true;
          covered[op.in[k]] = 1;
        }
      if (mutable_input) candidate.invariants.push_back(&n);
    }
    size_t cells = 0;
    std::sort(touched.begin(), touched.end());
    touched.erase(std::unique(touched.begin(), touched.end()), touched.end());
    for (int slot : touched)
      if (exposed[slot] && written[slot] && !covered[slot]) {
        candidate.raw.push_back(slot);
        cells += static_cast<size_t>(p.body.slots[slot].len);
      }
    if (cells > max_key_cells) {
      reset();
      continue;
    }

    for (size_t pc = b; pc < e; ++pc)
      all_reads(pc, [&](int slot) { ++reads_inside[slot]; });
    candidate.live_out.assign(slot_count, 0);
    for (int slot : touched)
      candidate.live_out[slot] =
          reads[slot] > reads_inside[slot] || carried[slot];
    reset();

    for (size_t pc = b; pc < e; ++pc) {
      if (code[pc].kind != Kernel) continue;
      const int loop = code[pc].node->invariant_loop;
      if (loop < 0 ||
          std::find(candidate.bump_loops.begin(), candidate.bump_loops.end(),
                    loop) != candidate.bump_loops.end())
        continue;
      for (size_t q = b; q < e; ++q)
        if ((code[q].kind == ForEnter || code[q].kind == WhileEnter) &&
            code[q].node->loop_index == loop) {
          candidate.bump_loops.push_back(loop);
          break;
        }
    }
    candidates.push_back(std::move(candidate));
    code[b].candidate = static_cast<uint32_t>(candidates.size() - 1);
  }
  for (const auto& candidate : candidates)
    code[candidate.begin].kind =
        code[candidate.begin].kind == ForEnter ? MemoForEnter : MemoWhileEnter;
}

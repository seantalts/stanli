// Eligible forward-only loops use private slot values instead of a recorded
// execution trace. Alias
// is a snapshot copy, not a pointer assignment: later writes must not mutate a
// previously assigned Stan value. The plan admits only stateless body kernels
// (prepare_node rejects make_state and nested execution kernels).
struct ForwardLoopExecution {
  const StructuredLoop& p;
  LoopState& s;
  KernelCtx& outer;
  std::vector<double> values;
  std::vector<double> temporary;
  std::vector<double> invariant_values;
  std::vector<int64_t> invariant_offsets, node_generation, loop_generation;
  double target = 0.0;
  enum Flow { Normal, Break, Continue };

  ForwardLoopExecution(LoopState& state, KernelCtx& context)
      : p(state.p),
        s(state),
        outer(context),
        values(static_cast<size_t>(p.initial_size), 0.0),
        invariant_offsets(p.site_count, -1),
        node_generation(p.site_count, -1),
        loop_generation(p.loop_count, 0) {
    int64_t scratch = 0, cached = 0;
    for (const Node* n : s.sites) {
      const Op& op = p.body.ops[n->op];
      if (n->invariant_loop >= 0) {
        invariant_offsets[n->site] = cached;
        cached = add(cached, add(length(p, op.out), length(p, op.out2)));
      }
      if (n->storage == Node::InPlace) {
        scratch = std::max(scratch, length(p, update_rhs(op)));
        continue;
      }
      scratch = std::max(
          scratch,
          add(add(length(p, op.out), length(p, op.out2)), n->kernel_scratch));
    }
    // Keep even zero-length descriptors backed by a valid pointer.
    temporary.resize(static_cast<size_t>(std::max<int64_t>(scratch, 1)));
    // Kernel output slots are unique, so these snapshots use at most one
    // additional copy of the plan's slots, independently of executed trips.
    invariant_values.resize(static_cast<size_t>(std::max<int64_t>(cached, 1)));
    if (values.empty()) values.resize(1);
    for (const auto& fill : p.fills)
      std::copy(fill.second.begin(), fill.second.end(), value(fill.first));
    for (const auto& in : p.imports) {
      const int64_t len = length(p, in.slot);
      if (in.input >= outer.n_in || in.offset > outer.in[in.input].len ||
          len > outer.in[in.input].len - in.offset)
        throw std::logic_error("structured import exceeds graph input");
      std::copy_n(outer.in[in.input].data + in.offset, len, value(in.slot));
    }
  }

  double* value(int slot) { return values.data() + p.body.slots[slot].offset; }

  void kernel(const Node& n) {
    const Op& op = p.body.ops[n.op];
    if (n.invariant_loop >= 0 &&
        node_generation[n.site] == loop_generation[n.invariant_loop]) {
      const double* cached =
          invariant_values.data() + invariant_offsets[n.site];
      std::copy_n(cached, length(p, op.out), value(op.out));
      if (op.out2 >= 0)
        std::copy_n(cached + length(p, op.out), length(p, op.out2),
                    value(op.out2));
      return;
    }
    // A local copy also resets descriptor lengths/n_in after a preceding
    // dynamic call. Never change values_only: it selects numerical overloads.
    KernelCtx c = s.ctx[n.site];
    c.eval_state = outer.eval_state;
    c.n_in = op.n_in;
    for (int k = 0; k < op.n_in; ++k) {
      c.in[k] = Desc{value(op.in[k]), length(p, op.in[k])};
      c.in_adj[k] = Desc{nullptr, length(p, op.in[k])};
    }
    if (n.storage == Node::InPlace) {
      const auto& spec = *static_cast<const DynamicIndexSpec*>(op.udata);
      const IndexInputLayout layout = require_index_input_layout(spec, true);
      c.out = Desc{value(op.in[0]), length(p, op.in[0])};
      if (c.dyn_lengths) apply_dynamic_length(c);
      // Aliases already own snapshot copies. Only the selected RHS needs
      // staging; copying the full base for each scalar write would make a
      // matrix fill quadratic in its number of cells. index_positions validates
      // the complete selection before calling the writer, just as the retained
      // in-place path does. Duplicate selectors retain their source order.
      std::copy_n(c.in[layout.rhs].data, c.in[layout.rhs].len,
                  temporary.data());
      index_positions(spec, c, true, [&](int64_t i, int64_t at) {
        c.out.data[at] = temporary[static_cast<size_t>(i)];
      });
      return;
    }
    const int64_t out_len = length(p, op.out);
    const int64_t out2_len = length(p, op.out2);
    c.out = Desc{temporary.data(), out_len};
    c.out_adj_vec = Desc{nullptr, out_len};
    c.out2 =
        Desc{op.out2 >= 0 ? temporary.data() + out_len : nullptr, out2_len};
    c.scratch =
        n.kernel_scratch ? temporary.data() + out_len + out2_len : nullptr;
    if (c.dyn_lengths) apply_dynamic_length(c);
    // All inputs remain untouched until the complete callback succeeds,
    // including self-read updates, duplicate selectors and overlapping slices.
    n.forward(c);
    std::copy_n(c.out.data, c.out.len, value(op.out));
    if (op.out2 >= 0) std::copy_n(c.out2.data, c.out2.len, value(op.out2));
    if (n.invariant_loop >= 0) {
      double* cached = invariant_values.data() + invariant_offsets[n.site];
      std::copy_n(value(op.out), out_len, cached);
      if (op.out2 >= 0) std::copy_n(value(op.out2), out2_len, cached + out_len);
      node_generation[n.site] = loop_generation[n.invariant_loop];
    }
  }

  Flow run(const Node& n) {
    switch (n.kind) {
      case Node::Sequence:
        for (const auto& child : n.children) {
          const Flow flow = run(child);
          if (flow != Normal) return flow;
        }
        return Normal;
      case Node::KernelCall:
        kernel(n);
        return Normal;
      case Node::Alias:
        if (n.dst != n.src)
          std::copy_n(value(n.src), length(p, n.src), value(n.dst));
        return Normal;
      case Node::If:
        return run(n.children[value(n.condition)[0] != 0.0 ? 0 : 1]);
      case Node::For: {
        ++loop_generation[n.loop_index];
        const double lo = value(n.lower)[0], hi = value(n.upper)[0];
        if (!std::isfinite(lo) || !std::isfinite(hi) || std::trunc(lo) != lo ||
            std::trunc(hi) != hi || lo < std::numeric_limits<int32_t>::min() ||
            hi < std::numeric_limits<int32_t>::min() ||
            lo > std::numeric_limits<int32_t>::max() ||
            hi > std::numeric_limits<int32_t>::max())
          throw std::logic_error("structured loop invalid integer bounds");
        const int64_t count = hi >= lo ? static_cast<int64_t>(hi - lo) + 1 : 0;
        for (int64_t i = 0; i < count; ++i) {
          value(n.iterator)[0] = lo + static_cast<double>(i);
          if (run(n.children[0]) == Break) break;
        }
        return Normal;
      }
      case Node::While:
        ++loop_generation[n.loop_index];
        for (;;) {
          if (run(n.children[0]) == Break || value(n.condition)[0] == 0.0)
            break;
          if (run(n.children[1]) == Break) break;
        }
        return Normal;
      case Node::Break:
        return Break;
      case Node::Continue:
        return Continue;
      case Node::Target:
        // reduce_target uses exactly this source-order scalar addition.
        target += value(n.src)[0];
        return Normal;
    }
    throw std::logic_error("invalid structured node");
  }

  void forward() {
    int64_t expected = p.has_target ? 1 : 0;
    for (int slot : p.outputs) expected = add(expected, length(p, slot));
    if (expected != outer.out.len)
      throw std::logic_error("structured output size mismatch");
    run(p.root);
    int64_t pos = 0;
    for (int slot : p.outputs) {
      std::copy_n(value(slot), length(p, slot), outer.out.data + pos);
      pos += length(p, slot);
    }
    if (p.has_target) outer.out.data[pos] = target;
  }
};

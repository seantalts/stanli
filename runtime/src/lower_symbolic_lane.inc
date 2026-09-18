// Included inside lower_detail. Prove the whole source family before ordinary
// typed lowering supplies a scalar seed; refusals retain that ordinary work.
namespace {
struct SymbolicLaneProof {
  Lowering& lo;
  const mir::Stmt& loop;
  int64_t first, last;
  enum class Kind { Bad, Parameter, Read, Chain };
  struct Local {
    Kind value = Kind::Bad;
    bool consumed = false;
  };
  std::map<std::string, Local> locals;
  int remaining = 128;
  int reads = 0, anchors = 0;
  uint16_t anchor_opcode = OP_NONE_;
  int anchor_arity = 0;
  bool target = false;
  int64_t offset = 0;

  std::optional<int64_t> index_offset(const mir::Expr& e) {
    if (e.kind == mir::Expr::Var && e.name == loop.loopvar) return 0;
    if (e.kind != mir::Expr::FunApp || e.fn_lib != mir::Expr::Lib::StanLib ||
        e.args.size() != 2 || e.args[1].kind != mir::Expr::LitInt)
      return std::nullopt;
    // A single fixed shift only; checked in the range of graph immediates.
    if (e.args[0].kind != mir::Expr::Var || e.args[0].name != loop.loopvar)
      return std::nullopt;
    const int64_t n = e.args[1].lit_i;
    if (n < 0 || n > INT_MAX) return std::nullopt;
    if (e.name == "Plus__" || e.name == "add") return n;
    if (e.name == "Minus__" || e.name == "subtract") return -n;
    return std::nullopt;
  }

  Kind expression(const mir::Expr& e) {
    if (--remaining < 0 || e.type_ != "UReal") return Kind::Bad;
    if (e.kind == mir::Expr::Var) {
      auto local = locals.find(e.name);
      if (local != locals.end()) {
        if (local->second.consumed) return Kind::Bad;
        local->second.consumed = true;
        return local->second.value;
      }
      auto v = lo.scope.find(e.name);
      return v != lo.scope.end() && v->second.autodiff &&
                     lo.g.slots[v->second.slot].is_param &&
                     lo.g.slots[v->second.slot].len == 1
                 ? Kind::Parameter
                 : Kind::Bad;
    }
    if (e.kind == mir::Expr::Indexed && e.args.size() == 2 &&
        e.args[0].kind == mir::Expr::Var && !locals.count(e.args[0].name) &&
        e.args[1].name == "IndexSingle" && e.args[1].args.size() == 1) {
      if (++reads != 1 || lo.scope.count(e.args[0].name)) return Kind::Bad;
      const auto delta = index_offset(e.args[1].args[0]);
      const auto view = lo.try_static_view(e.args[0]);
      const auto* value = lo.td.find(e.args[0].name);
      if (!delta || view.state != Lowering::StaticProbeState::Known || !value ||
          value->is_int)
        return Kind::Bad;
      const auto& si = view.value.si;
      const bool flat_real =
          is_vector(si) || is_row_vector(si) ||
          (is_array(si) && lo.array_shape(si).dims.size() == 1 &&
           lo.array_shape(si).leaf == ViewKind::Flat &&
           e.args[0].unsized.leaf == mir::UnsizedLeaf::Real);
      if (!flat_real || first + *delta < 1 || last + *delta > view.value.len ||
          view.value.len != static_cast<int64_t>(value->r.size()))
        return Kind::Bad;
      offset = first + *delta - 1;
      return Kind::Read;
    }
    if (e.kind != mir::Expr::FunApp || e.fn_lib != mir::Expr::Lib::StanLib ||
        e.fn_propto)
      return Kind::Bad;
    // Recognize the registered family, then require the ordinary typed seed
    // below to produce the corresponding scalar operation. A Product descriptor
    // is only scalar multiplication here because every argument must be UReal.
    uint16_t opcode = OP_NONE_;
    if (e.name == "fma" && e.args.size() == 3) opcode = OP_FMA;
    if (e.args.size() == 2) {
      const auto* spec = function_spec(e);
      const auto* builtin = spec ? spec->builtin() : nullptr;
      if (builtin && builtin->shape == BuiltinShapePolicy::Product)
        opcode = OP_MUL;
      else if (builtin && builtin->shape == BuiltinShapePolicy::Elementwise &&
               (builtin->opcode == OP_ADD || builtin->opcode == OP_MUL))
        opcode = builtin->opcode;
    }
    if (opcode != OP_NONE_) {
      if (++anchors != 1) return Kind::Bad;
      anchor_opcode = opcode;
      anchor_arity = static_cast<int>(e.args.size());
      int params = 0, varying = 0;
      for (const auto& arg : e.args) {
        const Kind kind = expression(arg);
        params += kind == Kind::Parameter;
        varying += kind == Kind::Read;
      }
      return params == anchor_arity - 1 && varying == 1 ? Kind::Chain
                                                        : Kind::Bad;
    }
    if ((e.name == "exp" || e.name == "square") && e.args.size() == 1 &&
        expression(e.args[0]) == Kind::Chain)
      return Kind::Chain;
    return Kind::Bad;
  }

  bool statement(const mir::Stmt& s) {
    if (--remaining < 0) return false;
    if (s.kind == mir::Stmt::Skip) return true;
    if (s.kind == mir::Stmt::Block || s.kind == mir::Stmt::SList) {
      for (const auto& child : s.body)
        if (!statement(child)) return false;
      return true;
    }
    if (target) return false;
    if (s.kind == mir::Stmt::Decl) {
      if (s.decl_type.base != "SReal" || s.read_transform ||
          s.decl_id == loop.loopvar || locals.count(s.decl_id) ||
          lo.scope.count(s.decl_id) || lo.decls.count(s.decl_id) ||
          lo.int_locals.count(s.decl_id) || lo.int_env.count(s.decl_id) ||
          lo.td.find(s.decl_id))
        return false;
      Local local;
      if (s.has_init) {
        local.value = expression(s.init);
        if (local.value == Kind::Bad) return false;
      }
      locals.emplace(s.decl_id, local);
      return true;
    }
    if (s.kind == mir::Stmt::Assignment) {
      auto local = locals.find(s.lhs);
      if (!s.lhs_idx.empty() || local == locals.end() ||
          local->second.value != Kind::Bad || local->second.consumed)
        return false;
      local->second.value = expression(s.rhs);
      return local->second.value != Kind::Bad;
    }
    if (s.kind == mir::Stmt::TargetPE) {
      target = expression(s.target) == Kind::Chain;
      return target;
    }
    return false;
  }
};

struct SymbolicLaneSource {
  // The proof below permits exactly one varying hashed field: the data-read
  // identity in the first FMA/ADD/MUL. Other operands are fixed parameter IDs
  // or lane-local sentinel -1, with no idata. XOR and odd multiplication are
  // bijections modulo 2^64, so fresh checked identities give L distinct hashes.
  // Broader source families must re-prove this property or use actual hashing.
  static constexpr bool kDistinctLaneHashes = true;
  std::vector<Op> lane;
  int first_fresh = 0, stride = 0;
  Op at(int position, int64_t l) const {
    Op op = lane[position];
    for (int j = 0; j < op.n_in; ++j)
      if (op.in[j] >= first_fresh) op.in[j] += static_cast<int>(l * stride);
    op.out += static_cast<int>(l * stride);
    return op;
  }
  void replace_terms(int, int64_t, int term, std::vector<int>& targets,
                     std::unordered_set<int>& terms) const {
    // The proof admits only this terminal region and no preceding targets.
    assert(targets.empty() && terms.empty());
    targets.push_back(term);
    terms.insert(term);
  }
};
}  // namespace

bool Lowering::try_lower_symbolic_lane_tail(const mir::Stmt& s, int64_t first,
                                            int64_t last) {
  namespace rp = detail::reroll_plan;
  if (in_write_array || region_current || udf_depth || target_scale != 1.0 ||
      first < 1 || last > INT_MAX || last - first + 1 < detail::kMinLanes ||
      !g.ops.empty() || !target_terms.empty() || !jac_slots.empty() ||
      !extra_roots.empty() || !out.fills.empty() || !const_cache.empty() ||
      std::getenv("STANLI_NO_REROLL") || std::getenv("STANLI_NO_CONSTFOLD") ||
      structured_policy == StructuredMode::Prefer ||
      structured_policy == StructuredMode::Force)
    return false;
  for (const auto& slot : g.slots)
    if (!slot.is_param || slot.len != 1) return false;
  SymbolicLaneProof proof{*this, s, first, last};
  for (const auto& child : s.body)
    if (!proof.statement(child)) return false;
  if (!proof.target || proof.reads != 1 || proof.anchors != 1) return false;
  for (const auto& local : proof.locals)
    if (!local.second.consumed) return false;

  // This is ordinary lowering, not a speculative compiler/environment copy.
  // The proof excludes effects and control, so a typed or pricing refusal can
  // retain iteration one and lower the remainder without replaying any work.
  const size_t parameter_slots = g.slots.size();
  int_env[s.loopvar] = first;
  for (const auto& child : s.body) lower_stmt(child);
  const auto continue_ordinary = [&] {
    for (int64_t lane = first + 1; lane <= last; ++lane) {
      int_env[s.loopvar] = lane;
      for (const auto& child : s.body) lower_stmt(child);
    }
    int_env.erase(s.loopvar);
    return true;
  };
  if (g.ops.size() < 2 || g.ops.size() > detail::kMaxPeriod + 1 ||
      target_terms.size() != 1 || !jac_slots.empty())
    return continue_ordinary();
  const Op read = g.ops.front();
  if (read.opcode != OP_INDEX || read.n_in != 1 || read.n_idata != 1 ||
      read.idata[0] != proof.offset || g.slots[read.out].len != 1)
    return continue_ordinary();
  const int P = static_cast<int>(g.ops.size()) - 1;
  const int64_t lanes = last - first + 1;
  // Preserve the ordinary fresh-slot identity arithmetic without allocating
  // any of its later slots. One INDEX plus P active outputs per iteration.
  if (g.slots.size() != static_cast<size_t>(read.out + P + 1) ||
      lanes > (INT_MAX - read.out) / (P + 1))
    return continue_ordinary();
  std::vector<double> values;
  for (const auto& fill : out.fills)
    if (fill.first == read.in[0]) {
      if (proof.offset + lanes > static_cast<int64_t>(fill.second.size()))
        return continue_ordinary();
      values.assign(fill.second.begin() + proof.offset,
                    fill.second.begin() + proof.offset + lanes);
    }
  if (static_cast<int64_t>(values.size()) != lanes) return continue_ordinary();

  rp::CandidatePlan plan;
  plan.lanes = lanes;
  plan.positions.resize(P);
  int previous = read.out;
  for (int p = 0; p < P; ++p) {
    const Op& op = g.ops[p + 1];
    if (op.opcode != (p == 0 ? proof.anchor_opcode : OP_EXPV) &&
        !(p > 0 && op.opcode == OP_SQUARE))
      return continue_ordinary();
    if (!has_op_trait(op.opcode, op_trait::kRerollWidenable) ||
        op.n_in != (p == 0 ? proof.anchor_arity : 1) ||
        op.out != read.out + p + 1 || op.out2 >= 0 || op.n_idata || op.udata ||
        op.dyn_lengths || op.dyn_extent_in >= 0 || g.slots[op.out].len != 1)
      return continue_ordinary();
    auto& pos = plan.positions[p];
    pos.ins.resize(op.n_in);
    int chain_reads = 0;
    for (int j = 0; j < op.n_in; ++j) {
      auto& in = pos.ins[j];
      if (op.in[j] == previous) {
        ++chain_reads;
        if (p == 0) {
          in.kind = rp::InKind::kConstLanes;
          in.values = std::move(values);
        } else {
          in.kind = rp::InKind::kLaneLocal;
          in.producer_pos = p - 1;
        }
      } else {
        if (p != 0 || op.in[j] >= read.out || !g.slots[op.in[j]].is_param)
          return continue_ordinary();
        in.kind = rp::InKind::kInvariant;
      }
    }
    if (chain_reads != 1) return continue_ordinary();
    previous = op.out;
  }
  if (target_terms[0] != previous) return continue_ordinary();
  plan.positions.back().term_widen = true;
  // The proven INDEX is an exact slice of the existing fill. Its scalar
  // result is dead after widening; do not execute a speculative constant-fold
  // pass (which would materialize its full backing array) just to discard it.
  SymbolicLaneSource source{{g.ops.begin() + 1, g.ops.end()}, read.out, P + 1};
  auto selected = rp::SelectedPlan<SymbolicLaneSource>::select(g, plan, source);
  if (!selected) return continue_ordinary();
  std::vector<Op> result;
  std::vector<int> targets;
  std::unordered_set<int> terms;
  std::unordered_map<int, int> renamed;
  std::move(*selected).emit(g, out.fills, targets, terms, result, renamed);
  // Selection commits to this representation. An internal emitter invariant
  // failure must fail compilation, never continue from a partly mutated graph.
  for (const auto& op : result) {
    if (op.out < 0 || static_cast<size_t>(op.out) >= g.slots.size())
      fail("invalid symbolic lane output", s.raw);
    for (int j = 0; j < op.n_in; ++j)
      if (op.in[j] < 0 || static_cast<size_t>(op.in[j]) >= g.slots.size())
        fail("invalid symbolic lane input", s.raw);
  }
  for (size_t slot = parameter_slots; slot < g.slots.size(); ++slot)
    if (g.slots[slot].is_param) fail("invalid symbolic lane parameter", s.raw);
  g.ops = std::move(result);
  target_terms = std::move(targets);
  // Nothing declared inside the terminal loop escapes. Drop seed bindings and
  // observations (including the full source-array observation) just as the old
  // isolated trial did. Keep the original data environment and emitted fills.
  for (const auto& local : proof.locals) {
    scope.erase(local.first);
    decls.erase(local.first);
    td.env().erase(local.first);
  }
  for (auto it = scope.begin(); it != scope.end();) {
    if (it->second.slot >= static_cast<int>(parameter_slots))
      it = scope.erase(it);
    else
      ++it;
  }
  for (auto it = observations.begin(); it != observations.end();) {
    if (it->first.slot >= static_cast<int>(parameter_slots))
      it = observations.erase(it);
    else
      ++it;
  }
  int_env.erase(s.loopvar);
  return true;
}

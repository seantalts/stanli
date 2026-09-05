#include "lower_internal.hpp"

namespace stanli {
namespace lower_detail {

bool Lowering::expr_has_jacobian(const mir::Expr& e) {
  if (e.kind == mir::Expr::FunApp) {
    CallableTransformSpec transform;
    if (callable_transform(e.name, &transform) &&
        transform.direction == TransformDirection::Jacobian)
      return true;
    // Stan permits Jacobian adjustments in a UDF precisely when its name
    // has this suffix. Conservatively carry a target through such a call;
    // an unused zero is cheaper than dropping a nested adjustment.
    if (e.fn_lib == mir::Expr::Lib::UserDefined &&
        transform_suffix(e.name, "_jacobian"))
      return true;
  }
  for (const auto& a : e.args)
    if (expr_has_jacobian(a)) return true;
  return false;
}
// Does `s` increment the target, explicitly or through a Jacobian call?
bool Lowering::has_target_pe(const mir::Stmt& s) {
  if (s.kind == mir::Stmt::TargetPE) return true;
  if ((s.has_init && expr_has_jacobian(s.init)) || expr_has_jacobian(s.rhs) ||
      expr_has_jacobian(s.target) || expr_has_jacobian(s.lower) ||
      expr_has_jacobian(s.upper) || expr_has_jacobian(s.cond))
    return true;
  for (const auto& e : s.fn_args)
    if (expr_has_jacobian(e)) return true;
  for (const auto& e : s.lhs_idx)
    if (expr_has_jacobian(e)) return true;
  for (const auto& k : s.body)
    if (has_target_pe(k)) return true;
  return false;
}
bool Lowering::needs_runtime_control(const mir::Stmt& s) {
  // A structured while owns every runtime decision in its body.  Promoting
  // its enclosing block would absorb UDF-local declarations and returns,
  // which are not live-outs of that outer region.
  if (s.kind == mir::Stmt::While) return false;
  if (s.kind == mir::Stmt::Block || s.kind == mir::Stmt::SList) {
    // This scan runs before the block is lowered, but loop bounds later in
    // the block can depend on scalar-int locals established by earlier
    // statements.  Mirror just that compile-time environment in statement
    // order.  In particular, stanc spells `int d = rows(x)` as a default
    // declaration followed by an assignment, and UDFs commonly use d to
    // size locals and loops.  Looking through the whole block without this
    // lexical state rejects an otherwise static write-array UDF.
    const auto saved = int_env;
    std::set<std::string> local_ints;
    bool found = false;
    try {
      for (const auto& child : s.body) {
        if (needs_runtime_control(child)) {
          found = true;
          break;
        }
        if (child.kind == mir::Stmt::Decl && child.decl_type.base == "SInt") {
          local_ints.insert(child.decl_id);
          int_env.erase(child.decl_id);
          if (child.has_init) int_env[child.decl_id] = eval_int(child.init);
        } else if (child.kind == mir::Stmt::Assignment &&
                   child.lhs_idx.empty() && local_ints.count(child.lhs)) {
          int_env[child.lhs] = eval_int(child.rhs);
        }
      }
    } catch (...) {
      int_env = saved;
      throw;
    }
    int_env = saved;
    return found;
  }
  if (s.kind == mir::Stmt::IfElse) {
    // This is a speculative write_array scan, so follow an already-known
    // arm exactly as ordinary lowering will.  Besides avoiding needless
    // work, this preserves Stan's reachability semantics for invalid shape
    // selectors in a dead statement arm.
    if (auto evaluated = try_eval_pure(s.cond)) {
      const size_t arm = evaluated->r.at(0) != 0.0 ? 0 : 1;
      return arm < s.body.size() && needs_runtime_control(s.body[arm]);
    }
    if (s.cond.data_only) return true;
  }
  if (s.kind == mir::Stmt::For) {
    const long lo = eval_int(s.lower), hi = eval_int(s.upper);
    if (lo > hi) return false;
    const auto old = int_env.find(s.loopvar);
    const bool had_old = old != int_env.end();
    const long old_value = had_old ? old->second : 0;
    bool found = false;
    // Scan under the same compile-time loop bindings ordinary lowering
    // will use. This keeps static conditions such as `if (t < N)` out of
    // a region without overlooking an arm that exists only at a later t.
    for (long v = lo; v <= hi && !found; ++v) {
      int_env[s.loopvar] = v;
      for (const auto& k : s.body)
        if (needs_runtime_control(k)) {
          found = true;
          break;
        }
    }
    if (had_old)
      int_env[s.loopvar] = old_value;
    else
      int_env.erase(s.loopvar);
    return found;
  }
  for (const auto& k : s.body)
    if (needs_runtime_control(k)) return true;
  return false;
}
// A Break/Continue selected by a runtime condition cannot be lowered as a
// standalone conditional island: its jump target belongs to the enclosing
// loop. Promote that whole loop to the necessity island instead. Nested
// loops own their own control statements and therefore stop this search.
bool Lowering::runtime_loop_control(const mir::Stmt& s, bool runtime_path) {
  if (s.kind == mir::Stmt::Break || s.kind == mir::Stmt::Continue)
    return runtime_path;
  if (s.kind == mir::Stmt::For || s.kind == mir::Stmt::While) return false;
  if (s.kind == mir::Stmt::IfElse) {
    if (auto evaluated = try_eval_pure(s.cond)) {
      const bool take_then = evaluated->r.at(0) != 0.0;
      if (take_then && !s.body.empty())
        return runtime_loop_control(s.body[0], runtime_path);
      if (!take_then && s.body.size() > 1)
        return runtime_loop_control(s.body[1], runtime_path);
      return false;
    }
    for (const auto& arm : s.body)
      if (runtime_loop_control(arm, true)) return true;
    return false;
  }
  for (const auto& child : s.body)
    if (runtime_loop_control(child, runtime_path)) return true;
  return false;
}
// Remove a return at the lexical end of a statement arm, preserving every
// statement that precedes it.  This is the structured form used by UDFs
// such as ctsem's mcalc: each arm returns, but one arm first updates a local
// matrix.  The updates can lower as an ordinary statement island and the
// two returned expressions can then join through a ternary value island.
bool Lowering::peel_terminal_return(mir::Stmt* s, mir::Expr* value) {
  if (s->kind == mir::Stmt::Return) {
    if (!s->has_init) return false;
    *value = s->rhs;
    s->kind = mir::Stmt::Skip;
    s->body.clear();
    return true;
  }
  if ((s->kind == mir::Stmt::Block || s->kind == mir::Stmt::SList) &&
      !s->body.empty())
    return peel_terminal_return(&s->body.back(), value);
  return false;
}
// Compile `s` (a statement region) or `e` (a ternary) into a program.
void Lowering::lower_island(const mir::Stmt* s, const mir::Expr* e,
                            IslandRegion* reg, Range* expr_out,
                            std::shared_ptr<IslandProg>* prog_out) {
  auto prog = std::make_shared<IslandProg>();
  ProgramCompiler c{*prog, fun_defs};
  c.in_write_array = in_write_array;
  // Non-returning statement calls may print or reject. A register program
  // would replay them during reverse mode, so ProgramCompiler refuses them
  // until necessity islands have an execute-once effect path.
  for (const auto& [name, v] : int_env) c.ints[name] = {v};
  // Data the region reads as a compile-time integer, answered by the
  // same interpreter that answers a size expression. The region has
  // already resolved the indices, so what arrives is a literal read of a
  // data-only value -- nothing here depends on the region's own scope.
  c.extern_int = [&](const mir::Expr& x, long* out) {
    if (!x.data_only) return false;
    auto evaluated = try_eval_pure(x);
    if (!evaluated || !evaluated->is_int || evaluated->i.size() != 1)
      return false;
    *out = evaluated->i[0];
    return true;
  };
  c.extern_ints = [&](const mir::Expr& x, std::vector<long>* values,
                      std::vector<int64_t>* dims) {
    if (!x.data_only || x.unsized.depth == 0 ||
        x.unsized.leaf != mir::UnsizedLeaf::Int)
      return false;
    auto evaluated = try_eval_pure(x);
    if (!evaluated || !evaluated->is_int ||
        evaluated->i.size() != evaluated->r.size())
      return false;
    values->assign(evaluated->i.begin(), evaluated->i.end());
    *dims = evaluated->dims;
    return true;
  };
  c.extern_real = [&](const mir::Expr& x, double* value) {
    if (x.type_ != "UReal") return false;
    auto evaluated = try_eval_pure(x);
    if (!evaluated || evaluated->is_int || evaluated->r.size() != 1)
      return false;
    *value = evaluated->r[0];
    return true;
  };
  c.lower_higher_order = [&](const mir::Expr& x, Range* result) {
    return lower_program_higher_order(c, x, result);
  };
  if (!in_write_array) {
    c.bind_target = [&](Range* r) {
      const int slot = current_target_slot();
      r->reg = c.alloc(1);
      r->len = 1;
      prog->ins.push_back(IslandProg::LiveIn{r->reg, 1});
      reg->in_slots.push_back(slot);
      return true;
    };
  }
  std::set<std::string> outer_names;
  for (const auto& [name, value] : scope) outer_names.insert(name);
  for (const auto& [name, value] : decls) outer_names.insert(name);
  const std::set<std::string> outer_int_names = int_locals;
  c.bind_extern = [&](const std::string& name, Range* r) {
    auto sc = scope.find(name);
    int slot = sc != scope.end() ? sc->second.slot : env_slot(name);
    if (slot < 0) slot = uninitialized_decl_slot(name);
    if (slot < 0) return false;
    const int64_t len = g.slots[slot].len;
    r->reg = c.alloc((int)len);
    r->len = (int)len;
    const SlotInfo& si = scope.at(name).si;
    r->rows = si.rows;
    r->cols = si.cols;
    r->kind = si.kind;
    if (is_array(si)) {
      const ArrayShape& arr = array_shape(si);
      r->dims = arr.dims;
      r->leaf = arr.leaf;
    }
    if (len > 0) {
      prog->ins.push_back(IslandProg::LiveIn{r->reg, (int)len});
      reg->in_slots.push_back(slot);
    }
    return true;
  };
  // `target +=` inside the region accumulates into a register of its
  // own, seeded to zero, and the total leaves as one more live-out that
  // lowering registers as a target term. A `~` statement cannot go here
  // (its dropped constants depend on argument types the program binds
  // uniformly), and stanc lowers `~` to TargetPE with the propto form
  // already chosen, so the compiler refuses what it cannot reproduce.
  int target_reg = -1;
  if (s) {
    target_reg = c.alloc(1);
    const double zero = 0.0;
    c.emit_const(target_reg, &zero, 1);
    c.target_reg = target_reg;
  }
  try {
    if (s) {
      // A local declared before the region but never assigned has no
      // slot yet (lowering makes one on first assignment), so there is
      // no outside value to read: the region declares it itself. Stan
      // initializes a local to NaN, and an arm that does not assign it
      // has to leave it that way.
      std::vector<std::string> pre;
      assigned_names(*s, &pre);
      for (const std::string& name : pre) {
        if (scope.count(name) || int_locals.count(name)) continue;
        auto dl = decls.find(name);
        if (dl == decls.end()) continue;
        Range view;
        view.rows = dl->second.si.rows;
        view.cols = dl->second.si.cols;
        view.kind = dl->second.si.kind;
        if (is_array(dl->second.si)) {
          const ArrayShape& arr = array_shape(dl->second.si);
          view.dims = arr.dims;
          view.leaf = arr.leaf;
        }
        const double fill =
            dl->second.int_array
                ? static_cast<double>(std::numeric_limits<int>::min())
                : std::numeric_limits<double>::quiet_NaN();
        c.declare(name, (int)dl->second.len, view, fill);
      }
      c.stmt(*s);
      std::vector<std::string> assigned;
      assigned_names(*s, &assigned);
      for (const std::string& name : assigned) {
        const bool is_outer_int = outer_int_names.count(name) != 0;
        if (!outer_names.count(name) && !is_outer_int) continue;
        auto it = c.reals.find(name);
        if (it == c.reals.end()) continue;
        reg->out_names.push_back(name);
        reg->out_is_int.push_back(is_outer_int);
        reg->out_views.push_back(it->second);
        for (int k = 0; k < it->second.len; ++k)
          prog->out_regs.push_back(it->second.reg + k);
      }
      if (has_target_pe(*s)) {
        reg->has_target = true;
        prog->out_regs.push_back(target_reg);
      }
      // An integer the region folded is one this lowering holds a copy
      // of, and the copy is a compile-time constant every later size,
      // index and read would keep using. The region compiler folds only
      // what certainly happens, so the value it ends with is the one
      // every path through the region leaves behind. Nothing carries an
      // integer out of the program itself: a live-out is a register, and
      // registers hold doubles.
      for (const std::string& name : assigned) {
        auto folded = c.ints.find(name);
        auto held = int_env.find(name);
        if (folded != c.ints.end() && folded->second.size() == 1 &&
            held != int_env.end())
          held->second = folded->second[0];
      }
    } else {
      *expr_out = c.expr(*e);
      for (int k = 0; k < expr_out->len; ++k)
        prog->out_regs.push_back(expr_out->reg + k);
    }
    c.finish();
  } catch (Bail& b) {
    fail("runtime-control region: " + b.why, s ? s->raw : e->raw);
  }
  // No live-out register is legitimate when the region found live-outs
  // and every one of them is zero-width: the data made the values empty,
  // as `matrix[0, 0]` from a dimension table does, so there is nothing
  // for the program to carry out. Finding no live-out at all is the
  // mistake this catches -- a region that lost what it was to produce --
  // unless the region's entire purpose was a conditional effect: that has
  // no data output by design, its value being the output or exception.
  reg->has_effect = island_has_effect(*prog);
  if (prog->out_regs.empty() && !(e && expr_out->len == 0) &&
      (s == nullptr || reg->out_names.empty()) && !reg->has_effect)
    fail("runtime-control region produces nothing", s ? s->raw : e->raw);
  // A region with a runtime branch keeps the var replay -- reversing
  // control flow needs the structured form the flat program has already
  // lost -- so this usually declines. It is asked anyway because a region
  // can reach here branch-free: a `~` refusal or an unknown name is not
  // the only way to end up compiled.
  // The register compactor's liveness analysis is straight-line (with
  // forward branches as barriers).  A while adds a back edge, so retaining
  // the uncompact program is the correctness-first choice: a state register
  // written in one iteration is necessarily live at the next head.
  bool has_back_edge = false;
  bool has_unmodelled_ranges = false;
  for (size_t pc = 0; pc < prog->code.size(); ++pc) {
    const Program::Instr& instr = prog->code[pc];
    if (program_code_spec(instr.code).has(kProgramNoAdjoint))
      has_unmodelled_ranges = true;
    if ((instr.code == Program::JZ || instr.code == Program::JMP) &&
        instr.dst <= static_cast<int>(pc)) {
      has_back_edge = true;
    }
  }
  // The straight-line compactor derives every range width from Instr::len.
  // Structured matrix calls use that field for the result width while
  // their operands can have different widths, so retain the original
  // register numbering until those instructions carry explicit spans.
  if (!has_back_edge && !has_unmodelled_ranges) compact_island(*prog);
  prog->native_adj = gen_adjoint(*prog) && !std::getenv("STANLI_NO_NATIVE_ADJ");
  *prog_out = std::move(prog);
}
// The OP_ISLAND for a compiled region, plus one extraction per live-out.
void Lowering::emit_island(const std::shared_ptr<IslandProg>& prog,
                           const IslandRegion& reg,
                           const std::vector<int>& out_lens,
                           std::vector<int>* out_slots) {
  int64_t packed = 0;
  for (int len : out_lens) packed += len;
  Op is;
  is.opcode = OP_ISLAND;
  // Variant stays zero: kIslandSoftmax3Variant is a tagged-payload contract
  // and may only accompany Softmax3IslandProg (the graph carver creates it).
  std::vector<int> inputs = reg.in_slots;
  if (inputs.size() <= 6) {
    for (size_t k = 0; k < prog->ins.size(); ++k) {
      prog->ins[k].input = (int)k;
      prog->ins[k].offset = 0;
    }
  } else {
    // Op::in is deliberately compact. Pack just enough leading live-ins
    // to leave five ordinary descriptors; the program's LiveIn records
    // retain the individual register ranges and point into the packed one.
    const size_t packed_count = inputs.size() - 5;
    int packed = inputs[0];
    int64_t packed_len = g.slots[packed].len;
    for (size_t k = 1; k < packed_count; ++k) {
      packed_len += g.slots[inputs[k]].len;
      packed = emit_raw(OP_CONCAT2, {packed, inputs[k]}, packed_len, {}).slot;
    }
    int offset = 0;
    for (size_t k = 0; k < packed_count; ++k) {
      prog->ins[k].input = 0;
      prog->ins[k].offset = offset;
      offset += prog->ins[k].len;
    }
    std::vector<int> compact{packed};
    for (size_t k = packed_count; k < inputs.size(); ++k) {
      prog->ins[k].input = (int)compact.size();
      prog->ins[k].offset = 0;
      compact.push_back(inputs[k]);
    }
    inputs = std::move(compact);
  }
  is.n_in = (int)inputs.size();
  for (int k = 0; k < is.n_in; ++k) is.in[k] = inputs[k];
  is.out = add_slot(packed, false);
  is.udata = prog.get();
  g.udata_pool.push_back(prog);
  g.ops.push_back(is);
  int64_t off = 0;
  for (size_t k = 0; k < out_lens.size(); ++k) {
    const int len = out_lens[k];
    const Val v =
        emit_raw(len == 1 ? OP_INDEX : OP_SLICE, {is.out}, len, {}, {(int)off});
    out_slots->push_back(v.slot);
    off += len;
  }
}
// `if (<not known while building the graph>) ... else ...`
void Lowering::lower_runtime_ifelse(const mir::Stmt& s) {
  IslandRegion reg;
  std::shared_ptr<IslandProg> prog;
  Range ignored;
  lower_island(&s, nullptr, &reg, &ignored, &prog);
  // Widths come from the region compiler's own registers: they are what
  // out_regs packs, and they already reflect a zero-length sentinel
  // declaration the region's assignment sized.
  std::vector<int> out_lens;
  for (const Range& v : reg.out_views) out_lens.push_back(v.len);
  if (reg.has_target) out_lens.push_back(1);
  // Nothing to carry out and no target to accumulate: every live-out is
  // zero-width, so the region has no observable effect and its values
  // keep the empty shape they already have outside. A `target +=` would
  // have put its own register here, so this cannot drop one -- and a
  // print()/reject() have no live-out by design, so it cannot either.
  if (prog->out_regs.empty() && !reg.has_effect) return;
  std::vector<int> out_slots;
  emit_island(prog, reg, out_lens, &out_slots);
  // Later statements read the island's results, not the old values.
  for (size_t k = 0; k < reg.out_names.size(); ++k) {
    const std::string& name = reg.out_names[k];
    SlotInfo si;
    if (reg.out_is_int[k]) {
      // This local was an SInt before the loop.  Its loop-carried value is
      // now a register-program result; retain the UInt type but make it a
      // graph-local runtime value so later branches and scalar reads use
      // the value the loop actually produced.
      si = view_of("UInt");
      si.param_free = false;
      scope[name] = Val{out_slots[k], false, si};
      decls[name] = DeclView{1, false, si};
      int_env.erase(name);
      int_locals.erase(name);
      td.env().erase(name);
      continue;
    }
    bool shaped_outside = false;
    auto old = scope.find(name);
    if (old != scope.end()) {
      si = old->second.si;
      shaped_outside = g.slots[old->second.slot].len != 0;
    } else {
      auto dl = decls.find(name);
      if (dl != decls.end()) {
        si = dl->second.si;
        shaped_outside = dl->second.len != 0;
      }
    }
    if (!shaped_outside) {
      // The outside declaration was the inliner's zero-length sentinel;
      // the region's registers carry the real shape.
      si = SlotInfo{};
      si.rows = reg.out_views[k].rows;
      si.cols = reg.out_views[k].cols;
      si.kind = reg.out_views[k].kind;
      auto dl = decls.find(name);
      if (dl != decls.end()) {
        dl->second.len = reg.out_views[k].len;
        dl->second.si = si;
      }
    }
    // Runtime regions conservatively return parameter-dependent live-outs;
    // treating one as data without a per-output dependency proof would
    // select kernels that deliberately omit adjoints for that input.
    si.param_free = false;
    scope[name] = Val{out_slots[k], scalar_autodiff(), si};
  }
  if (reg.has_target) push_target_term(out_slots.back());
}
// `<not known while building the graph> ? a : b`
Lowering::Val Lowering::lower_runtime_ternary(const mir::Expr& e) {
  IslandRegion reg;
  std::shared_ptr<IslandProg> prog;
  Range value;
  lower_island(nullptr, &e, &reg, &value, &prog);
  std::vector<int> out_slots;
  emit_island(prog, reg, {value.len}, &out_slots);
  SlotInfo si;
  si.rows = value.rows;
  si.cols = value.cols;
  si.kind = value.kind;
  return {out_slots[0], scalar_autodiff(), si};
}
// Use the runtime-control compiler as a graph producer for higher-order
// families whose shared implementation already lives there. This keeps a
// straight-line graph call and a call under dynamic control on one callback
// binder and one kernel path instead of growing a second graph-only parser.
Lowering::Val Lowering::lower_program_expression(const mir::Expr& e) {
  IslandRegion reg;
  std::shared_ptr<IslandProg> prog;
  Range value;
  lower_island(nullptr, &e, &reg, &value, &prog);
  std::vector<int> out_slots;
  emit_island(prog, reg, {value.len}, &out_slots);
  SlotInfo si;
  if (value.kind == ViewKind::Array)
    si = array_view(value.dims, value.leaf, e.data_only);
  else {
    si = view_of(e.type_);
    si.rows = value.rows;
    si.cols = value.cols;
    si.kind = value.kind;
    si.param_free = e.data_only;
  }
  return {out_slots[0], expression_autodiff(e), si};
}
bool Lowering::stmt_effectful(const mir::Stmt& s) {
  if (s.kind == mir::Stmt::NRFunApp && message_action(s.fn_name)) return true;
  for (const auto& e : s.fn_args)
    if (expr_effectful(e)) return true;
  if (s.has_init && expr_effectful(s.init)) return true;
  if (expr_effectful(s.rhs) || expr_effectful(s.target) ||
      expr_effectful(s.lower) || expr_effectful(s.upper) ||
      expr_effectful(s.cond))
    return true;
  for (const auto& e : s.lhs_idx)
    if (expr_effectful(e)) return true;
  for (const auto& k : s.body)
    if (stmt_effectful(k)) return true;
  return false;
}
// Repeating an expression fewer times is observable for more than RNGs:
// target() reads the accumulator, compiler-internal calls may validate or
// emit, and the callback families can hide effects in another function.
// Admit the ordinary Stan-library expression grammar and explicitly keep
// those effect-capable seams out. User functions are refused wholesale;
// proving a UDF repeatable needs its own interprocedural effect summary.
bool Lowering::repeatable_target_expr(const mir::Expr& e,
                                      const std::string& loopvar) {
  if (e.kind == mir::Expr::Unsupported || expr_references(e, loopvar))
    return false;
  if (e.kind == mir::Expr::FunApp) {
    if (e.fn_lib != mir::Expr::Lib::StanLib) return false;
    const std::string& name = e.name;
    const bool rng =
        name.size() >= 4 && name.compare(name.size() - 4, 4, "_rng") == 0;
    if (rng || mir::higher_order_call(e) || mir::stateful_intrinsic_kind(e))
      return false;
  }
  for (const auto& a : e.args)
    if (!repeatable_target_expr(a, loopvar)) return false;
  return true;
}
// Conservative statement whitelist for a loop whose only externally
// visible effect is adding iterator-independent terms to target. Locals
// declared under the loop may be initialized and updated; any assignment
// to a name from the enclosing scope refuses the rewrite.
bool Lowering::repeatable_target_stmt(const mir::Stmt& s,
                                      const std::string& loopvar,
                                      const std::set<std::string>& locals,
                                      bool* has_target) {
  const auto expression_ok = [&](const mir::Expr& e) {
    return repeatable_target_expr(e, loopvar);
  };
  switch (s.kind) {
    case mir::Stmt::Block:
    case mir::Stmt::SList:
      for (const auto& child : s.body)
        if (!repeatable_target_stmt(child, loopvar, locals, has_target))
          return false;
      return true;
    case mir::Stmt::TargetPE:
      if (!expression_ok(s.target)) return false;
      *has_target = true;
      return true;
    case mir::Stmt::Decl:
      if (s.read_transform) return false;
      for (const auto& dim : s.decl_type.dims)
        if (!expression_ok(dim)) return false;
      return !s.has_init || expression_ok(s.init);
    case mir::Stmt::Assignment:
      if (!locals.count(s.lhs) || !expression_ok(s.rhs)) return false;
      for (const auto& index : s.lhs_idx)
        if (!expression_ok(index)) return false;
      return true;
    case mir::Stmt::For:
      if (!expression_ok(s.lower) || !expression_ok(s.upper)) return false;
      for (const auto& child : s.body)
        if (!repeatable_target_stmt(child, loopvar, locals, has_target))
          return false;
      return true;
    case mir::Stmt::IfElse:
      if (!expression_ok(s.cond)) return false;
      for (const auto& arm : s.body)
        if (!repeatable_target_stmt(arm, loopvar, locals, has_target))
          return false;
      return true;
    case mir::Stmt::Skip:
      return true;
    default:
      // Checks, print/reject, while/control transfer, returns, and new
      // statement kinds all keep the ordinary per-iteration path.
      return false;
  }
}
// Availability is independent of both MIR's AD type and param_free. A
// data-only loop result lives in a slot without a compile-time observation.
// This probe does not lower or execute anything (in particular, no UDF loop).
bool Lowering::needs_runtime_value(const mir::Expr& e) {
  if (is_shape_query(e) &&
      try_static_shape_query(e).state == StaticProbeState::Known)
    return false;
  if (e.kind == mir::Expr::Var) {
    const auto v = scope.find(e.name);
    return v != scope.end() && !observation(v->second) &&
           (!v->second.si.param_free ||
            (!int_env.count(e.name) && !td.find(e.name)));
  }
  if (expr_effectful(e)) return true;
  for (const auto& arg : e.args)
    if (needs_runtime_value(arg)) return true;
  return false;
}
bool Lowering::runtime_int_value(const mir::Expr& e) const {
  if (e.type_ != "UInt" || e.unsized.leaf != mir::UnsizedLeaf::Int ||
      e.unsized.depth != 0)
    return false;
  if (e.kind == mir::Expr::Var) {
    auto it = scope.find(e.name);
    return it != scope.end() && !it->second.si.param_free;
  }
  if (e.kind == mir::Expr::Indexed && !e.args.empty() &&
      e.args[0].kind == mir::Expr::Var) {
    auto it = scope.find(e.args[0].name);
    return it != scope.end() && !it->second.si.param_free;
  }
  return false;
}
Lowering::Val Lowering::lower_runtime_int_sum(const mir::Expr& e,
                                              CallArguments& actuals) {
  if (!in_write_array)
    fail("runtime integer sum is supported only in generated quantities",
         e.raw);
  if (!is_int_sum_surface(e))
    fail(
        "runtime integer sum needs one one-dimensional int-array argument "
        "and a scalar int result",
        e.raw);

  actuals.require_arity(1);
  Val a = actuals.at(0).value();
  if (!is_array(a.si))
    fail("runtime integer sum argument is not an array", e.raw);
  const ArrayShape& shape = array_shape(a.si);
  const int64_t len = g.slots[a.slot].len;
  if (shape.leaf != ViewKind::Flat || shape.dims.size() != 1)
    fail("runtime integer sum needs a one-dimensional int array", e.raw);
  if (len <= 0) fail("runtime integer sum needs a nonempty int array", e.raw);
  if (a.si.param_free)
    fail("runtime integer sum needs a runtime-produced int array", e.raw);

  const auto initialized = int_initialized_prefix.find(a.slot);
  if (initialized == int_initialized_prefix.end() || initialized->second != len)
    fail("runtime integer sum array is not definitely initialized", e.raw);
  const auto known = int_ranges.find(a.slot);
  if (known == int_ranges.end())
    fail("runtime integer sum has unproved integral slot values", e.raw);
  const IntRange range = known->second;
  const uint64_t n = static_cast<uint64_t>(len);
  if (range.lo < 0) {
    const uint64_t magnitude =
        static_cast<uint64_t>(-static_cast<int64_t>(range.lo));
    const uint64_t capacity = static_cast<uint64_t>(
        -static_cast<int64_t>(std::numeric_limits<int32_t>::min()));
    if (n > capacity / magnitude)
      fail("runtime integer sum may overflow int32 in a partial sum", e.raw);
  }
  if (range.hi > 0 &&
      n > static_cast<uint64_t>(std::numeric_limits<int32_t>::max()) /
              static_cast<uint64_t>(range.hi))
    fail("runtime integer sum may overflow int32 in a partial sum", e.raw);

  Val result = with_layout(emit_value(OP_SUM_VEC, {a}, 1, view_of("UInt")),
                           ExpressionLayout::scalar());
  result.autodiff = false;
  // A range is only a static proof; the source itself was required to be
  // runtime-produced.  Keeping this result non-constant prevents later
  // compile-time geometry/control from consuming it through Val metadata.
  result.si.param_free = false;
  set_int_range(result, static_cast<int64_t>(range.lo) * len,
                static_cast<int64_t>(range.hi) * len);
  return result;
}
void Lowering::lower_stmt_impl(const mir::Stmt& s) {
  switch (s.kind) {
    case mir::Stmt::Decl:
      // --O1 reuses one symbol id for block-local declarations at the same
      // source position. A preceding scalar-int declaration may therefore
      // leave a folded binding under the id subsequently assigned to an
      // array or real container. The fresh declaration shadows every
      // representation of that old scalar before its initializer is read.
      if (s.decl_type.base != "SInt") {
        int_env.erase(s.decl_id);
        int_locals.erase(s.decl_id);
      }
      if (s.read_transform) {
        lower_read_param(s);
      } else if (s.decl_type.base == "SInt") {
        if (s.has_init && in_write_array && runtime_int_binding(s.init)) {
          bind_runtime_int(s.decl_id, s.init, s.raw);
          return;
        }
        // A fresh scalar-int declaration shadows every representation of
        // an earlier declaration with the same optimized MIR id.  In
        // particular, a preceding runtime sum may have installed a graph
        // value in scope/decls; leaving it there would make a later Var
        // read win over the compile-time literal installed below.
        scope.erase(s.decl_id);
        decls.erase(s.decl_id);
        td.env().erase(s.decl_id);
        int_env.erase(s.decl_id);
        int_locals.erase(s.decl_id);
        // Only compile-time integers belong in int_env. MIR's DataOnly
        // AD level alone does not make a parameter-selected integer known.
        int_locals.insert(s.decl_id);
        // eval_int, not the interpreter directly: the initializer may be
        // a shape query on a slot-bound value (rows(lscale) inside an
        // inlined function), which only eval_int can answer.
        if (s.has_init) {
          if (auto value = static_int(s.init))
            int_env[s.decl_id] = *value;
          else
            bind_runtime_int(s.decl_id, s.init, s.raw);
        }
      } else if (s.decl_type.base.empty() &&
                 s.decl_type.unsized.leaf != mir::UnsizedLeaf::Unknown) {
        // O1 introduces unsized container temporaries for expressions such
        // as a for-loop sequence built with append_array. C++ assignment
        // gives these locals the RHS shape, so delay allocating or checking
        // their view until the first whole-variable assignment does likewise.
        scope.erase(s.decl_id);
        DeclView sh;
        sh.autodiff = s.decl_type.unsized.leaf != mir::UnsizedLeaf::Int &&
                      !s.decl_data_only && scalar_autodiff();
        sh.int_array = s.decl_type.unsized.leaf == mir::UnsizedLeaf::Int;
        sh.deferred_shape = true;
        if (s.has_init) {
          Val v = lower_expr(s.init);
          sh.len = g.slots[v.slot].len;
          sh.si = v.si;
          sh.deferred_shape = false;
          v.autodiff = sh.autodiff;
          v.layout = owning_layout(v.si);
          scope[s.decl_id] = v;
          sync_data_local(s.decl_id, s.init, v);
        } else {
          td.env().erase(s.decl_id);
        }
        decls[s.decl_id] = sh;
      } else {
        // A redeclaration shadows whatever the name held: --O1 inlining
        // reuses one symbol for a callee's local across loop iterations,
        // and its size can differ per iteration. The stale binding must
        // not constrain the fresh variable's width.
        scope.erase(s.decl_id);
        DeclView sh;
        sh.len = sized_len(s.decl_type);
        sh.autodiff = !s.decl_data_only && scalar_autodiff();
        sh.si = view_of(s.decl_type);
        // CmdStan fills every uninitialized integer container with the
        // INT_MIN sentinel.  Runtime-sum provenance remains deliberately
        // one-dimensional, but the value-level initialization contract is
        // independent of rank.
        sh.int_array =
            s.decl_type.base == "SArray" && s.decl_type.elem_base == "SInt";
        if (s.has_init) {
          Val v = lower_expr(s.init);
          SlotInfo expected = view_of(s.decl_type, v.si.param_free);
          require_binding(v, sh.len, expected, s.decl_id, s.raw);
          v.autodiff = sh.autodiff;
          v.si = expected;
          v.layout = owning_layout(v.si);
          scope[s.decl_id] = v;
        }
        decls[s.decl_id] = sh;
        if (s.has_init)
          sync_data_local(s.decl_id, s.init, scope.at(s.decl_id));
        else
          td.env().erase(s.decl_id);
      }
      return;
    case mir::Stmt::Assignment: {
      if (s.lhs_idx.empty() && int_locals.count(s.lhs)) {
        if (in_write_array && runtime_int_binding(s.rhs)) {
          bind_runtime_int(s.lhs, s.rhs, s.raw);
          return;
        }
        if (auto value = static_int(s.rhs))
          int_env[s.lhs] = *value;
        else
          bind_runtime_int(s.lhs, s.rhs, s.raw);
        return;
      }
      if (!s.lhs_idx.empty()) {
        // Element write under unrolled control flow: functional update.
        Val prev_v{-1, false, {}};
        auto it = scope.find(s.lhs);
        if (it != scope.end()) {
          prev_v = it->second;
        } else {
          auto dl = decls.find(s.lhs);
          if (dl == decls.end())
            fail("indexed assignment to undeclared " + s.lhs);
          SlotInfo si = dl->second.si;
          si.param_free = true;
          prev_v = Val{add_slot(dl->second.len, false), dl->second.autodiff, si,
                       owning_layout(si)};
          const double initial =
              dl->second.int_array
                  ? static_cast<double>(std::numeric_limits<int>::min())
                  : std::numeric_limits<double>::quiet_NaN();
          out.fills.emplace_back(prev_v.slot,
                                 std::vector<double>(dl->second.len, initial));
          if (dl->second.int_array) set_uninitialized_int_array(prev_v);
          observe_fill(prev_v, dl->second.int_array, initial, dl->second.len);
        }
        const int prev = prev_v.slot;
        bool all_single = true;
        for (const auto& ix : s.lhs_idx)
          if (ix.name != "IndexSingle") all_single = false;
        const std::vector<int64_t>* dd =
            is_array(prev_v.si) ? &array_shape(prev_v.si).dims : nullptr;
        const Val rhs_v = lower_expr(s.rhs);
        if (std::any_of(
                s.lhs_idx.begin(), s.lhs_idx.end(),
                [&](const mir::Expr& ix) { return runtime_selector(ix); })) {
          Val nv = region_index(prev_v, s.lhs_idx, s.rhs.type_, s.rhs.unsized,
                                &rhs_v);
          nv.autodiff = prev_v.autodiff;
          scope[s.lhs] = nv;
          sync_indexed_data_local(s.lhs, nv);
          return;
        }
        observe_indexed_rhs(s.rhs, rhs_v);
        const int rhs = rhs_v.slot;
        SlotInfo out_si = prev_v.si;
        // A one-index All spans the complete logical value. Keep this as
        // an indexed functional update rather than silently rewriting the
        // MIR statement: the ordinary binding checks still enforce width
        // and logical view, while the store path preserves integer-array
        // initialization and observation metadata. Matrix `[:, j]` is a
        // separate two-index form below and never enters this branch.
        if (s.lhs_idx.size() == 1 && s.lhs_idx[0].name == "IndexAll") {
          if (is_scalar(prev_v))
            fail("full-span assignment needs a container for " + s.lhs, s.raw);
          require_binding(rhs_v, g.slots[prev].len, prev_v.si, s.lhs, s.raw);
          Val nv = with_layout(emit_value(OP_SET_SLICE, {prev_v, rhs_v},
                                          g.slots[prev].len, out_si, {0}),
                               owning_layout(out_si));
          propagate_int_update(nv, prev_v, rhs_v, 0, 1);
          scope[s.lhs] = nv;
          sync_indexed_data_local(s.lhs, nv);
          return;
        }
        // Whole matrix row write M[i] = row_vector: one value per column,
        // strided by the physical row count.
        if (s.lhs_idx.size() == 1 && s.lhs_idx[0].name == "IndexSingle" &&
            is_matrix(prev_v.si) && is_row_vector(rhs_v.si)) {
          const int64_t i = eval_int(s.lhs_idx[0].args[0]) - 1;
          if (i < 0 || i >= prev_v.si.rows)
            fail("row assignment index out of bounds for " + s.lhs);
          if (g.slots[rhs].len != prev_v.si.cols)
            fail("row assignment size mismatch for " + s.lhs);
          Val nv = with_layout(emit_value(OP_SET_SLICE_STRIDED, {prev_v, rhs_v},
                                          g.slots[prev].len, out_si,
                                          {(int)i, (int)prev_v.si.rows}),
                               owning_layout(out_si));
          propagate_int_update(nv, prev_v, rhs_v, i, prev_v.si.rows);
          scope[s.lhs] = nv;
          sync_indexed_data_local(s.lhs, nv);
          return;
        }
        // Whole vector leaf write A[i, :] = rhs for array[N] vector[S].
        // Graph array storage keeps each outer element contiguous, so this
        // is the assignment mirror of the read path above.
        if (s.lhs_idx.size() == 2 && s.lhs_idx[0].name == "IndexSingle" &&
            s.lhs_idx[1].name == "IndexAll" && dd && dd->size() == 2 &&
            (array_shape(prev_v.si).leaf == ViewKind::Vector ||
             array_shape(prev_v.si).leaf == ViewKind::RowVector)) {
          const int64_t i = eval_int(s.lhs_idx[0].args[0]);
          const int64_t width = (*dd)[1];
          check_index(i, (*dd)[0], "array assignment index", s.raw);
          SlotInfo expected = indexed_view(prev_v.si, 1, width, s.rhs.type_);
          require_binding(rhs_v, width, expected, s.lhs, s.raw);
          const int64_t start = (i - 1) * width;
          Val nv =
              with_layout(emit_value(OP_SET_SLICE, {prev_v, rhs_v},
                                     g.slots[prev].len, out_si, {(int)start}),
                          owning_layout(out_si));
          propagate_int_update(nv, prev_v, rhs_v, start, 1);
          scope[s.lhs] = nv;
          sync_indexed_data_local(s.lhs, nv);
          return;
        }
        // Between write w[a:b] = rhs (contiguous on 1-D values). Deeper
        // bases fall through to the shared index geometry below.
        const bool flat_1d_array =
            is_array(prev_v.si) && array_shape(prev_v.si).dims.size() == 1 &&
            array_shape(prev_v.si).leaf == ViewKind::Flat;
        const bool one_dimensional =
            is_vector(prev_v.si) || is_row_vector(prev_v.si) || flat_1d_array;
        if (s.lhs_idx.size() == 1 && is_range(s.lhs_idx[0]) &&
            one_dimensional) {
          const StaticRange range =
              *static_range(s.lhs_idx[0], g.slots[prev].len);
          const int64_t lo = range.lo;
          const int64_t hi = range.hi;
          const int64_t len = hi >= lo ? hi - lo + 1 : 0;
          check_range(lo, hi, g.slots[prev].len, "range assignment", s.raw);
          if (g.slots[rhs].len != len)
            fail("range assignment size mismatch for " + s.lhs);
          const int64_t start = len == 0 ? 0 : lo - 1;
          Val nv =
              with_layout(emit_value(OP_SET_SLICE, {prev_v, rhs_v},
                                     g.slots[prev].len, out_si, {(int)start}),
                          owning_layout(out_si));
          propagate_int_update(nv, prev_v, rhs_v, start, 1);
          scope[s.lhs] = nv;
          sync_indexed_data_local(s.lhs, nv);
          return;
        }
        // Scatter write x[idx] = rhs on a 1-D value: the indices are data,
        // so spell it as one element write each; repeats then resolve
        // last-wins as CmdStan. A gather over a deeper base selects whole
        // outer elements and resolves through the index geometry below.
        if (s.lhs_idx.size() == 1 && s.lhs_idx[0].name == "IndexMulti" &&
            one_dimensional) {
          DataMap::Entry iv =
              eval_pure(s.lhs_idx[0].args[0], "a scatter index");
          if (!iv.is_int) fail("scatter index must be int data", s.raw);
          if ((int64_t)iv.i.size() != g.slots[rhs].len)
            fail("scatter assignment size mismatch for " + s.lhs);
          Val nv = prev_v;
          for (size_t k = 0; k < iv.i.size(); ++k) {
            check_index(iv.i[k], g.slots[prev].len, "scatter index", s.raw);
            const Val el =
                emit_value(OP_INDEX, {rhs_v}, 1, view_of("UReal"), {(int)k});
            const Val next =
                emit_value(OP_SET_INDEX, {nv, el}, g.slots[prev].len, out_si,
                           {(int)(iv.i[k] - 1)});
            propagate_int_update(next, nv, el, iv.i[k] - 1, 1);
            nv = next;
          }
          scope[s.lhs] = nv;
          sync_indexed_data_local(s.lhs, nv);
          return;
        }
        // Column write M[:, j] = rhs (contiguous in col-major storage).
        if (s.lhs_idx.size() == 2 && s.lhs_idx[0].name == "IndexAll" &&
            s.lhs_idx[1].name == "IndexSingle" && is_matrix(prev_v.si)) {
          const int64_t j = eval_int(s.lhs_idx[1].args[0]) - 1;
          if (j < 0 || j >= prev_v.si.cols)
            fail("column assignment index out of bounds for " + s.lhs);
          if (g.slots[rhs].len != prev_v.si.rows)
            fail("column assignment size mismatch for " + s.lhs);
          Val nv = emit_value(OP_SET_SLICE, {prev_v, rhs_v}, g.slots[prev].len,
                              out_si, {(int)(j * prev_v.si.rows)});
          propagate_int_update(nv, prev_v, rhs_v, j * prev_v.si.rows, 1);
          scope[s.lhs] = nv;
          sync_indexed_data_local(s.lhs, nv);
          return;
        }
        // Row-range column write M[a:b, j] = rhs (contiguous within the
        // column).
        if (s.lhs_idx.size() == 2 && is_range(s.lhs_idx[0]) &&
            s.lhs_idx[1].name == "IndexSingle" && is_matrix(prev_v.si)) {
          const StaticRange range = *static_range(s.lhs_idx[0], prev_v.si.rows);
          const int64_t lo = range.lo;
          const int64_t hi = range.hi;
          const int64_t j = eval_int(s.lhs_idx[1].args[0]) - 1;
          if (j < 0 || j >= prev_v.si.cols)
            fail("column assignment index out of bounds for " + s.lhs);
          const int64_t len = hi >= lo ? hi - lo + 1 : 0;
          check_range(lo, hi, prev_v.si.rows, "row-range assignment", s.raw);
          if (g.slots[rhs].len != len)
            fail("range assignment size mismatch for " + s.lhs);
          const int64_t start = len == 0 ? 0 : j * prev_v.si.rows + lo - 1;
          Val nv =
              with_layout(emit_value(OP_SET_SLICE, {prev_v, rhs_v},
                                     g.slots[prev].len, out_si, {(int)start}),
                          owning_layout(out_si));
          propagate_int_update(nv, prev_v, rhs_v, start, 1);
          scope[s.lhs] = nv;
          sync_indexed_data_local(s.lhs, nv);
          return;
        }
        // Columns outermost, as CmdStan's assign walks them: a repeated
        // index has to resolve last-wins in the same order.
        if (!all_single && s.lhs_idx.size() == 2 && is_matrix(prev_v.si)) {
          const std::vector<int64_t> ri = index_positions(
              s.lhs_idx[0], prev_v.si.rows, "block assignment row", s.raw);
          const std::vector<int64_t> ci = index_positions(
              s.lhs_idx[1], prev_v.si.cols, "block assignment column", s.raw);
          if ((int64_t)(ri.size() * ci.size()) != g.slots[rhs].len)
            fail("block assignment size mismatch for " + s.lhs, s.raw);
          Val nv = prev_v;
          for (size_t j = 0; j < ci.size(); ++j)
            for (size_t i = 0; i < ri.size(); ++i) {
              const Val el = emit_value(OP_INDEX, {rhs_v}, 1, view_of("UReal"),
                                        {(int)(j * ri.size() + i)});
              const Val next =
                  emit_value(OP_SET_INDEX, {nv, el}, g.slots[prev].len, out_si,
                             {(int)(ci[j] * prev_v.si.rows + ri[i])});
              propagate_int_update(next, nv, el, ci[j] * prev_v.si.rows + ri[i],
                                   1);
              nv = next;
            }
          scope[s.lhs] = nv;
          sync_indexed_data_local(s.lhs, nv);
          return;
        }
        if (all_single && dd && s.lhs_idx.size() <= dd->size() &&
            !is_matrix(prev_v.si)) {
          // The mirror of the read path, through the same flat_addr.
          const auto& D = *dd;
          const bool mat = array_shape(prev_v.si).leaf == ViewKind::Matrix;
          std::vector<int64_t> ix;
          for (const auto& k : s.lhs_idx) ix.push_back(eval_int(k.args[0]) - 1);
          const Addr a = flat_addr(D, mat, ix);
          if (a.len != g.slots[rhs].len && a.len != 1)
            fail("indexed assignment size mismatch for " + s.lhs);
          Val nv =
              a.stride != 1
                  ? emit_value(OP_SET_SLICE_STRIDED, {prev_v, rhs_v},
                               g.slots[prev].len, out_si,
                               {(int)a.off, (int)a.stride})
                  : (a.len == 1
                         ? emit_value(OP_SET_INDEX, {prev_v, rhs_v},
                                      g.slots[prev].len, out_si, {(int)a.off})
                         : emit_value(OP_SET_SLICE, {prev_v, rhs_v},
                                      g.slots[prev].len, out_si, {(int)a.off}));
          propagate_int_update(nv, prev_v, rhs_v, a.off, a.stride);
          scope[s.lhs] = nv;
          sync_indexed_data_local(s.lhs, nv);
          return;
        }
        // A full array-index prefix followed by an explicit `:` for every
        // remaining dimension: H[i, :, :] on array[N] matrix[R, C] (a
        // container leaf), or y_approx[i, :] on a plain array[N, S] real
        // (the remaining dimension is just another array axis, no
        // container leaf at all) -- either way this spells the same
        // whole-remainder replacement flat_addr's "whole elements" case
        // already gives an implicit-rest prefix. Not `all_single` (the
        // trailing indices are All, not omitted or Single), so it falls
        // outside the block above.
        if (dd) {
          size_t prefix_len = 0;
          while (prefix_len < s.lhs_idx.size() &&
                 s.lhs_idx[prefix_len].name == "IndexSingle")
            ++prefix_len;
          bool trailing_all = true;
          for (size_t d = prefix_len; d < s.lhs_idx.size(); ++d)
            if (s.lhs_idx[d].name != "IndexAll") trailing_all = false;
          if (prefix_len > 0 && trailing_all && prefix_len < dd->size() &&
              s.lhs_idx.size() == dd->size()) {
            std::vector<int64_t> ix;
            ix.reserve(prefix_len);
            for (size_t d = 0; d < prefix_len; ++d) {
              const int64_t one = eval_int(s.lhs_idx[d].args[0]);
              check_index(one, (*dd)[d], "array assignment index", s.raw);
              ix.push_back(one - 1);
            }
            const bool mat = array_shape(prev_v.si).leaf == ViewKind::Matrix;
            const Addr a = flat_addr(*dd, mat, ix);
            require_binding(
                rhs_v, a.len,
                indexed_view(prev_v.si, prefix_len, a.len, s.rhs.type_), s.lhs,
                s.raw);
            Val nv = emit_value(OP_SET_SLICE, {prev_v, rhs_v},
                                g.slots[prev].len, out_si, {(int)a.off});
            propagate_int_update(nv, prev_v, rhs_v, a.off, 1);
            scope[s.lhs] = nv;
            sync_indexed_data_local(s.lhs, nv);
            return;
          }
        }
        // Any remaining static selection over a container base resolves
        // through the shared index geometry: mixed ranges, gathers, and
        // upfrom forms over deep and container-leaf arrays, and the
        // one-index matrix row forms. The map enumerates destination cells
        // in the graph's outer-major storage, the order the RHS stores its
        // cells, so repeated gather indices keep CmdStan's last-write-wins.
        if (!is_scalar(prev_v) && !all_single) {
          const BuiltinArgumentShape shape = view_argument_shape(
              prev_v.si, g.slots[prev].len, BuiltinArgumentKind::Real);
          if (s.lhs_idx.size() <= shape.dimensions.size()) {
            std::vector<std::vector<int64_t>> selected;
            std::vector<bool> drops;
            selected.reserve(s.lhs_idx.size());
            drops.reserve(s.lhs_idx.size());
            for (size_t d = 0; d < s.lhs_idx.size(); ++d) {
              selected.push_back(index_positions(s.lhs_idx[d],
                                                 shape.dimensions[d],
                                                 "assignment index", s.raw));
              drops.push_back(s.lhs_idx[d].name == "IndexSingle");
            }
            BuiltinIndexMap map;
            try {
              map = builtin_index_map(shape, selected, drops,
                                      SliceStorageOrder::OuterMajor);
            } catch (const std::invalid_argument& error) {
              fail(std::string("unsupported indexed assignment: ") +
                       error.what(),
                   s.raw);
            }
            if (g.slots[rhs].len != map.count)
              fail("indexed assignment size mismatch for " + s.lhs, s.raw);
            Val nv = prev_v;
            switch (map.kind) {
              case BuiltinSliceMap::Kind::Contiguous:
                nv = with_layout(emit_value(OP_SET_SLICE, {prev_v, rhs_v},
                                            g.slots[prev].len, out_si,
                                            {checked_immediate(
                                                map.count == 0 ? 0 : map.offset,
                                                "assignment offset")}),
                                 owning_layout(out_si));
                propagate_int_update(nv, prev_v, rhs_v, map.offset, 1);
                break;
              case BuiltinSliceMap::Kind::Strided:
                nv = with_layout(
                    emit_value(
                        OP_SET_SLICE_STRIDED, {prev_v, rhs_v},
                        g.slots[prev].len, out_si,
                        {checked_immediate(map.offset, "assignment offset"),
                         checked_immediate(map.stride, "assignment stride")}),
                    owning_layout(out_si));
                propagate_int_update(nv, prev_v, rhs_v, map.offset, map.stride);
                break;
              default:
                for (int64_t k = 0; k < map.count; ++k) {
                  const int cell = checked_immediate(map.gather[(size_t)k],
                                                     "assignment cell");
                  const Val el = emit_value(OP_INDEX, {rhs_v}, 1,
                                            view_of("UReal"), {(int)k});
                  const Val next =
                      emit_value(OP_SET_INDEX, {nv, el}, g.slots[prev].len,
                                 out_si, {cell});
                  propagate_int_update(next, nv, el, map.gather[(size_t)k], 1);
                  nv = next;
                }
                break;
            }
            scope[s.lhs] = nv;
            sync_indexed_data_local(s.lhs, nv);
            return;
          }
        }
        int64_t flat = 0;
        if (all_single && s.lhs_idx.size() == 1) {
          flat = eval_int(s.lhs_idx[0].args[0]) - 1;
        } else if (all_single && s.lhs_idx.size() == 2 &&
                   is_matrix(prev_v.si)) {
          flat = (eval_int(s.lhs_idx[1].args[0]) - 1) * prev_v.si.rows +
                 (eval_int(s.lhs_idx[0].args[0]) - 1);
        } else {
          std::string desc = "unsupported indexed assignment: lhs=" + s.lhs;
          for (const auto& ix : s.lhs_idx)
            desc += " [" + (ix.name.empty() ? "?" : ix.name) + "]";
          fail(desc, s.raw);
        }
        Val nv = with_layout(emit_value(OP_SET_INDEX, {prev_v, rhs_v},
                                        g.slots[prev].len, out_si, {(int)flat}),
                             owning_layout(out_si));
        propagate_int_update(nv, prev_v, rhs_v, flat, 1);
        scope[s.lhs] = nv;
        sync_indexed_data_local(s.lhs, nv);
        return;
      }
      {
        Val rhs = lower_expr(s.rhs);
        auto old = scope.find(s.lhs);
        if (old != scope.end()) {
          require_binding(rhs, g.slots[old->second.slot].len, old->second.si,
                          s.lhs, s.raw);
          const bool param_free = rhs.si.param_free;
          rhs.autodiff = old->second.autodiff;
          rhs.si = old->second.si;
          rhs.si.param_free = param_free;
        } else {
          auto dl = decls.find(s.lhs);
          if (dl != decls.end()) {
            if (dl->second.deferred_shape) {
              dl->second.len = g.slots[rhs.slot].len;
              dl->second.si = rhs.si;
              dl->second.deferred_shape = false;
            } else if (dl->second.len == 0 &&
                       (g.slots[rhs.slot].len != 0 ||
                        (is_matrix(dl->second.si) && is_matrix(rhs.si) &&
                         (dl->second.si.rows != rhs.si.rows ||
                          dl->second.si.cols != rhs.si.cols)))) {
              // stanc3's --O1 inliner declares a function's return
              // variable zero-length (`array[real, 0]`, `vector[0]`)
              // because the returned size is the callee's business, and
              // C++ assignment resizes. Slots do not, so the first
              // whole-variable assignment defines the shape instead.
              dl->second.len = g.slots[rhs.slot].len;
              dl->second.si = rhs.si;
            } else {
              SlotInfo expected = dl->second.si;
              require_binding(rhs, dl->second.len, expected, s.lhs, s.raw);
              const bool pf = rhs.si.param_free;
              rhs.si = expected;
              rhs.si.param_free = pf;
            }
            rhs.autodiff = dl->second.autodiff;
          }
        }
        rhs.layout = owning_layout(rhs.si);
        scope[s.lhs] = rhs;
        sync_data_local(s.lhs, s.rhs, rhs);
      }
      return;
    }
    case mir::Stmt::TargetPE: {
      // Stan defines `target += e` for a container `e` as adding `sum(e)`
      // -- CmdStan's `lp_accum__.add(e)` reduces the whole container. A
      // target term is consumed as a scalar, so the reduction has to
      // happen here; pushing the container's slot would silently
      // contribute element zero alone.
      Val t = lower_expr(s.target);
      if (g.slots[t.slot].len != 1) t = emit_value(OP_SUM_VEC, {t}, 1);
      push_target_term(t.slot);
      return;
    }
    case mir::Stmt::Block:
    case mir::Stmt::SList: {
      if (!write_array_known_static && in_write_array &&
          needs_runtime_control(s)) {
        lower_runtime_ifelse(s);
        return;
      }
      const bool outer_known_static = write_array_known_static;
      write_array_known_static = true;
      try {
        for (const auto& k : s.body) lower_stmt(k);
      } catch (...) {
        write_array_known_static = outer_known_static;
        throw;
      }
      write_array_known_static = outer_known_static;
      return;
    }
    case mir::Stmt::Skip:
      return;
    case mir::Stmt::NRFunApp:
      if (s.fn_name == "FnCheck") {
        // prepare_data checks already ran in bind_data. Any check reaching
        // this lowering belongs to log_prob/write_array and must retain its
        // per-evaluation position, even when its value is parameter-free.
        if (!s.check_transform) fail("malformed FnCheck", s.raw);
        if (mir::is_structured_check(s.check_transform->kind)) {
          if (!s.check_transform->args.empty() || s.fn_args.size() != 1)
            fail("malformed structured FnCheck", s.raw);
          const Val value = lower_expr(s.fn_args[0]);
          const int64_t value_len = g.slots[value.slot].len;
          validate_view(value.si, value_len, "structured FnCheck value");

          auto spec = std::make_shared<StructuredCheckSpec>();
          spec->kind = s.check_transform->kind;
          spec->name =
              s.check_var_name.empty() ? s.fn_args[0].name : s.check_var_name;
          if (is_array(value.si)) {
            const ArrayShape& shape = array_shape(value.si);
            spec->dims = shape.dims;
            if (shape.leaf == ViewKind::Vector)
              spec->leaf = StructuredLeaf::Vector;
            else if (shape.leaf == ViewKind::Matrix)
              spec->leaf = StructuredLeaf::Matrix;
            else
              fail("structured FnCheck requires vector or matrix leaves",
                   s.raw);
          } else if (is_vector(value.si)) {
            spec->dims = {value_len};
            spec->leaf = StructuredLeaf::Vector;
          } else if (is_matrix(value.si)) {
            spec->dims = {value.si.rows, value.si.cols};
            spec->leaf = StructuredLeaf::Matrix;
          } else {
            fail("structured FnCheck requires a vector or matrix", s.raw);
          }

          const size_t leaf_rank = spec->leaf == StructuredLeaf::Matrix ? 2 : 1;
          const mir::UnsizedLeaf expr_leaf = s.fn_args[0].unsized.leaf;
          if (s.fn_args[0].unsized.depth != spec->dims.size() - leaf_rank ||
              (spec->leaf == StructuredLeaf::Vector &&
               expr_leaf != mir::UnsizedLeaf::Vector) ||
              (spec->leaf == StructuredLeaf::Matrix &&
               expr_leaf != mir::UnsizedLeaf::Matrix))
            fail("structured FnCheck type does not match its value", s.raw);
          const bool matrix_only = spec->kind == mir::Transform::CholeskyCorr ||
                                   spec->kind == mir::Transform::Correlation ||
                                   spec->kind == mir::Transform::Covariance ||
                                   spec->kind == mir::Transform::CholeskyCov;
          const bool vector_only =
              spec->kind != mir::Transform::SumToZero && !matrix_only;
          if ((matrix_only && spec->leaf != StructuredLeaf::Matrix) ||
              (vector_only && spec->leaf != StructuredLeaf::Vector))
            fail("structured FnCheck transform and leaf disagree", s.raw);

          (void)emit_value(OP_CHECK_STRUCTURED, {value}, 1);
          g.ops.back().udata = spec.get();
          g.udata_pool.push_back(std::move(spec));
          return;
        }
        if (s.check_transform->args.size() != 1 || s.fn_args.size() != 2)
          fail("malformed FnCheck", s.raw);
        const uint16_t opcode =
            s.check_transform->kind == mir::Transform::Lower   ? OP_CHECK_LOWER
            : s.check_transform->kind == mir::Transform::Upper ? OP_CHECK_UPPER
                                                               : 0;
        if (opcode == 0) fail("unsupported FnCheck transform", s.raw);

        const Val value = lower_expr(s.fn_args[0]);
        const Val bound = lower_expr(s.fn_args[1]);
        const int64_t value_len = g.slots[value.slot].len;
        const int64_t bound_len = g.slots[bound.slot].len;
        validate_view(value.si, value_len, "FnCheck value");
        validate_view(bound.si, bound_len, "FnCheck bound");
        const bool bound_is_scalar = is_scalar(bound);
        const bool shapes_match =
            is_scalar(value)
                ? bound_is_scalar
                : (bound_is_scalar ||
                   same_view(value.si, value_len, bound.si, bound_len));

        auto spec = std::make_shared<BoundCheckSpec>();
        spec->name =
            s.check_var_name.empty() ? s.fn_args[0].name : s.check_var_name;
        spec->bound_is_scalar = bound_is_scalar;
        spec->shapes_match = shapes_match;
        (void)emit_value(opcode, {value, bound}, 1);
        g.ops.back().udata = spec.get();
        g.udata_pool.push_back(std::move(spec));
        return;
      }
      // Size validation remains a separate compatibility seam.
      if (s.fn_name == "FnValidateSize") return;
      if (s.fn_name == "check_matching_dims") {
        if (s.fn_args.size() != 5 || s.fn_args[0].kind != mir::Expr::LitStr ||
            s.fn_args[1].kind != mir::Expr::LitStr ||
            s.fn_args[3].kind != mir::Expr::LitStr)
          fail("malformed check_matching_dims", s.raw);
        const Val value = lower_expr(s.fn_args[2]);
        const Val bound = lower_expr(s.fn_args[4]);
        const int64_t value_len = g.slots[value.slot].len;
        const int64_t bound_len = g.slots[bound.slot].len;
        validate_view(value.si, value_len, "check_matching_dims value");
        validate_view(bound.si, bound_len, "check_matching_dims bound");
        auto spec = std::make_shared<BoundCheckSpec>();
        spec->name = s.fn_args[1].lit_s;
        spec->shapes_match =
            same_view(value.si, value_len, bound.si, bound_len);
        (void)emit_value(OP_CHECK_MATCHING_DIMS, {value, bound}, 1);
        g.ops.back().udata = spec.get();
        g.udata_pool.push_back(std::move(spec));
        return;
      }
      // Deliberately not a `check_*` prefix match: a value check like
      // check_positive_finite rejects a draw at runtime, and skipping one
      // would silently accept points CmdStan refuses.
      // reject() and print(): the message is a mix of string literals
      // and expressions, so the literals become the op's chunk list and
      // the expressions become its inputs. reject throws
      // std::domain_error at forward time, which is the same exception
      // from the same place CmdStan's generated code throws it, so the
      // sampler counts it as a rejected proposal rather than a failure.
      if (const auto action = message_action(s.fn_name)) {
        auto spec = std::make_shared<MessageSpec>();
        std::vector<int> ins;
        *spec =
            lower_message_arguments(s.fn_args, [&](const mir::Expr& argument) {
              // Op::in holds six. Keep that backend capacity check here;
              // parsing and semantic dispatch remain shared.
              if (ins.size() >= 6)
                fail(std::string(*action == MessageAction::Reject ? "reject"
                                                                  : "print") +
                         " with more than 6 printed values",
                     s.raw);
              ins.push_back(lower_expr(argument).slot);
            });
        Op op;
        op.opcode = *action == MessageAction::Reject ? OP_REJECT : OP_PRINT;
        op.n_in = (int)ins.size();
        for (size_t k = 0; k < ins.size(); ++k) op.in[k] = ins[k];
        // The output is a dead scalar: every op writes somewhere, and
        // nothing reads this one.
        op.out = add_slot(1, false);
        op.udata = spec.get();
        g.udata_pool.push_back(spec);
        g.ops.push_back(op);
        return;
      }
      if (s.fn_name == "FnWriteParam") {
        // One CSV column, at the point the emission happens: this is what
        // fixes the column order to CmdStan's. Arrays of containers are
        // emitted one element at a time -- `array[K] simplex[K] theta`
        // arrives as K writes of `theta[k]` -- and CmdStan names those
        // columns outer-index-first, theta.1.1 .. theta.1.K, theta.2.1 ...
        // so the index path becomes part of the column name.
        if (s.fn_args.size() != 1) fail("FnWriteParam arity", s.raw);
        std::vector<long> ixs;
        const mir::Expr* base = &s.fn_args[0];
        while (base->kind == mir::Expr::Indexed) {
          for (size_t k = base->args.size(); k-- > 1;) {
            if (base->args[k].name != "IndexSingle")
              fail("FnWriteParam under a non-scalar index", s.raw);
            ixs.push_back(eval_int(base->args[k].args[0]));
          }
          base = &base->args[0];
        }
        if (base->kind != mir::Expr::Var)
          fail("FnWriteParam of a non-variable", s.raw);
        std::string name = base->name;
        for (auto it = ixs.rbegin(); it != ixs.rend(); ++it)
          name += "." + std::to_string(*it);
        const Val v = lower_expr(s.fn_args[0]);
        // stanc peels the array dimensions, so what is left here is a
        // scalar, a vector/row_vector, or a matrix -- and its type decides
        // how CmdStan indexes the columns.
        using Naming = CompiledModel::ParamView::Naming;
        const std::string& t = s.fn_args[0].type_;
        CompiledModel::ParamView pv{name, v.slot, g.slots[v.slot].len};
        if (t == "UReal" || t == "UInt" || t == "UComplex") {
          pv.naming = Naming::Scalar;
        } else if (t == "UMatrix") {
          if (!is_matrix(v.si))
            fail("FnWriteParam of a matrix with unknown shape: " + name, s.raw);
          pv.rows = v.si.rows;
          pv.naming = Naming::Matrix;
        } else {
          pv.naming = Naming::Container;
        }
        out.views.push_back(pv);
        return;
      }
      fail("unsupported statement function " + s.fn_name);
    case mir::Stmt::For: {
      long lo = 0, hi = 0;
      try {
        lo = eval_int(s.lower);
        hi = eval_int(s.upper);
      } catch (const CompileError&) {
        if (in_write_array ||
            !(needs_runtime_value(s.lower) || needs_runtime_value(s.upper)) ||
            !try_lower_region(s))
          throw;
        return;
      }
      if (lo > hi) {
        int_env.erase(s.loopvar);
        return;
      }
      // Both the pre-control target fold and the ordinary path ask the same
      // structural question.  A nonselected automatic candidate reaches
      // both sites, so retain the answer for this lowering encounter rather
      // than walking a potentially large body twice.
      std::optional<bool> repeatable_target;
      const auto has_repeatable_target = [&]() {
        if (!repeatable_target) repeatable_target = repeatable_target_body(s);
        return *repeatable_target;
      };
      // Both cheap invariant folding and retained selection precede the
      // per-iteration control scan. Neither needs an expanded graph.
      if ((structured_policy == StructuredMode::Prefer ||
           structured_policy == StructuredMode::Force) &&
          lo != hi && has_repeatable_target()) {
        const double old_scale = target_scale;
        target_scale *= static_cast<double>(hi) - static_cast<double>(lo) + 1;
        int_env[s.loopvar] = lo;
        try {
          for (const auto& child : s.body) lower_stmt(child);
        } catch (...) {
          target_scale = old_scale;
          int_env.erase(s.loopvar);
          throw;
        }
        target_scale = old_scale;
        int_env.erase(s.loopvar);
        return;
      }
      if (try_lower_region(s, std::pair<int64_t, int64_t>{lo, hi})) return;
      // runtime_loop_control evaluates data-only conditions while looking
      // for a parameter-selected break/continue. Scan under the same loop
      // binding that ordinary unrolling will use: without it, an indexed
      // condition such as idx[ri] is either treated as spuriously dynamic
      // or can escape static-shape specialization as an unknown variable.
      // The bounds come first so a zero-trip loop never evaluates its body.
      const auto old = int_env.find(s.loopvar);
      const bool had_old = old != int_env.end();
      const long old_value = had_old ? old->second : 0;
      bool has_runtime_loop_control = false;
      try {
        for (long v = lo; v <= hi && !has_runtime_loop_control; ++v) {
          int_env[s.loopvar] = v;
          for (const auto& child : s.body)
            if (runtime_loop_control(child)) {
              has_runtime_loop_control = true;
              break;
            }
        }
      } catch (...) {
        if (had_old)
          int_env[s.loopvar] = old_value;
        else
          int_env.erase(s.loopvar);
        throw;
      }
      if (had_old)
        int_env[s.loopvar] = old_value;
      else
        int_env.erase(s.loopvar);
      if (has_runtime_loop_control) {
        lower_runtime_ifelse(s);
        return;
      }
      if (lo != hi && has_repeatable_target()) {
        const double old_scale = target_scale;
        target_scale *= static_cast<double>(hi) - static_cast<double>(lo) + 1.0;
        int_env[s.loopvar] = lo;
        try {
          for (const auto& child : s.body) lower_stmt(child);
        } catch (...) {
          target_scale = old_scale;
          int_env.erase(s.loopvar);
          throw;
        }
        target_scale = old_scale;
        int_env.erase(s.loopvar);
        return;
      }
      for (long v = lo; v <= hi; ++v) {
        int_env[s.loopvar] = v;
        try {
          for (const auto& k : s.body) lower_stmt(k);
        } catch (LoopContinue&) {
          continue;
        } catch (LoopBreak&) {
          break;
        }
      }
      int_env.erase(s.loopvar);
      return;
    }
    case mir::Stmt::While: {
      if (try_lower_region(s)) return;
      // Unlike `for`, a `while` has no statically supplied trip count.
      // Compile it as one structured register-program island, which
      // rechecks its guard at execution time and replays the executed
      // iterations under autodiff.  This deliberately has no lowering-time
      // iteration cap: nontermination is the model's runtime behaviour,
      // not a reason to silently truncate or reject a finite long loop.
      lower_runtime_ifelse(s);
      return;
    }
    case mir::Stmt::IfElse: {
      // The guards are data-only and fold away below (both flags are
      // pinned on), so this is the only chance to note that a CSV
      // section ended here.
      if (in_write_array) {
        switch (mir::emit_guard(s)) {
          case mir::EmitGuard::TransformedParams:
            if (!n_tp_start) n_tp_start = out.views.size();
            break;
          case mir::EmitGuard::GeneratedQuantities:
            if (!n_gq_start) n_gq_start = out.views.size();
            break;
          case mir::EmitGuard::None:
            break;
        }
      }
      bool known = false, c = false;
      if (auto evaluated = try_eval_pure(s.cond)) {
        c = evaluated->r.at(0) != 0.0;
        known = true;
      }
      if (known) {
        if (c && !s.body.empty()) lower_stmt(s.body[0]);
        if (!c && s.body.size() > 1) lower_stmt(s.body[1]);
        return;
      }
      if (udf_depth > 0 && s.body.size() == 2) {
        mir::Stmt effects = s;
        mir::Expr then_value, else_value;
        if (peel_terminal_return(&effects.body[0], &then_value) &&
            peel_terminal_return(&effects.body[1], &else_value)) {
          std::vector<std::string> assigned;
          assigned_names(effects, &assigned);
          if (!assigned.empty() || has_target_pe(effects) ||
              stmt_effectful(effects))
            lower_runtime_ifelse(effects);

          mir::Expr choice;
          choice.kind = mir::Expr::TernaryIf;
          choice.args = {s.cond, then_value, else_value};
          choice.type_ = then_value.type_;
          choice.unsized = then_value.unsized;
          choice.data_only =
              s.cond.data_only && then_value.data_only && else_value.data_only;
          choice.raw = s.raw;
          throw LpReturn{lower_expr(choice)};
        }
      }
      // Data-only or not, an unfoldable condition compiles to an island.
      // Data-only says the MIR adlevel is DataOnly, not that the values are
      // in the interpreter's frame: a UDF local built by indexed assignment
      // lives in the graph, and only the region compiler can read it there.
      // The island's live-outs come back parameter-dependent, which costs
      // adjoints such a branch does not need but is never wrong.
      lower_runtime_ifelse(s);
      return;
    }
    case mir::Stmt::Return:
      // Only reachable inside an inlined UDF body (log_prob itself has no
      // value returns); unwinds to lower_call_udf.
      if (!s.has_init) fail("void return unsupported in UDF inlining");
      throw LpReturn{lower_expr(s.rhs)};
    case mir::Stmt::Break:
      throw LoopBreak{};
    case mir::Stmt::Continue:
      throw LoopContinue{};
    default:
      fail("unsupported statement", s.raw);
  }
}
}  // namespace lower_detail
}  // namespace stanli

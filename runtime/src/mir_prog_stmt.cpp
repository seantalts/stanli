// ProgramCompiler: statements, loops and assignment.
#include <stanli/mir_prog.hpp>

namespace stanli {

void check_program_integer_contract(
    const std::vector<stanli::mir::Stmt>& body,
    const std::map<std::string, const stanli::mir::FunDef*>& functions,
    std::set<const stanli::mir::FunDef*>& visited) {
  std::function<void(const mir::Expr&)> expression;
  expression = [&](const mir::Expr& value) {
    if (value.kind == mir::Expr::FunApp) {
      if (value.fn_lib == mir::Expr::Lib::UserDefined) {
        const auto found = functions.find(mir::function_key(value));
        if (found != functions.end() && visited.insert(found->second).second)
          check_program_integer_contract(found->second->body, functions,
                                         visited);
      } else if (const auto* spec = function_spec(value);
                 spec && spec->builtin() &&
                 spec->result() == FunctionArgumentKind::Integer) {
        const auto policy = spec->builtin()->shape;
        const auto op = spec->builtin()->opcode;
        const bool native_integer =
            op == OP_ADD || op == OP_SUB || op == OP_MUL || op == OP_DIV ||
            op == OP_NEG || op == OP_ABS || op == OP_CHOOSE ||
            op == OP_SUM_VEC || op == OP_TO_INT;
        // Stanc retains a negative integer literal as unary negation even
        // when a real comparison consumes it. Its exact value is proven here
        // without invoking integer arithmetic on unknown runtime operands.
        const bool literal_negation =
            spec->builtin()->opcode == OP_NEG && value.args.size() == 1 &&
            value.args[0].kind == mir::Expr::LitInt &&
            value.args[0].lit_i >= -(int64_t)std::numeric_limits<int>::max() &&
            value.args[0].lit_i <= std::numeric_limits<int>::max();
        if (!native_integer && !literal_negation &&
            spec->builtin()->ordering == BuiltinOrdering::None &&
            policy != BuiltinShapePolicy::Predicate &&
            policy != BuiltinShapePolicy::ShapeQuery &&
            policy != BuiltinShapePolicy::SliceView &&
            policy != BuiltinShapePolicy::Constructor &&
            policy != BuiltinShapePolicy::Rng)
          throw Bail{"integer arithmetic requires the MIR interpreter"};
      }
    }
    for (const auto& child : value.args) expression(child);
  };
  std::function<void(const mir::Stmt&)> statement;
  statement = [&](const mir::Stmt& value) {
    for (const auto* expr : {&value.init, &value.rhs, &value.target,
                             &value.lower, &value.upper, &value.cond})
      expression(*expr);
    for (const auto* exprs : {&value.read_dims, &value.lhs_idx, &value.fn_args})
      for (const auto& expr : *exprs) expression(expr);
    for (const auto& child : value.body) statement(child);
  };
  for (const auto& value : body) statement(value);
}

void ProgramCompiler::mark_int_reset(const std::string& name) {
  if (!structured_while_depth) return;
  const int at = emit(Program::JMP, 0);
  int_reset_at[name] = at;
  unused_markers.insert(at);
}

void ProgramCompiler::assigned_names(const mir::Stmt& s,
                                     std::set<std::string>* names) {
  mir::for_each_assigned(s,
                         [&](const std::string& name) { names->insert(name); });
  for (const auto& child : s.body) assigned_names(child, names);
}

bool ProgramCompiler::invariant_for_bound(
    const mir::Expr& e, const std::set<std::string>& written) {
  // Fixed container geometry does not depend on element mutations.
  // Retain the effect check on the operand even when only its shape matters.
  if (is_shape_query(e)) {
    Range view;
    if (static_view(e.args[0], &view))
      return invariant_for_bound(e.args[0], {});
  }
  switch (e.kind) {
    case mir::Expr::Var:
      return !written.count(e.name);
    case mir::Expr::LitInt:
    case mir::Expr::LitReal:
      return true;
    case mir::Expr::Promotion:
    case mir::Expr::TernaryIf:
    case mir::Expr::EAnd:
    case mir::Expr::EOr:
    case mir::Expr::Indexed:
      break;
    case mir::Expr::IndexAll:
    case mir::Expr::IndexSingle:
    case mir::Expr::IndexUpfrom:
    case mir::Expr::IndexBetween:
    case mir::Expr::IndexMulti:
      break;
    case mir::Expr::FunApp: {
      if (e.name == "FnMakeArray" || e.name == "FnMakeRowVec") break;
      if (e.fn_lib != mir::Expr::Lib::StanLib) return false;
      const auto* spec = function_spec(e);
      if (!spec || !spec->builtin()) return false;
      if (spec->builtin()->shape == BuiltinShapePolicy::Rng) return false;
      break;
    }
    default:
      return false;
  }
  for (const auto& arg : e.args)
    if (!invariant_for_bound(arg, written)) return false;
  return true;
}

void ProgramCompiler::reify_written_int(const std::string& name) {
  auto it = ints.find(name);
  if (it == ints.end()) return;
  if (it->second.size() != 1)
    bail("structured while writes an integer array: " + name);
  const int r = alloc(1);
  const double value = static_cast<double>(it->second[0]);
  const auto reset = int_reset_at.find(name);
  if (reset == int_reset_at.end()) {
    hoisted_initializers.push_back(const_instr(r, &value, 1));
  } else {
    p.code[reset->second] = const_instr(r, &value, 1);
    unused_markers.erase(reset->second);
    int_reset_at.erase(reset);
  }
  reals[name] = Range{r, 1};
  ints.erase(it);
  int_decl_at.erase(name);
}

void ProgramCompiler::reify_loop_write(const std::string& name) {
  if (int_array_names.count(name)) {
    auto known = known_int_arrays.find(name);
    if (!reals.count(name) && known != known_int_arrays.end()) {
      const auto shape = known_int_array_dims.find(name);
      const std::vector<int64_t> dims =
          shape == known_int_array_dims.end()
              ? std::vector<int64_t>{static_cast<int64_t>(known->second.size())}
              : shape->second;
      const auto values = int_array_graph_values(known->second, dims);
      Range value{alloc(static_cast<int>(values.size())),
                  static_cast<int>(values.size())};
      value.kind = ViewKind::Array;
      value.dims = dims;
      emit_const(value.reg, values.data(), value.len);
      reals[name] = value;
    }
    known_int_arrays.erase(name);
    known_int_array_dims.erase(name);
  } else {
    reify_written_int(name);
  }
  // An imported value written on an earlier iteration is no longer a
  // preparation constant, even before the first write in this body.
  if (!reals.count(name) && bind_extern) {
    Range value;
    if (bind_outer(name, &value)) reals[name] = value;
  }
  extern_bound.erase(name);
}

void ProgramCompiler::require_runtime_integer_contract() {
  if (!return_frame && !checked_region)
    bail("runtime for/integer binding needs a checked function scope");
  std::set<const mir::FunDef*> visited;
  if (checked_region)
    check_program_integer_contract({*checked_region}, funs, visited);
  for (auto* frame = return_frame; frame; frame = frame->parent)
    check_program_integer_contract(*frame->body, funs, visited);
}

void ProgramCompiler::compile_loop_body(const mir::Stmt& loop, int index) {
  known_reals.erase(loop.loopvar);
  require_runtime_integer_contract();
  ints.erase(loop.loopvar);
  reals[loop.loopvar] = Range{index, 1};
  loops.push_back({});
  loops.back().structured = true;
  structured_while_seen = true;
  ++structured_while_depth;
  ++runtime_for_depth;
  for (const auto& child : loop.body)
    assigned_names(child, &loops.back().assigned);
  forget_flags(loops.back().assigned, loop.loopvar);
  start_loop_facts();
  (void)compile_body(loop.body);
  join_loop_exits();
  --runtime_for_depth;
  --structured_while_depth;
  loops.pop_back();
  reals.erase(loop.loopvar);
  int_decl_at.erase(loop.loopvar);
}

ProgramCompiler::Path ProgramCompiler::compile_body(
    const std::vector<mir::Stmt>& body) {
  for (const auto& child : body)
    if (const Path path = compile_stmt(child); path != Path::Open) return path;
  return Path::Open;
}

Range ProgramCompiler::copied_if_overlapping(const Range& v, const Range& dst) {
  if (v.len < 2 || v.reg >= dst.reg + dst.len || dst.reg >= v.reg + v.len)
    return v;
  Range copy = v;
  copy.reg = alloc(v.len);
  for (int k = 0; k < v.len; ++k) emit(Program::MOV, copy.reg + k, v.reg + k);
  return copy;
}

void ProgramCompiler::refuse_second_size(const std::string& name) {
  if (region_sized.count(name))
    bail(name +
         " is given containers of different sizes under a condition that "
         "depends on a parameter, which stanli does not support");
}

bool ProgramCompiler::bind_outer(const std::string& name, Range* r) {
  if (!bind_extern || !bind_extern(name, r)) return false;
  int status = -1;
  if (bind_empty_status && bind_empty_status(name, &status)) {
    if (!is_tracked(name)) unassigned.tracked.push_back(name);
    unassigned.empty_reg[name] = status;
    unassigned.path.insert_empty(name);
  } else if (r->len == 0) {
    note_entered_empty(name);
  }
  return true;
}

const std::set<std::string>& ProgramCompiler::PathFacts::maybe_empty() const {
  static const std::set<std::string> none;
  return known ? known->maybe_empty : none;
}

const std::map<std::string, long>& ProgramCompiler::PathFacts::flags() const {
  static const std::map<std::string, long> none;
  return known ? known->flags : none;
}

ProgramCompiler::PathFacts::Known& ProgramCompiler::PathFacts::edit() {
  if (!known)
    known = std::make_shared<Known>();
  else if (known.use_count() > 1)
    known = std::make_shared<Known>(*known);
  return *known;
}

void ProgramCompiler::PathFacts::insert_empty(const std::string& name) {
  if (!maybe_empty().count(name)) edit().maybe_empty.insert(name);
}

bool ProgramCompiler::PathFacts::erase_empty(const std::string& name) {
  if (!maybe_empty().count(name)) return false;
  edit().maybe_empty.erase(name);
  return true;
}

void ProgramCompiler::PathFacts::set_flag(const std::string& name, long value) {
  const auto& current = flags();
  const auto found = current.find(name);
  if (found == current.end() || found->second != value)
    edit().flags[name] = value;
}

void ProgramCompiler::PathFacts::erase_flag(const std::string& name) {
  if (flags().count(name)) edit().flags.erase(name);
}

ProgramCompiler::PathFacts ProgramCompiler::join(const PathFacts& a,
                                                 const PathFacts& b) {
  if (!a.reachable) return b;
  if (!b.reachable || a.known == b.known) return a;
  PathFacts out{true, nullptr};
  for (const auto& name : a.maybe_empty()) out.insert_empty(name);
  for (const auto& name : b.maybe_empty()) out.insert_empty(name);
  const auto& other = b.flags();
  for (const auto& [name, value] : a.flags()) {
    const auto found = other.find(name);
    if (found != other.end() && found->second == value)
      out.set_flag(name, value);
  }
  return out;
}

void ProgramCompiler::note_entered_empty(const std::string& name) {
  if (is_tracked(name)) return;
  unassigned.tracked.push_back(name);
  unassigned.entered_empty.insert(name);
  unassigned.marks[name];
  unassigned.path.insert_empty(name);
}

void ProgramCompiler::write_empty_status(const std::string& name,
                                         double status) {
  const auto reg = unassigned.empty_reg.find(name);
  if (reg != unassigned.empty_reg.end()) {
    emit_const(reg->second, &status, 1);
    return;
  }
  if (!unassigned.marks.count(name)) unassigned.tracked.push_back(name);
  const int at = emit(Program::JMP, 0);
  unassigned.marks[name].emplace_back(at, status);
  unused_markers.insert(at);
}

void ProgramCompiler::note_empty(const std::string& name) {
  unassigned.path.insert_empty(name);
  write_empty_status(name, 1.0);
}

void ProgramCompiler::note_assigned(const std::string& name) {
  // Where no path has the local empty, its status is already 0.
  if (!unassigned.path.maybe_empty().count(name)) return;
  const auto it = reals.find(name);
  if (it == reals.end() || it->second.len == 0) return;
  unassigned.path.erase_empty(name);
  write_empty_status(name, 0.0);
}

int ProgramCompiler::empty_status_register(const std::string& name) {
  const auto found = unassigned.empty_reg.find(name);
  if (found != unassigned.empty_reg.end()) return found->second;
  const int reg = alloc(1);
  if (unassigned.entered_empty.count(name)) {
    const double empty = 1.0;
    hoisted_initializers.push_back(const_instr(reg, &empty, 1));
  }
  for (const auto& [at, status] : unassigned.marks[name]) {
    p.code[(size_t)at] = const_instr(reg, &status, 1);
    unused_markers.erase(at);
  }
  unassigned.marks.erase(name);
  unassigned.empty_reg[name] = reg;
  return reg;
}

bool ProgramCompiler::may_be_empty(const std::string& name) const {
  const auto it = reals.find(name);
  return it != reals.end() && it->second.len != 0 &&
         unassigned.path.reachable &&
         unassigned.path.maybe_empty().count(name) &&
         (unassigned.empty_reg.count(name) || unassigned.marks.count(name));
}

void ProgramCompiler::require_assigned(const std::string& name) {
  if (unassigned.tracked.empty() || !unassigned.path.reachable) return;
  const auto& maybe_empty = unassigned.path.maybe_empty();
  const bool in_loop = runtime_for_depth || structured_while_depth;
  // Most reads have nothing to check; answer those without a lookup.
  if (maybe_empty.empty() && !in_loop) return;
  const auto it = reals.find(name);
  if (it == reals.end()) return;
  if (it->second.len != 0) {
    if (!maybe_empty.count(name) || !is_tracked(name)) return;
    emit_empty_check(name, empty_status_register(name));
    // The paths that continue past the check have the local assigned.
    unassigned.path.erase_empty(name);
    return;
  }
  // The read was compiled for an empty local, which a later statement of
  // the loop around it may have assigned on an earlier trip.
  if (!in_loop || !is_tracked(name)) return;
  for (const LoopFrame& loop : loops)
    if (loop.assigned.count(name)) {
      emit_empty_check(name, empty_status_register(name), true);
      return;
    }
}

void ProgramCompiler::emit_empty_check(const std::string& name, int status,
                                       bool if_assigned) {
  Program::Message m;
  m.spec.chunks = {
      "stanli does not support a local whose size depends on a "
      "branch on parameters: " +
      name +
      (if_assigned ? " was assigned on an earlier trip of a loop"
                   : " was not assigned on this path")};
  m.spec.error = MessageError::Runtime;
  p.messages.push_back(std::move(m));
  const int skip = emit(Program::JZ, 0, status);
  int pass = -1;
  if (if_assigned) {
    pass = emit(Program::JMP, 0);
    p.code[(size_t)skip].dst = (int)p.code.size();
  }
  emit(Program::REJECT, 0, (int)p.messages.size() - 1);
  p.code[(size_t)(if_assigned ? pass : skip)].dst = (int)p.code.size();
}

void ProgramCompiler::jump_with_facts(PathFacts* target) {
  if (target) *target = join(*target, unassigned.path);
  unassigned.path = PathFacts{};
}

bool ProgramCompiler::is_tracked(const std::string& name) const {
  return unassigned.empty_reg.count(name) || unassigned.marks.count(name);
}

void ProgramCompiler::join_entered(size_t tracked_before) {
  if (!unassigned.path.reachable) return;
  for (size_t k = tracked_before; k < unassigned.tracked.size(); ++k)
    unassigned.path.insert_empty(unassigned.tracked[k]);
}

void ProgramCompiler::start_loop_facts() {
  loops.back().at_start = unassigned.path;
  loops.back().tracked_at_start = unassigned.tracked.size();
}

void ProgramCompiler::join_loop_exits() {
  LoopFrame& loop = loops.back();
  unassigned.path = join(join(unassigned.path, loop.at_start),
                         join(loop.at_break, loop.at_continue));
  loop.at_start = loop.at_break = loop.at_continue = PathFacts{};
  join_entered(loop.tracked_at_start);
}

void ProgramCompiler::forget_flags(const std::set<std::string>& written,
                                   const std::string& loopvar) {
  if (unassigned.path.flags().empty()) return;
  unassigned.path.erase_flag(loopvar);
  for (const std::string& name : written) unassigned.path.erase_flag(name);
}

void ProgramCompiler::join_jumps(PathFacts* jumps) {
  unassigned.path = join(unassigned.path, *jumps);
  *jumps = PathFacts{};
}

void ProgramCompiler::forget_local(const std::string& name) {
  unassigned.path.erase_empty(name);
  unassigned.path.erase_flag(name);
}

void ProgramCompiler::emit_assign_size_reject(const std::string& name,
                                              const std::string& message) {
  refuse_second_size(name);
  Program::Message m;
  m.spec.chunks = {message};
  m.spec.error = MessageError::InvalidArgument;
  p.messages.push_back(std::move(m));
  emit(Program::REJECT, 0, (int)p.messages.size() - 1);
}

std::vector<int64_t> ProgramCompiler::assign_dims(const Range& r) {
  switch (r.kind) {
    case ViewKind::Array:
      return r.dims.empty() ? std::vector<int64_t>{r.len} : r.dims;
    case ViewKind::Matrix:
      return {r.rows, r.cols};
    case ViewKind::Vector:
    case ViewKind::RowVector:
      return {r.len};
    case ViewKind::Flat:
      break;
  }
  return {};
}

std::string ProgramCompiler::whole_assign_size_message(const std::string& name,
                                                       const Range& dst,
                                                       const Range& v) {
  const ViewKind leaf = dst.kind == ViewKind::Array ? dst.leaf : dst.kind;
  const AssignLeaf assign_leaf = leaf == ViewKind::Matrix   ? AssignLeaf::Matrix
                                 : leaf == ViewKind::Vector ? AssignLeaf::Vector
                                 : leaf == ViewKind::RowVector
                                     ? AssignLeaf::RowVector
                                     : AssignLeaf::Scalar;
  const size_t rank = leaf_rank(leaf);
  const std::vector<int64_t> dst_dims = assign_dims(dst);
  if (dst_dims.size() < rank) return "";
  return assign_size_mismatch(name, assign_leaf, dst_dims.size() - rank,
                              dst_dims, assign_dims(v));
}

std::string ProgramCompiler::selected_assign_size_message(
    const std::string& name, const mir::Expr& index, bool array, int64_t want,
    int64_t got) {
  const bool empty_range = index.kind == mir::Expr::IndexBetween && want == 0;
  std::string label;
  if (array)
    label = empty_range ? "array[negative_min_max, ...] assign"
                        : "array[multi, ...] assign";
  else if (index.kind == mir::Expr::IndexBetween)
    label = empty_range ? "vector[negative_min_max] assign"
                        : "vector[min_max] assign";
  else if (index.kind == mir::Expr::IndexUpfrom)
    label = "vector[min] assign";
  else
    label = "vector[multi] assign";
  const bool size_word = array && !empty_range;
  return label + ": assigning variable " + name + " (" + std::to_string(want) +
         ") and right hand side" + (size_word ? " size" : "") + " (" +
         std::to_string(got) + ") must match in size";
}

ProgramCompiler::Path ProgramCompiler::compile_multi_return(const mir::Stmt& s,
                                                            bool runtime_exit) {
  std::vector<Range> values;
  for (const auto& value : s.fn_args) values.push_back(expr(value));
  if (return_frame && (runtime_exit || return_frame->has_result)) {
    emit_function_return(values, true);
    return Path::Ended;
  }
  if (!return_frame) bail("return without a function scope");
  return_frame->results = std::move(values);
  return Path::Returned;
}

ProgramCompiler::Path ProgramCompiler::compile_multi_assign(
    const mir::Stmt& s) {
  std::vector<Range> values;
  if (s.rhs.fn_lib == mir::Expr::Lib::StanLib) {
    const BuiltinSpec* matrix = shaped_builtin_spec(
        s.rhs.name, s.rhs.args.size(), BuiltinShapePolicy::MatrixOp);
    if (matrix == nullptr)
      bail(s.rhs.name +
           ", which returns a tuple, is not supported in a "
           "runtime-control region");
    BuiltinMatrixMap map;
    const Range packed = matrix_op_call(s.rhs, matrix, &map);
    int offset = 0;
    for (const BuiltinArgumentShape& part : map.parts) {
      values.push_back(
          shaped(Range{packed.reg + offset, (int)part.storage_size}, part));
      offset += (int)part.storage_size;
    }
  } else if (s.rhs.fn_lib == mir::Expr::Lib::UserDefined) {
    auto it = funs.find(mir::function_key(s.rhs));
    if (it == funs.end()) bail("unknown function " + s.rhs.name);
    (void)inline_call(*it->second, inline_arguments(s.rhs.args), false,
                      propto_ctx && mir::inherits_propto(s.rhs), &values);
  } else {
    bail(s.rhs.name + ", which returns a tuple, is not supported yet");
  }
  if (values.size() != s.fn_args.size())
    bail(s.rhs.name + " returned the wrong number of values");
  // Each result is assigned as `target = value`: the assignment path takes
  // the result's registers in place of evaluating a right side.
  struct Given {
    const mir::Stmt*& stmt;
    ~Given() { stmt = nullptr; }
  } given_scope{given_stmt};
  for (size_t k = 0; k < values.size(); ++k) {
    mir::Stmt assign;
    assign.kind = mir::Stmt::Assignment;
    assign.lhs = s.fn_args[k].name;
    assign.raw = s.raw;
    given_stmt = &assign;
    given_value = values[k];
    const Path path = compile_stmt(assign);
    given_stmt = nullptr;
    if (path != Path::Open) bail("a MultiAssign target left the scope");
  }
  return Path::Open;
}

ProgramCompiler::Path ProgramCompiler::compile_runtime_if(const mir::Stmt& s) {
  std::vector<int> to_else;
  cond_jump(s.cond, false, &to_else);
  ++branch_depth;
  const PathFacts before = unassigned.path;
  const size_t tracked_before = unassigned.tracked.size();
  std::optional<long> flag;
  if (s.cond.kind == mir::Expr::Var)
    if (const auto known = before.flags().find(s.cond.name);
        known != before.flags().end())
      flag = known->second;
  // An arm on a flag that is known not to run adds no path.
  if (flag && *flag == 0) unassigned.path = PathFacts{};
  const bool then_exits =
      !s.body.empty() && compile_stmt(s.body[0]) == Path::Ended;
  PathFacts after_then = std::exchange(unassigned.path, before);
  if (flag && *flag != 0) unassigned.path = PathFacts{};
  bool else_exits = false;
  if (s.body.size() > 1) {
    const int jmp = then_exits ? -1 : emit(Program::JMP, 0);
    patch_jumps(to_else);
    else_exits = compile_stmt(s.body[1]) == Path::Ended;
    if (jmp >= 0) p.code[(size_t)jmp].dst = (int)p.code.size();
  } else {
    patch_jumps(to_else);
  }
  unassigned.path = join(after_then, unassigned.path);
  join_entered(tracked_before);
  --branch_depth;
  return then_exits && else_exits ? Path::Ended : Path::Open;
}

ProgramCompiler::Path ProgramCompiler::compile_stmt(const mir::Stmt& s) {
  const bool assigns = s.kind == mir::Stmt::Assignment;
  if (assigns && !s.lhs_idx.empty()) require_assigned(s.lhs);
  Path path;
  try {
    path = compile_stmt_folded(s);
  } catch (const std::domain_error& error) {
    Program::Message m;
    m.spec.chunks = {error.what()};
    m.spec.error = MessageError::Domain;
    p.messages.push_back(std::move(m));
    emit(Program::REJECT, 0, (int)p.messages.size() - 1);
    return Path::Open;
  }
  if (assigns && s.lhs_idx.empty()) {
    note_assigned(s.lhs);
    // A folded integer's branches fold too; only a register needs a flag.
    if (s.rhs.kind == mir::Expr::LitInt && !given(s) && reals.count(s.lhs))
      unassigned.path.set_flag(s.lhs, s.rhs.lit_i);
    else
      unassigned.path.erase_flag(s.lhs);
  }
  return path;
}

ProgramCompiler::Path ProgramCompiler::compile_stmt_folded(const mir::Stmt& s) {
  switch (s.kind) {
    case mir::Stmt::Decl: {
      // A declaration shadows every compile-time fact retained for an
      // earlier optimized symbol with the same name.
      // It also shadows either runtime representation: --O1 may reuse one
      // symbol id for a scalar int in one block and an array/container in
      // the next, and the later assignment must not see the old binding.
      reals.erase(s.decl_id);
      ints.erase(s.decl_id);
      known_reals.erase(s.decl_id);
      deferred_shapes.erase(s.decl_id);
      region_sized.erase(s.decl_id);
      forget_local(s.decl_id);
      known_int_arrays.erase(s.decl_id);
      known_int_array_dims.erase(s.decl_id);
      int_array_names.erase(s.decl_id);
      int_decl_at.erase(s.decl_id);
      int_reset_at.erase(s.decl_id);
      if (s.decl_type.base.empty() &&
          s.decl_type.unsized.leaf != mir::UnsizedLeaf::Unknown) {
        const mir::UnsizedView view = s.decl_type.unsized;
        if (view.leaf == mir::UnsizedLeaf::Complex)
          bail("complex unsized declaration " + s.decl_id);
        // Scalar Unsized declarations are normalized to SInt/SReal by the
        // reader.  Refuse an unnormalized scalar here rather than treating
        // its fixed width as a deferred container shape.
        if (view.depth == 0 && view.leaf != mir::UnsizedLeaf::Vector &&
            view.leaf != mir::UnsizedLeaf::RowVector &&
            view.leaf != mir::UnsizedLeaf::Matrix)
          bail("scalar unsized declaration " + s.decl_id);

        if (view.depth != 0 && view.leaf == mir::UnsizedLeaf::Int) {
          int_array_names.insert(s.decl_id);
          int_decl_at[s.decl_id] = {branch_depth, loops.size()};
        }
        if (!s.has_init) {
          if (runtime_for_depth)
            bail("runtime for needs a sized container declaration");
          reals[s.decl_id] = Range{};
          deferred_shapes[s.decl_id] = view;
          note_empty(s.decl_id);
          return Path::Open;
        }

        const Range v = expr(s.init);
        if (!unsized_accepts(view, v))
          bail("declaration logical view mismatch for " + s.decl_id);
        const double fill =
            int_array_names.count(s.decl_id)
                ? static_cast<double>(std::numeric_limits<int>::min())
                : std::numeric_limits<double>::quiet_NaN();
        const Range d = declare(s.decl_id, v.len, v, fill);
        for (int k = 0; k < v.len; ++k)
          emit(Program::MOV, d.reg + k, v.reg + k);
        if (int_array_names.count(s.decl_id)) {
          std::vector<long> values;
          if (try_cints(s.init, &values) && values.size() == (size_t)v.len) {
            known_int_arrays[s.decl_id] = std::move(values);
            known_int_array_dims[s.decl_id] = v.dims;
          }
        }
        return Path::Open;
      }
      if (s.decl_type.base.empty())
        bail("declaration has no sized or unsized logical view: " + s.decl_id);
      if (s.decl_type.base == "SInt") {
        int_decl_at[s.decl_id] = {branch_depth, loops.size()};
        if (runtime_for_depth) {
          const Range d =
              declare(s.decl_id, 1, Range{},
                      static_cast<double>(std::numeric_limits<int>::min()));
          if (s.has_init) {
            const Range v = expr(s.init);
            if (!is_scalar(v)) bail("integer declaration is not scalar");
            emit(Program::MOV, d.reg, v.reg);
          }
          return Path::Open;
        }
        if (s.has_init) {
          long folded;
          if (try_cint(s.init, &folded)) {
            ints[s.decl_id] = {folded};
            mark_int_reset(s.decl_id);
            return Path::Open;
          }
          // A declaration after or inside a structured loop may read
          // loop-carried state (for example `int any = hits != 0`). Only
          // that genuinely runtime initializer needs a register. Keeping
          // foldable locals as ints is essential for foreach indices used
          // in later matrix subscripts inside ctsem's integration loop.
          Range view;
          const Range d =
              declare(s.decl_id, 1, view,
                      static_cast<double>(std::numeric_limits<int>::min()));
          const Range v = expr(s.init);
          if (!is_scalar(v)) bail("integer declaration is not scalar");
          emit(Program::MOV, d.reg, v.reg);
          return Path::Open;
        }
        ints[s.decl_id] = {std::numeric_limits<int>::min()};
        mark_int_reset(s.decl_id);
        return Path::Open;
      }
      const bool int_array =
          s.decl_type.base == "SArray" && s.decl_type.elem_base == "SInt";
      if (int_array) {
        int_array_names.insert(s.decl_id);
        int_decl_at[s.decl_id] = {branch_depth, loops.size()};
      }
      if (s.has_init) {
        const Range v = expr(s.init);
        const int want = (int)sized_len(s.decl_type);
        if (v.len != want) bail("declaration width mismatch for " + s.decl_id);
        Range expected;
        expected.len = want;
        expected = declared(expected, s.decl_type);
        if (!same_view(v, expected))
          bail("declaration logical view mismatch for " + s.decl_id);
        const Range d = declare(s.decl_id, want, expected);
        for (int k = 0; k < want; ++k) emit(Program::MOV, d.reg + k, v.reg + k);
        if (int_array) {
          std::vector<long> values;
          if (try_cints(s.init, &values) && values.size() == (size_t)want) {
            known_int_arrays[s.decl_id] = std::move(values);
            known_int_array_dims[s.decl_id] = expected.dims;
          }
        }
      } else {
        Range view;
        const double fill =
            s.decl_type.base == "SArray" && s.decl_type.elem_base == "SInt"
                ? static_cast<double>(std::numeric_limits<int>::min())
                : std::numeric_limits<double>::quiet_NaN();
        const Range expected = declared(view, s.decl_type);
        declare(s.decl_id, (int)sized_len(s.decl_type), expected, fill);
        if (int_array) {
          known_int_arrays[s.decl_id] = std::vector<long>(
              (size_t)sized_len(s.decl_type), std::numeric_limits<int>::min());
          known_int_array_dims[s.decl_id] = expected.dims;
        }
      }
      return Path::Open;
    }
    case mir::Stmt::Assignment: {
      if (accumulator && s.lhs == accumulator->name) {
        if (given(s) || !s.lhs_idx.empty() || s.rhs.name != "Plus__" ||
            s.rhs.args.size() != 2 || s.rhs.args[0].name != s.lhs)
          bail("accumulator " + s.lhs + " is not only added to");
        const Range v = expr(s.rhs.args[1]);
        for (int k = 0; k < v.len; ++k)
          emit(Program::ADD, accumulator->reg, accumulator->reg, v.reg + k);
        return Path::Open;
      }
      if (deferred_shapes.count(s.lhs)) {
        if (!s.lhs_idx.empty())
          bail("indexed assignment before unsized shape adoption for " + s.lhs);
        // Evaluating the value can inline a call, which replaces these
        // maps and leaves any iterator into them dangling.
        const Range v = assignment_value(s);
        auto deferred = deferred_shapes.find(s.lhs);
        if (!unsized_accepts(deferred->second, v))
          bail("assignment logical view mismatch for " + s.lhs);
        auto binding = reals.find(s.lhs);
        if (binding == reals.end()) bail("assignment to undeclared " + s.lhs);

        Range adopted = v;
        adopted.reg = alloc(v.len);
        // The assignment may be under a runtime jump.  Fill the adopted
        // run before control flow starts so backward replay and an untaken
        // path never observe uninitialized register cells.
        if (v.len != 0) late_bound.emplace_back(adopted.reg, v.len);
        for (int k = 0; k < v.len; ++k)
          emit(Program::MOV, adopted.reg + k, v.reg + k);
        binding->second = adopted;
        deferred_shapes.erase(deferred);
        region_sized.insert(s.lhs);

        if (int_array_names.count(s.lhs)) {
          std::vector<long> values;
          if (fold_is_certain(s.lhs) && !given(s) &&
              try_cints(s.rhs, &values) && values.size() == (size_t)v.len) {
            known_int_arrays[s.lhs] = std::move(values);
            known_int_array_dims[s.lhs] = adopted.dims;
          } else {
            known_int_arrays.erase(s.lhs);
            known_int_array_dims.erase(s.lhs);
          }
        }
        return Path::Open;
      }
      if (ints.count(s.lhs) && s.lhs_idx.empty()) {
        long ignored;
        // A conditional write must preserve the old value on the untaken
        // path, so it cannot be folded into the single compile-time copy.
        // A runtime integer can also be assigned before its first loop
        // (for example a bound selected by a comparison of real inputs).
        // Reify any nonconstant scalar assignment; expr still decides
        // whether its integer operations have a supported implementation.
        const bool certain = fold_is_certain(s.lhs);
        const bool constant = certain && !given(s) && try_cint(s.rhs, &ignored);
        if (!certain || !constant) {
          if (certain && !constant && !structured_while_seen && !in_write_array)
            require_runtime_integer_contract();
          reify_written_int(s.lhs);
        }
      }
      if (ints.count(s.lhs) && s.lhs_idx.empty()) {
        // This assignment is certain and its RHS stayed a compile-time
        // integer, so subsequent reads may use the folded value directly.
        ints[s.lhs] = {cint(s.rhs)};
        mark_int_reset(s.lhs);
        return Path::Open;
      }
      auto it = reals.find(s.lhs);
      if (it == reals.end()) {
        // Assigning to a name the region did not declare: it lives
        // outside, so bind it (its current value is a live-in -- the
        // untaken branch has to leave it alone) and assign into those
        // registers.
        Range ext;
        if (bind_outer(s.lhs, &ext)) {
          reals[s.lhs] = ext;
          extern_bound.insert(s.lhs);
          it = reals.find(s.lhs);
        }
      }
      if (it == reals.end() && s.lhs_idx.empty()) {
        // First touch of this name from here, and nothing to import as a
        // live-in either: a local whose only other write sat in a branch
        // the surrounding graph lowering folded away as unreachable, so
        // it never got a slot to hand this region. A full-variable write
        // with no prior binding is the zero-size "adopt the assigned
        // shape" case below, minus the placeholder declaration -- give it
        // one now.
        reals[s.lhs] = Range{};
        note_entered_empty(s.lhs);
        it = reals.find(s.lhs);
      }
      if (it == reals.end()) bail("assignment to undeclared " + s.lhs);
      const Range dst = it->second;
      const Range v = s.lhs_idx.empty()
                          ? assignment_value(s)
                          : copied_if_overlapping(assignment_value(s), dst);
      // Once written here, the imported value can no longer be answered
      // from the enclosing graph's preparation-time observation.
      extern_bound.erase(s.lhs);
      if (s.lhs_idx.empty()) {
        std::vector<long> folded_ints;
        const bool have_folded_ints = int_array_names.count(s.lhs) &&
                                      fold_is_certain(s.lhs) && !given(s) &&
                                      try_cints(s.rhs, &folded_ints) &&
                                      folded_ints.size() == (size_t)v.len;
        if (dst.len == 0 && (v.len != 0 || !same_view(v, dst))) {
          refuse_second_size(s.lhs);
          // stan::model::assign gives a zero-size destination the
          // right-hand side's size, so adopt the assigned shape. That
          // includes an empty value of another shape, such as a 0x0
          // matrix assigned to a name first written in this region.
          //
          // The assignment can sit inside a data-dependent
          // branch -- `cond ? udf(x) : y` inlines to an assignment under
          // `if (cond)` -- and then the arm that does not run leaves
          // these registers unwritten. They are the variable's live-out,
          // so the harvest reads them anyway: under the backward's var
          // replay that is a null (or a previous call's, already
          // recovered) vari. finish() fills them with NaN, which is what
          // Stan holds in a value it never computed.
          Range nd = v;
          nd.reg = alloc(v.len);
          late_bound.emplace_back(nd.reg, v.len);
          for (int k = 0; k < v.len; ++k)
            emit(Program::MOV, nd.reg + k, v.reg + k);
          reals[s.lhs] = nd;
          region_sized.insert(s.lhs);
          if (have_folded_ints) {
            known_int_arrays[s.lhs] = std::move(folded_ints);
            known_int_array_dims[s.lhs] = nd.dims;
          } else if (int_array_names.count(s.lhs)) {
            known_int_arrays.erase(s.lhs);
            known_int_array_dims.erase(s.lhs);
          }
          return Path::Open;
        }
        if (v.len != dst.len || !same_view(v, dst)) {
          const std::string message = whole_assign_size_message(s.lhs, dst, v);
          if (message.empty())
            bail("assignment logical view mismatch for " + s.lhs);
          emit_assign_size_reject(s.lhs, message);
          return Path::Open;
        }
        for (int k = 0; k < v.len; ++k)
          emit(Program::MOV, dst.reg + k, v.reg + k);
        if (have_folded_ints) {
          known_int_arrays[s.lhs] = std::move(folded_ints);
          known_int_array_dims[s.lhs] = dst.dims;
        } else if (int_array_names.count(s.lhs)) {
          known_int_arrays.erase(s.lhs);
          known_int_array_dims.erase(s.lhs);
        }
        return Path::Open;
      }
      // A single All is the complete destination view. This is reachable
      // in ODE functions and runtime-control statement islands, so it
      // must agree with both graph lowering and MirInterp rather than
      // forcing an otherwise supported region onto the fallback path.
      if (s.lhs_idx.size() == 1 && s.lhs_idx[0].kind == mir::Expr::IndexAll) {
        if (dst.kind == ViewKind::Flat)
          bail("full-span assignment needs a container for " + s.lhs);
        if (v.len != dst.len || !same_view(v, dst)) {
          const std::string message = whole_assign_size_message(s.lhs, dst, v);
          if (message.empty())
            bail("full-span assignment logical view mismatch for " + s.lhs);
          emit_assign_size_reject(s.lhs, message);
          return Path::Open;
        }
        for (int k = 0; k < v.len; ++k)
          emit(Program::MOV, dst.reg + k, v.reg + k);
        if (int_array_names.count(s.lhs)) {
          std::vector<long> values;
          if (fold_is_certain(s.lhs) && !given(s) &&
              try_cints(s.rhs, &values) && values.size() == (size_t)v.len) {
            known_int_arrays[s.lhs] = std::move(values);
            known_int_array_dims[s.lhs] = dst.dims;
          } else {
            known_int_arrays.erase(s.lhs);
            known_int_array_dims.erase(s.lhs);
          }
        }
        return Path::Open;
      }
      if ((dst.kind == ViewKind::Array || dst.kind == ViewKind::Matrix ||
           (s.lhs_idx.size() == 1 &&
            s.lhs_idx[0].kind == mir::Expr::IndexMulti)) &&
          has_runtime_selector(s.lhs_idx)) {
        const bool scalar_array = dst.kind == ViewKind::Array &&
                                  dst.leaf == ViewKind::Flat &&
                                  (dst.dims.empty() || dst.dims.size() == 1) &&
                                  s.lhs_idx.size() == 1 &&
                                  s.lhs_idx[0].kind == mir::Expr::IndexSingle;
        if (scalar_array && is_scalar(v)) {
          const Range index = expr(s.lhs_idx[0].args.at(0));
          if (!is_scalar(index))
            bail("runtime assignment index must be scalar");
          emit(Program::DYN_SET, dst.reg, dst.reg, v.reg, index.reg, dst.len);
        } else {
          const Range updated = dynamic_index(dst, s.lhs_idx, &v);
          emit(Program::MOVR, dst.reg, updated.reg, 0, 0, dst.len);
        }
        known_int_arrays.erase(s.lhs);
        known_int_array_dims.erase(s.lhs);
        return Path::Open;
      }
      if (dst.kind == ViewKind::Matrix && s.lhs_idx.size() == 2) {
        const std::vector<int64_t> rows = matrix_positions(
            s.lhs_idx[0], dst.rows, "assignment row of " + s.lhs,
            SelectorSite::MatrixRow, true, true);
        const std::vector<int64_t> cols = matrix_positions(
            s.lhs_idx[1], dst.cols, "assignment column of " + s.lhs,
            SelectorSite::MatrixColumn, true, true);
        if (!rows.empty() && cols.size() > (size_t)kMaxRegs / rows.size())
          bail("matrix assignment selection is too large");
        const size_t width = rows.size() * cols.size();
        if (v.len != static_cast<int>(width)) {
          emit_assign_size_reject(
              s.lhs, "matrix assignment width mismatch for " + s.lhs + ": " +
                         std::to_string(width) + " selected, " +
                         std::to_string(v.len) + " assigned");
          return Path::Open;
        }
        int at = 0;
        for (int64_t j : cols)
          for (int64_t i : rows)
            emit(Program::MOV, dst.reg + (int)(j * dst.rows + i), v.reg + at++);
        return Path::Open;
      }
      if (dst.kind == ViewKind::Array) {
        const std::vector<int64_t> dims =
            dst.dims.empty() ? std::vector<int64_t>{dst.len} : dst.dims;
        if (s.lhs_idx.size() > dims.size())
          bail("too many assignment indices for " + s.lhs);
        std::vector<std::vector<int64_t>> positions;
        positions.reserve(dims.size());
        for (size_t d = 0; d < dims.size(); ++d) {
          if (d < s.lhs_idx.size()) {
            positions.push_back(matrix_positions(
                s.lhs_idx[d], dims[d], "assignment array of " + s.lhs,
                selector_site(d, dims.size() - leaf_rank(dst.leaf),
                              dst.leaf == ViewKind::Matrix),
                true, true));
          } else {
            if (dims[d] < 0 || dims[d] > kMaxRegs)
              bail("array assignment has an invalid omitted extent");
            positions.emplace_back();
            positions.back().reserve((size_t)dims[d]);
            for (int64_t k = 0; k < dims[d]; ++k) positions.back().push_back(k);
          }
        }
        size_t width = 1;
        for (const auto& axis : positions) {
          if (!axis.empty() && width > (size_t)kMaxRegs / axis.size())
            bail("array assignment selection is too large");
          width *= axis.size();
        }
        if (v.len != (int)width) {
          const int64_t got =
              v.kind == ViewKind::Array && !v.dims.empty() ? v.dims[0] : v.len;
          const bool leading_differs =
              s.lhs_idx[0].kind != mir::Expr::IndexSingle &&
              (int64_t)positions[0].size() != got;
          emit_assign_size_reject(
              s.lhs, leading_differs
                         ? selected_assign_size_message(
                               s.lhs, s.lhs_idx[0], true,
                               (int64_t)positions[0].size(), got)
                         : "array assignment width mismatch for " + s.lhs +
                               ": " + std::to_string(width) + " selected, " +
                               std::to_string(v.len) + " assigned");
          return Path::Open;
        }
        // The same shared index geometry the rvalue reads resolve
        // through; destination cells and the assigned run pair up in the
        // identical outer-major enumeration.
        const size_t leaf_axes =
            dst.leaf == ViewKind::Matrix ? 2
            : dst.leaf == ViewKind::Vector || dst.leaf == ViewKind::RowVector
                ? 1
                : 0;
        const BuiltinIndexMap store_map =
            builtin_index_map(dims, leaf_axes, positions,
                              std::vector<bool>(positions.size(), false),
                              SliceStorageOrder::OuterMajor);
        std::vector<int64_t> offsets;
        offsets.reserve((size_t)store_map.count);
        for (int64_t k = 0; k < store_map.count; ++k)
          offsets.push_back(store_map.kind == BuiltinSliceMap::Kind::Contiguous
                                ? store_map.offset + k
                            : store_map.kind == BuiltinSliceMap::Kind::Strided
                                ? store_map.offset + k * store_map.stride
                                : store_map.gather[(size_t)k]);
        for (size_t k = 0; k < offsets.size(); ++k)
          emit(Program::MOV, dst.reg + (int)offsets[k], v.reg + (int)k);
        if (int_array_names.count(s.lhs)) {
          std::vector<long> values;
          auto known = known_int_arrays.find(s.lhs);
          bool folded = false;
          if (offsets.size() == 1) {
            long value = 0;
            if (!given(s) && try_cint(s.rhs, &value)) {
              values = {value};
              folded = true;
            }
          } else {
            folded = !given(s) && try_cints(s.rhs, &values);
          }
          if (fold_is_certain(s.lhs) && folded &&
              values.size() == offsets.size() &&
              known != known_int_arrays.end()) {
            const std::vector<int64_t> known_offsets =
                first_fast_array_offsets(dims, positions);
            for (size_t k = 0; k < known_offsets.size(); ++k)
              known->second[(size_t)known_offsets[k]] = values[k];
          } else {
            known_int_arrays.erase(s.lhs);
            known_int_array_dims.erase(s.lhs);
          }
        }
        return Path::Open;
      }
      if ((dst.kind == ViewKind::Vector || dst.kind == ViewKind::RowVector ||
           dst.kind == ViewKind::Flat) &&
          s.lhs_idx.size() == 1) {
        long fixed_index;
        if (s.lhs_idx[0].kind == mir::Expr::IndexSingle &&
            s.lhs_idx[0].args.size() == 1 &&
            !try_cint(s.lhs_idx[0].args[0], &fixed_index)) {
          if (!is_scalar(v)) bail("element assignment from a container");
          const Range index = expr(s.lhs_idx[0].args[0]);
          if (!is_scalar(index)) bail("non-scalar assignment index");
          // Reading and writing the full range in the opcode metadata
          // preserves untouched cells through register compaction. Replay
          // records the selected write, including an aliased RHS, normally.
          emit(Program::DYN_SET, dst.reg, dst.reg, v.reg, index.reg, dst.len);
          known_int_arrays.erase(s.lhs);
          return Path::Open;
        }
        const std::vector<int64_t> positions =
            matrix_positions(s.lhs_idx[0], dst.len, "assignment vector",
                             dst.kind == ViewKind::Flat ? SelectorSite::Array
                                                        : SelectorSite::Vector,
                             true, true);
        if (v.len != static_cast<int>(positions.size())) {
          emit_assign_size_reject(
              s.lhs, selected_assign_size_message(
                         s.lhs, s.lhs_idx[0], dst.kind == ViewKind::Flat,
                         (int64_t)positions.size(), v.len));
          return Path::Open;
        }
        for (size_t k = 0; k < positions.size(); ++k)
          emit(Program::MOV, dst.reg + (int)positions[k], v.reg + (int)k);
        return Path::Open;
      }
      for (const auto& ix : s.lhs_idx)
        if (ix.kind != mir::Expr::IndexSingle)
          bail("assignment index form for " + s.lhs);
      if (!is_scalar(v)) bail("element assignment from a container");
      int64_t flat = 0;
      if (dst.kind == ViewKind::Matrix) {
        if (s.lhs_idx.size() != 2)
          bail("matrix assignment index form for " + s.lhs);
        const long i = cint(s.lhs_idx[0].args[0]);
        const long j = cint(s.lhs_idx[1].args[0]);
        if (i < 1 || i > dst.rows || j < 1 || j > dst.cols)
          bail("assignment index range for " + s.lhs);
        flat = (j - 1) * dst.rows + i - 1;
      } else {
        if (s.lhs_idx.size() != 1) bail("assignment index form for " + s.lhs);
        const long ix = cint(s.lhs_idx[0].args[0]);
        if (ix < 1 || ix > dst.len) bail("assignment index range for " + s.lhs);
        flat = ix - 1;
      }
      emit(Program::MOV, dst.reg + (int)flat, v.reg);
      if (int_array_names.count(s.lhs)) {
        long value = 0;
        auto known = known_int_arrays.find(s.lhs);
        if (fold_is_certain(s.lhs) && !given(s) && try_cint(s.rhs, &value) &&
            known != known_int_arrays.end() && flat >= 0 &&
            (size_t)flat < known->second.size())
          known->second[(size_t)flat] = value;
        else
          known_int_arrays.erase(s.lhs);
      }
      return Path::Open;
    }
    case mir::Stmt::Return: {
      bool runtime_exit =
          branch_depth || structured_while_depth > return_while_base;
      // A pending break or continue can bypass an apparently top-level
      // return in an unrolled loop. Finish and patch that loop before the
      // function's exit, rather than unwinding compilation immediately.
      if (return_frame)
        for (size_t i = return_frame->loop_base; i < loops.size(); ++i)
          runtime_exit = runtime_exit || !loops[i].breaks.empty() ||
                         !loops[i].continues.empty();
      if (!s.fn_args.empty()) return compile_multi_return(s, runtime_exit);
      if (return_frame && (runtime_exit || return_frame->has_result)) {
        emit_function_return(s.has_init ? expr(s.rhs) : Range{0, 0});
        return Path::Ended;
      }
      if (!return_frame) bail("return without a function scope");
      return_frame->result = s.has_init ? expr(s.rhs) : Range{0, 0};
      return Path::Returned;
    }
    case mir::Stmt::Break:
      if (loops.empty()) bail("break outside a loop");
      if (branch_depth > loops.back().branch_depth || loops.back().structured) {
        loops.back().breaks.push_back(emit(Program::JMP, 0));
        jump_with_facts(&loops.back().at_break);
        return Path::Open;
      }
      return Path::Break;
    case mir::Stmt::Continue:
      if (loops.empty()) bail("continue outside a loop");
      if (branch_depth > loops.back().branch_depth || loops.back().structured) {
        loops.back().continues.push_back(emit(Program::JMP, 0));
        jump_with_facts(&loops.back().at_continue);
        return Path::Open;
      }
      return Path::Continue;
    case mir::Stmt::For: {
      known_reals.erase(s.loopvar);
      long lo, hi;
      std::set<std::string> written;
      for (const auto& child : s.body) assigned_names(child, &written);
      const bool invariant_upper = invariant_for_bound(s.upper, written);
      if (!invariant_upper || !try_cint(s.lower, &lo) ||
          !try_cint(s.upper, &hi)) {
        // Every integer operation in the repeated body must have a typed
        // implementation; double storage alone does not prove this.
        require_runtime_integer_contract();
        // Evaluate and snapshot the lower bound exactly once.
        // In particular, the lower bound may be a variable assigned by the
        // body, so it cannot remain an aliased Range.
        const Range lower = expr(s.lower);
        if (!is_scalar(lower)) bail("for lower bound is not scalar");
        const int index = alloc(1);
        emit(Program::MOV, index, lower.reg);
        for (const auto& name : written) reify_loop_write(name);
        ints.erase(s.loopvar);
        reals[s.loopvar] = Range{index, 1};
        const int one = konst(1);
        const int condition = alloc(1);
        // The bound runs again on every trip, so a local of a function it
        // calls has to be reset there like a local of the body.
        structured_while_seen = true;
        ++structured_while_depth;
        const int head = (int)p.code.size();
        const Range upper = expr(s.upper);
        if (!is_scalar(upper)) bail("for upper bound is not scalar");
        emit(Program::LE, condition, index, upper.reg);
        const int empty = emit(Program::JZ, 0, condition);
        const int body_head = (int)p.code.size();
        loops.push_back({});
        loops.back().structured = true;
        loops.back().assigned = written;
        ++runtime_for_depth;
        forget_flags(written, s.loopvar);
        start_loop_facts();
        // Other paths may still continue or break; patch them below.
        (void)compile_body(s.body);
        join_loop_exits();
        --runtime_for_depth;
        --structured_while_depth;
        for (int jump : loops.back().continues)
          p.code[(size_t)jump].dst = (int)p.code.size();
        // The private double counter represents INT_MAX + 1 exactly; no
        // out-of-range loop variable reaches a body with an int upper bound.
        // Proven invariant bounds keep the cheaper counted-loop backedge.
        int done = -1;
        if (invariant_upper) {
          emit(Program::LT, condition, index, upper.reg);
          done = emit(Program::JZ, 0, condition);
        }
        emit(Program::ADD, index, index, one);
        emit(Program::JMP, invariant_upper ? body_head : head);
        const int end = (int)p.code.size();
        p.code[(size_t)empty].dst = end;
        if (done >= 0) p.code[(size_t)done].dst = end;
        for (int jump : loops.back().breaks) p.code[(size_t)jump].dst = end;
        loops.pop_back();
        reals.erase(s.loopvar);
        int_decl_at.erase(s.loopvar);
        return Path::Open;
      }
      loops.push_back({});
      loops.back().branch_depth = branch_depth;
      bool broken = false;
      bool returned = false;
      bool returned_directly = false;
      for (int64_t v = lo; v <= hi; ++v) {
        Path path = Path::Open;
        ints[s.loopvar] = {static_cast<long>(v)};
        int_decl_at[s.loopvar] = {branch_depth, loops.size()};
        path = compile_body(s.body);
        if (path == Path::Continue) {
          path = Path::Open;
        } else if (path == Path::Break) {
          path = Path::Open;
          // A runtime continue earlier in this trip skips the break, so
          // the later trips stay reachable and the break becomes a jump.
          if (loops.back().continues.empty())
            broken = true;
          else
            loops.back().breaks.push_back(emit(Program::JMP, 0));
        }
        if (path == Path::Returned) {
          returned_directly = true;
          break;
        }
        const bool path_returned = path == Path::Ended;
        // A continue can reach another unrolled trip; a break reaches the
        // statements after the loop. Only an all-return iteration ends
        // the enclosing lexical path.
        if (path_returned && loops.back().continues.empty()) {
          returned = loops.back().breaks.empty();
          break;
        }
        for (int jump : loops.back().continues)
          p.code[(size_t)jump].dst = (int)p.code.size();
        loops.back().continues.clear();
        join_jumps(&loops.back().at_continue);
        if (broken) break;
      }
      ints.erase(s.loopvar);
      int_decl_at.erase(s.loopvar);
      for (int jump : loops.back().breaks)
        p.code[(size_t)jump].dst = (int)p.code.size();
      join_jumps(&loops.back().at_break);
      loops.pop_back();
      if (returned_directly) return Path::Returned;
      return returned ? Path::Ended : Path::Open;
    }
    case mir::Stmt::While: {
      // The register program already has JZ/JMP and is replayed under
      // autodiff for an island.  Use those instructions directly so a
      // finite data loop and a parameter-sensitive recurrence have their
      // actual trip count, with no arbitrary lowering-time cap.
      std::set<std::string> written;
      for (const auto& child : s.body) assigned_names(child, &written);
      for (const std::string& name : written) reify_loop_write(name);

      // The condition runs again on every trip, like the body.
      structured_while_seen = true;
      ++structured_while_depth;
      const int head = (int)p.code.size();
      std::vector<int> exits;
      cond_jump(s.cond, false, &exits);
      loops.push_back({});
      loops.back().structured = true;
      loops.back().assigned = written;
      forget_flags(written, "");
      start_loop_facts();
      const bool body_returned = compile_body(s.body) == Path::Ended;
      join_loop_exits();
      --structured_while_depth;
      for (int jump : loops.back().continues) p.code[(size_t)jump].dst = head;
      if (!body_returned) emit(Program::JMP, head);
      patch_jumps(exits);
      for (int jump : loops.back().breaks)
        p.code[(size_t)jump].dst = (int)p.code.size();
      loops.pop_back();
      return Path::Open;
    }
    case mir::Stmt::IfElse: {
      long c;
      if (try_cint(s.cond, &c)) {
        if (c != 0 && !s.body.empty()) return compile_stmt(s.body[0]);
        if (c == 0 && s.body.size() > 1) return compile_stmt(s.body[1]);
        return Path::Open;
      }
      return compile_runtime_if(s);
    }
    case mir::Stmt::Block:
    case mir::Stmt::SList:
      return compile_body(s.body);
    case mir::Stmt::TargetPE: {
      // The region's own running total. The caller decides what it is
      // (the ODE side never sees one; lowering makes it a target term),
      // so all this does is accumulate.
      if (target_reg < 0) bail("target += is not available in this region");
      const Range v = expr(s.target);
      // `target += e` for a container adds `sum(e)`. Accumulating the
      // elements in ascending order is that sum, and it is the order
      // OP_SUM_VEC uses on the graph side, so the two paths agree to the
      // bit. A scalar is the one-element case of the same loop.
      for (int k = 0; k < v.len; ++k)
        emit(Program::ADD, target_reg, target_reg, v.reg + k);
      return Path::Open;
    }
    case mir::Stmt::NRFunApp:
      if (mir::internal_name(s) == "FnValidateSize") return Path::Open;
      if (const auto action = message_action(mir::internal_name(s))) {
        Program::Message message;
        std::vector<MessageShape> shapes;
        message.spec = lower_message_arguments(
            s.fn_args, s.print_layout, [&](const mir::Expr& argument) {
              const Range value = expr(argument);
              message.value_reg.push_back(value.reg);
              message.value_len.push_back(value.len);
              MessageShape shape;
              shape.integer = message_integer(argument);
              if (value.kind == ViewKind::Array) {
                shape.dims = value.dims;
                shape.matrix_leaf = value.leaf == ViewKind::Matrix;
              } else if (value.kind == ViewKind::Matrix) {
                shape.dims = {value.rows, value.cols};
                shape.matrix_leaf = true;
              } else if (!message_scalar(argument)) {
                shape.dims = {value.len};
              }
              shapes.push_back(std::move(shape));
            });
        message.spec.error = message_error(*action);
        message.spec.shapes = std::move(shapes);
        p.messages.push_back(std::move(message));
        emit(*action != MessageAction::Print ? Program::REJECT : Program::PRINT,
             0, (int)p.messages.size() - 1);
        return Path::Open;
      }
      if (const auto call = mir::user_defined_statement_call(s)) {
        auto it = funs.find(mir::function_key(*call));
        if (it == funs.end()) bail("unknown function " + call->name);
        (void)inline_call(*it->second, inline_arguments(call->args), true,
                          propto_ctx && mir::inherits_propto(*call));
        return Path::Open;
      }
      bail("statement function " + s.fn_name + " requires the MIR interpreter");
    case mir::Stmt::MultiAssign:
      return compile_multi_assign(s);
    case mir::Stmt::Skip:
      return Path::Open;
    default:
      bail("statement");
  }
}

}  // namespace stanli

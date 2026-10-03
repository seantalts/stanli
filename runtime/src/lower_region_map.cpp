#include <stanli/region_map.hpp>

#include "lower_internal.hpp"

namespace stanli {
namespace lower_detail {

namespace {

bool body_escapes(const mir::Stmt& s, bool in_inner_loop) {
  if (s.kind == mir::Stmt::Return) return true;
  if ((s.kind == mir::Stmt::Break || s.kind == mir::Stmt::Continue) &&
      !in_inner_loop)
    return true;
  const bool inner =
      in_inner_loop || s.kind == mir::Stmt::For || s.kind == mir::Stmt::While;
  for (const auto& child : s.body)
    if (body_escapes(child, inner)) return true;
  return false;
}

constexpr size_t kMaxRegionMapCode = 4096;
constexpr int64_t kMaxRegionMapSavedCells = int64_t{1} << 24;

template <typename F>
void each_written_span(const Program& p, const Program::Instr& I, F fn) {
  if (I.code == Program::CALL) {
    const Program::Call& call = p.calls[(size_t)I.a];
    fn(call.out, call.out_len);
    fn(call.scratch, call.scratch_len);
    return;
  }
  if (I.code == Program::TRANSFORM) {
    const Program::Transform& tr = p.transforms[(size_t)I.a];
    fn(tr.out, tr.out_len);
    fn(tr.jac, 1);
    return;
  }
  const int len = program_output_len(I);
  if (len > 0) fn(I.dst, len);
}

bool is_constant_store(Program::Code code) {
  return code == Program::CONST || code == Program::CONSTR ||
         code == Program::FILL;
}

void split_prologue(RegionMapProg& p) {
  const size_t n = p.code.size();
  std::vector<int> writes((size_t)p.n_regs, 0);
  for (const auto& I : p.code)
    each_written_span(p, I, [&](int reg, int len) {
      for (int k = 0; k < len; ++k) ++writes[(size_t)(reg + k)];
    });
  std::vector<char> live((size_t)p.n_regs, 0);
  for (const auto& li : p.ins)
    for (int k = 0; k < li.len; ++k) live[(size_t)(li.reg + k)] = 1;

  std::vector<char> hoist(n, 0);
  for (size_t i = 0; i < n; ++i) {
    const Program::Instr& I = p.code[i];
    if (!is_constant_store(I.code)) continue;
    const int len = program_output_len(I);
    bool single = len > 0;
    for (int k = 0; k < len && single; ++k)
      single = writes[(size_t)(I.dst + k)] == 1 && !live[(size_t)(I.dst + k)];
    hoist[i] = single;
  }

  std::vector<int> new_index(n + 1, 0);
  std::vector<Program::Instr> body;
  for (size_t i = 0; i < n; ++i) {
    new_index[i] = static_cast<int>(body.size());
    if (hoist[i])
      p.prologue.push_back(p.code[i]);
    else
      body.push_back(p.code[i]);
  }
  new_index[n] = static_cast<int>(body.size());
  for (auto& I : body)
    if (I.code == Program::JZ || I.code == Program::JMP)
      I.dst = new_index[(size_t)I.dst];
  p.code = std::move(body);

  std::vector<char> written((size_t)p.n_regs, 0);
  for (const auto& I : p.code)
    each_written_span(p, I, [&](int reg, int len) {
      for (int k = 0; k < len; ++k) written[(size_t)(reg + k)] = 1;
    });
  for (int reg = 0; reg < p.n_regs;) {
    if (!written[(size_t)reg]) {
      ++reg;
      continue;
    }
    int end = reg;
    while (end < p.n_regs && written[(size_t)end]) ++end;
    if (!p.saved.empty() &&
        reg - (p.saved.back().first + p.saved.back().second) <=
            kRegionMapSavedGap) {
      p.saved_cells += end - (p.saved.back().first + p.saved.back().second);
      p.saved.back().second = end - p.saved.back().first;
    } else {
      p.saved.emplace_back(reg, end - reg);
      p.saved_cells += end - reg;
    }
    reg = end;
  }
}

}  // namespace

bool Lowering::lower_region_map(const mir::Stmt& s, long lo, long hi) {
  const char* mode = std::getenv("STANLI_REGION_MAP");
  if (mode && std::string_view(mode) == "0") return false;
  if (in_write_array || region_current) return false;
  if (structured_policy == StructuredMode::Prefer ||
      structured_policy == StructuredMode::Force)
    return false;
  const bool diagnostics =
      std::getenv("STANLI_REGION_MAP_DIAGNOSTICS") != nullptr;
  const auto refuse = [&](const std::string& why) {
    if (diagnostics)
      emit_diagnostic("stanli_region_map refused (" + s.loopvar + "): " + why);
    return false;
  };
  if (structured_outer_depth != 1) return false;
  if (hi - lo + 1 < 32) return refuse("trip count below 32");
  if (!region_runtime_control(s))
    return refuse("no parameter-dependent control");
  if (target_scale != 1.0) return refuse("enclosing target scaling");
  if (!has_target_pe(s)) return refuse("body has no target increment");
  if (stmt_effectful(s)) return refuse("body has effects");
  for (const auto& child : s.body)
    if (body_escapes(child, false)) return refuse("break, continue or return");
  {
    std::set<std::string> locals;
    for (const auto& child : s.body) collect_loop_locals(child, &locals);
    std::vector<std::string> writes;
    assigned_names(s, &writes);
    for (const auto& name : writes) {
      if (name == s.loopvar) return refuse("loop variable assigned");
      if (!locals.count(name))
        return refuse("assigns " + name + " declared outside the body");
    }
  }

  auto snapshot = wa_snapshot();
  const auto abandon = [&](const std::string& why) {
    wa_restore(snapshot);
    return refuse(why);
  };

  auto prog = std::make_shared<RegionMapProg>();
  IslandRegion reg;
  ProgramCompiler c{*prog, fun_defs};
  c.checked_region = &s;
  configure_island_compiler(c, *prog, reg, &s);
  try {
    const int target_reg = c.alloc(1);
    const double zero = 0.0;
    c.emit_const(target_reg, &zero, 1);
    c.target_reg = target_reg;
    const int iter = c.alloc(1);
    c.compile_loop_body(s, iter);
    prog->out_regs.push_back(target_reg);
    c.finish();
    prog->ins.push_back(IslandProg::LiveIn{iter, 1, -1, 0, false});
  } catch (Bail& b) {
    return abandon(b.why);
  } catch (const CompileError& e) {
    return abandon(e.what());
  }
  if (reg.in_slots.empty()) return abandon("no live-in values");
  if (island_has_effect(*prog)) return abandon("program has effects");
  finalize_island_program(*prog, true);
  if (!prog->native_adj) {
    std::string why = "no generated adjoint";
    if (diagnostics) {
      std::set<int> codes;
      for (const auto& I : prog->code)
        if (I.code != Program::JZ && I.code != Program::JMP &&
            program_code_spec(I.code).has(kProgramNoAdjoint))
          codes.insert(static_cast<int>(I.code));
      for (int code : codes) why += " program_code=" + std::to_string(code);
      for (const auto& call : prog->calls)
        if (call.backward == nullptr) why += " call_without_backward";
      why += " instructions=" + std::to_string(prog->code.size());
    }
    return abandon(why);
  }
  if (prog->code.size() > kMaxRegionMapCode)
    return abandon("body program too large");
  for (const auto& li : prog->ins)
    if (!li.immutable) return abandon("program writes a live-in register");

  const auto& map = prog->adj.adj_reg;
  const int n_cells = prog->adj.n_regs;
  std::vector<char> persistent((size_t)n_cells, 0);
  for (size_t k = 0; k + 1 < prog->ins.size(); ++k) {
    const auto& li = prog->ins[k];
    for (int i = 0; i < li.len; ++i)
      persistent[(size_t)map[(size_t)(li.reg + i)]] = 1;
  }
  const auto touches_persistent = [&](int dst, int width) {
    for (int k = 0; k < width; ++k)
      if (dst + k >= 0 && dst + k < n_cells && persistent[(size_t)(dst + k)])
        return true;
    return false;
  };
  for (const auto& A : prog->adj.code) {
    if (A.code == Program::CALL) {
      const Program::Call& call = prog->calls[(size_t)A.a];
      if (touches_persistent(call.bwd_adj_out, call.out_len))
        return abandon("adjoint clears a live-in cell");
      continue;
    }
    Program::Instr probe;
    probe.code = A.code;
    probe.len = A.len;
    probe.c = A.c;
    probe.sub = A.sub;
    if (touches_persistent(A.dst, program_output_len(probe)))
      return abandon("adjoint clears a live-in cell");
  }

  prog->iter_reg = prog->ins.back().reg;
  prog->lo = lo;
  prog->count = static_cast<int64_t>(hi) - lo + 1;
  split_prologue(*prog);
  if (prog->count * prog->saved_cells > kMaxRegionMapSavedCells)
    return abandon("per-iteration state too large");
  for (int cell = 0; cell < n_cells;) {
    if (persistent[(size_t)cell]) {
      ++cell;
      continue;
    }
    int end = cell;
    while (end < n_cells && !persistent[(size_t)end]) ++end;
    prog->transient.emplace_back(cell, end - cell);
    cell = end;
  }

  const std::vector<int> inputs = pack_island_inputs(*prog, reg.in_slots);
  Op op;
  op.opcode = OP_REGION_MAP;
  op.n_in = static_cast<int>(inputs.size());
  for (int k = 0; k < op.n_in; ++k) op.in[k] = inputs[(size_t)k];
  op.out = add_slot(1, false);
  op.udata = prog.get();
  g.udata_pool.push_back(prog);
  g.ops.push_back(op);
  push_target_term(op.out);
  int_env.erase(s.loopvar);
  if (diagnostics)
    emit_diagnostic("stanli_region_map selected (" + s.loopvar +
                    "): iterations=" + std::to_string(prog->count) +
                    " prologue=" + std::to_string(prog->prologue.size()) +
                    " body=" + std::to_string(prog->code.size()) +
                    " saved_cells=" + std::to_string(prog->saved_cells) +
                    " saved_spans=" + std::to_string(prog->saved.size()) +
                    " registers=" + std::to_string(prog->n_regs) +
                    " live_ins=" + std::to_string(prog->ins.size()));
  return true;
}

}  // namespace lower_detail
}  // namespace stanli

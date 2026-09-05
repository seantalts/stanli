#include <stanli/graph_print.hpp>

#include <stanli/adjoint.hpp>
#include <stanli/island.hpp>
#include <stanli/optable.hpp>
#include <stanli/program.hpp>
#include <stanli/program_density.hpp>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

namespace stanli {
namespace {

void appendf(std::string& out, const char* fmt, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  const int n = std::vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (n < 0) return;
  if ((size_t)n < sizeof(buf)) {
    out.append(buf, (size_t)n);
    return;
  }
  std::vector<char> big((size_t)n + 1);
  va_start(ap, fmt);
  std::vsnprintf(big.data(), big.size(), fmt, ap);
  va_end(ap);
  out.append(big.data(), (size_t)n);
}

const char* shortname(uint16_t oc) {
  const char* n = opcode_name(oc);
  return n[0] == 'O' && n[1] == 'P' && n[2] == '_' ? n + 3 : n;
}

void append_slot_ref(std::string& out, const Graph& g, int slot) {
  if (slot < 0 || (size_t)slot >= g.slots.size()) {
    appendf(out, "s%d[?]", slot);
    return;
  }
  const Slot& s = g.slots[(size_t)slot];
  appendf(out, "s%d[%lld%s]", slot, (long long)s.len, s.is_param ? ",P" : "");
}

void append_slot_list(std::string& out, const char* label,
                      const std::vector<int>& slots) {
  if (slots.empty()) return;
  appendf(out, "%s:", label);
  for (size_t i = 0; i < slots.size(); ++i) {
    if (i && i % 16 == 0) appendf(out, "\n %s:", label);
    appendf(out, " s%d", slots[i]);
  }
  out += '\n';
}

void print_instr(std::string& out, const Program& p, size_t i,
                 const Program::Instr& I) {
  appendf(out, "  %3zu  %-9s", i, program_code_spec(I.code).name);
  switch (I.code) {
    case Program::CONST:
      appendf(out, " r%d <- pool[%d] (=%g)", I.dst, I.a, p.pool[(size_t)I.a]);
      break;
    case Program::CONSTR:
      appendf(out, " r%d..r%d <- pool[%d..] (=", I.dst, I.dst + I.len - 1, I.a);
      for (int k = 0; k < I.len; ++k)
        appendf(out, "%s%g", k ? "," : "", p.pool[(size_t)(I.a + k)]);
      appendf(out, ")");
      break;
    case Program::JZ:
      appendf(out, " if r%d == 0 goto %d", I.a, I.dst);
      break;
    case Program::JMP:
      appendf(out, " goto %d", I.dst);
      break;
    case Program::MOVR:
      appendf(out, " r%d..r%d <- r%d..r%d", I.dst, I.dst + I.len - 1, I.a,
              I.a + I.len - 1);
      break;
    case Program::DOT:
      appendf(out, " r%d <- dot(r%d.., r%d.., len %d)", I.dst, I.a, I.b, I.len);
      break;
    case Program::DENSITY:
      appendf(out, " r%d <- %s(r%d, r%d, r%d)", I.dst,
              program_density_name(I.len), I.a, I.b, I.c);
      break;
    case Program::CALL: {
      const Program::Call& c = p.calls[(size_t)I.a];
      appendf(out, " %s(", opcode_name(c.opcode));
      for (int k = 0; k < c.n_in; ++k)
        appendf(out, "%sr%d[len %d]", k ? ", " : "", c.in[k], c.in_len[k]);
      appendf(out, ") -> r%d[len %d]", c.out, c.out_len);
      if (c.scratch_len)
        appendf(out, ", scratch r%d[len %d]", c.scratch, c.scratch_len);
      else
        appendf(out, ", no scratch");
      break;
    }
    default:
      appendf(out, " r%d <- r%d", I.dst, I.a);
      const bool ternary = I.code == Program::LOG_MIX || I.code == Program::FMA;
      if ((I.code >= Program::ADD && I.code <= Program::FMIN) ||
          (I.code >= Program::GT && I.code <= Program::NE) ||
          I.code == Program::LSE2 || ternary)
        appendf(out, ", r%d", I.b);
      if (ternary) appendf(out, ", r%d", I.c);
      if (I.len) appendf(out, " (len %d)", I.len);
  }
  out += '\n';
}

void print_island_body(std::string& out, const Graph& g, size_t u, int n) {
  const Op& op = g.ops[u];
  const auto& p = *static_cast<const IslandProg*>(op.udata);
  appendf(out,
          "\n== island %d (graph op %zu): %zu instrs, %d regs, "
          "%zu calls, adjoint %zu instrs, native_adj=%d, softmax3=%d\n",
          n, u, p.code.size(), p.n_regs, p.calls.size(), p.adj.code.size(),
          (int)p.native_adj, (int)(op.variant == kIslandSoftmax3Variant));
  appendf(out, "  live-ins:");
  for (size_t k = 0; k < p.ins.size(); ++k)
    appendf(out, " slot%d->r%d[len %d]", op.in[k], p.ins[k].reg, p.ins[k].len);
  appendf(out, "\n  live-outs (out_regs):");
  for (int r : p.out_regs) appendf(out, " r%d", r);
  appendf(out, "\n\n  FORWARD:\n");
  for (size_t i = 0; i < p.code.size(); ++i) print_instr(out, p, i, p.code[i]);
  if (p.adj.code.empty()) return;
  appendf(out,
          "\n  ADJOINT (reverse order; dst/a/b/c are adjoint cells, "
          "va/vb/vc/vd are value registers):\n");
  for (size_t i = 0; i < p.adj.code.size(); ++i) {
    const AdjInstr& A = p.adj.code[i];
    appendf(out,
            "  %3zu  %-9s dst=%d a=%d b=%d c=%d | va=%d vb=%d vc=%d "
            "vd=%d\n",
            i, program_code_spec(A.code).name, A.dst, A.a, A.b, A.c, A.va, A.vb,
            A.vc, A.vd);
  }
}

}  // namespace

void print_graph(std::string& out, const Graph& g, const GraphPrintInfo& info) {
  appendf(out, "ops=%zu slots=%zu result=s%d\n", g.ops.size(), g.slots.size(),
          g.result_slot);
  append_slot_list(out, "roots", info.roots);
  append_slot_list(out, "target_terms", info.target_terms);
  append_slot_list(out, "jac_slots", info.jac_slots);
  if (!info.views.empty()) {
    std::vector<std::pair<std::string, int>> views = info.views;
    std::sort(views.begin(), views.end());
    appendf(out, "views:");
    for (size_t i = 0; i < views.size(); ++i) {
      if (i && i % 8 == 0) appendf(out, "\nviews:");
      appendf(out, " %s=s%d", views[i].first.c_str(), views[i].second);
    }
    out += '\n';
  }
  if (info.fills && !info.fills->empty()) {
    appendf(out, "fills:");
    for (size_t i = 0; i < info.fills->size(); ++i) {
      if (i && i % 8 == 0) appendf(out, "\nfills:");
      const auto& f = (*info.fills)[i];
      appendf(out, " s%d[%zu]=", f.first, f.second.size());
      for (size_t k = 0; k < f.second.size() && k < 4; ++k)
        appendf(out, "%s%.17g", k ? "," : "", f.second[k]);
      if (f.second.size() > 4) appendf(out, ",...");
    }
    out += '\n';
  }

  int island = 0;
  for (size_t u = 0; u < g.ops.size(); ++u) {
    const Op& op = g.ops[u];
    append_slot_ref(out, g, op.out);
    appendf(out, " = %s", shortname(op.opcode));
    if (op.variant) appendf(out, ".v=0x%02x", op.variant);
    for (int k = 0; k < op.n_in; ++k) {
      out += ' ';
      append_slot_ref(out, g, op.in[k]);
    }
    if (op.out2 != -1) appendf(out, " out2=s%d", op.out2);
    if (op.n_idata) {
      appendf(out, " idata=[");
      for (int64_t k = 0; k < op.n_idata && k < 8; ++k)
        appendf(out, "%s%d", k ? "," : "", op.idata[k]);
      if (op.n_idata > 8) appendf(out, ",...x%lld", (long long)op.n_idata);
      appendf(out, "]");
    }
    out += '\n';
    if (info.print_islands && op.opcode == OP_ISLAND)
      print_island_body(out, g, u, island++);
  }
}

void print_islands(std::string& out, const Graph& g) {
  appendf(out, "graph: %zu ops, %zu slots\n", g.ops.size(), g.slots.size());
  int n = 0;
  for (size_t u = 0; u < g.ops.size(); ++u) {
    if (g.ops[u].opcode != OP_ISLAND) continue;
    print_island_body(out, g, u, n++);
  }
}

}  // namespace stanli

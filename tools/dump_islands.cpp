// Print every island in a compiled model at instruction level: the forward
// program (checkpoint saves included), the CALL payloads, and the generated
// adjoint program. What dump_ops is for the graph, this is for the layer
// below it.
#include <stanli/adjoint.hpp>
#include <stanli/compile.hpp>
#include <stanli/graph.hpp>
#include <stanli/island.hpp>
#include <stanli/optable.hpp>
#include <stanli/program_density.hpp>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

using namespace stanli;

static std::string slurp(const char* p) {
  std::ifstream f(p);
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

static void print_instr(const Program& p, size_t i, const Program::Instr& I) {
  std::printf("  %3zu  %-9s", i, program_code_spec(I.code).name);
  switch (I.code) {
    case Program::CONST:
      std::printf(" r%d <- pool[%d] (=%g)", I.dst, I.a, p.pool[(size_t)I.a]);
      break;
    case Program::CONSTR:
      std::printf(" r%d..r%d <- pool[%d..] (=", I.dst, I.dst + I.len - 1, I.a);
      for (int k = 0; k < I.len; ++k)
        std::printf("%s%g", k ? "," : "", p.pool[(size_t)(I.a + k)]);
      std::printf(")");
      break;
    case Program::JZ:
      std::printf(" if r%d == 0 goto %d", I.a, I.dst);
      break;
    case Program::JMP:
      std::printf(" goto %d", I.dst);
      break;
    case Program::MOVR:
      std::printf(" r%d..r%d <- r%d..r%d", I.dst, I.dst + I.len - 1, I.a,
                  I.a + I.len - 1);
      break;
    case Program::DOT:
      std::printf(" r%d <- dot(r%d.., r%d.., len %d)", I.dst, I.a, I.b, I.len);
      break;
    case Program::DENSITY:
      std::printf(" r%d <- %s(r%d, r%d, r%d)", I.dst,
                  program_density_name(I.len), I.a, I.b, I.c);
      break;
    case Program::CALL: {
      const Program::Call& c = p.calls[(size_t)I.a];
      std::printf(" %s(", opcode_name(c.opcode));
      for (int k = 0; k < c.n_in; ++k)
        std::printf("%sr%d[len %d]", k ? ", " : "", c.in[k], c.in_len[k]);
      std::printf(") -> r%d[len %d]", c.out, c.out_len);
      if (c.scratch_len)
        std::printf(", scratch r%d[len %d]", c.scratch, c.scratch_len);
      else
        std::printf(", no scratch");
      break;
    }
    default:
      std::printf(" r%d <- r%d", I.dst, I.a);
      const bool ternary = I.code == Program::LOG_MIX || I.code == Program::FMA;
      if ((I.code >= Program::ADD && I.code <= Program::FMIN) ||
          (I.code >= Program::GT && I.code <= Program::NE) ||
          I.code == Program::LSE2 || ternary)
        std::printf(", r%d", I.b);
      if (ternary) std::printf(", r%d", I.c);
      if (I.len) std::printf(" (len %d)", I.len);
  }
  std::printf("\n");
}

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: dump_islands mir.sexp data.json\n");
    return 2;
  }
  DataMap data = DataMap::from_json(slurp(argv[2]));
  CompiledModel cm = compile_model(slurp(argv[1]), data);
  const Graph& g = cm.graph;

  std::printf("graph: %zu ops, %zu slots\n", g.ops.size(), g.slots.size());
  int n = 0;
  for (size_t u = 0; u < g.ops.size(); ++u) {
    const Op& op = g.ops[u];
    if (op.opcode != OP_ISLAND) continue;
    const auto& p = *static_cast<const IslandProg*>(op.udata);
    std::printf(
        "\n== island %d (graph op %zu): %zu instrs, %d regs, "
        "%zu calls, adjoint %zu instrs, native_adj=%d\n",
        n++, u, p.code.size(), p.n_regs, p.calls.size(), p.adj.code.size(),
        (int)p.native_adj);
    std::printf("  live-ins:");
    for (size_t k = 0; k < p.ins.size(); ++k)
      std::printf(" slot%d->r%d[len %d]", op.in[k], p.ins[k].reg, p.ins[k].len);
    std::printf("\n  live-outs (out_regs):");
    for (int r : p.out_regs) std::printf(" r%d", r);
    std::printf("\n\n  FORWARD:\n");
    for (size_t i = 0; i < p.code.size(); ++i) print_instr(p, i, p.code[i]);
    if (!p.adj.code.empty()) {
      std::printf(
          "\n  ADJOINT (reverse order; dst/a/b/c are adjoint cells, "
          "va/vb/vc/vd are value registers):\n");
      for (size_t i = 0; i < p.adj.code.size(); ++i) {
        const AdjInstr& A = p.adj.code[i];
        std::printf(
            "  %3zu  %-9s dst=%d a=%d b=%d c=%d | va=%d vb=%d vc=%d "
            "vd=%d\n",
            i, program_code_spec(A.code).name, A.dst, A.a, A.b, A.c, A.va, A.vb,
            A.vc, A.vd);
      }
    }
  }
  return 0;
}

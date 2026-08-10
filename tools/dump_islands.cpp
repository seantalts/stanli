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

static const char* mn(Program::Code c) {
  switch (c) {
    case Program::CONST:
      return "CONST";
    case Program::CONSTR:
      return "CONSTR";
    case Program::MOV:
      return "MOV";
    case Program::MOVR:
      return "MOVR";
    case Program::ADD:
      return "ADD";
    case Program::SUB:
      return "SUB";
    case Program::MUL:
      return "MUL";
    case Program::DIV:
      return "DIV";
    case Program::POW:
      return "POW";
    case Program::FMAX:
      return "FMAX";
    case Program::FMIN:
      return "FMIN";
    case Program::NEG:
      return "NEG";
    case Program::EXP:
      return "EXP";
    case Program::LOG:
      return "LOG";
    case Program::SQRT:
      return "SQRT";
    case Program::SQUARE:
      return "SQUARE";
    case Program::INV:
      return "INV";
    case Program::FABS:
      return "FABS";
    case Program::INV_LOGIT:
      return "INV_LOGIT";
    case Program::LOG1M:
      return "LOG1M";
    case Program::TANH:
      return "TANH";
    case Program::GT:
      return "GT";
    case Program::GE:
      return "GE";
    case Program::LT:
      return "LT";
    case Program::LE:
      return "LE";
    case Program::EQ:
      return "EQ";
    case Program::NE:
      return "NE";
    case Program::JZ:
      return "JZ";
    case Program::JMP:
      return "JMP";
    case Program::LOG_RANGE:
      return "LOG_RANGE";
    case Program::EXP_RANGE:
      return "EXP_RANGE";
    case Program::DOT:
      return "DOT";
    case Program::LSE_RANGE:
      return "LSE_RANGE";
    case Program::SOFTMAX:
      return "SOFTMAX";
    case Program::LSE2:
      return "LSE2";
    case Program::LOG_MIX:
      return "LOG_MIX";
    case Program::DENSITY:
      return "DENSITY";
    case Program::CALL:
      return "CALL";
  }
  return "?";
}

static void print_instr(const Program& p, size_t i, const Program::Instr& I) {
  std::printf("  %3zu  %-9s", i, mn(I.code));
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
      if (I.code >= Program::ADD && I.code <= Program::FMIN)
        std::printf(", r%d", I.b);
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
            i, mn(A.code), A.dst, A.a, A.b, A.c, A.va, A.vb, A.vc, A.vd);
      }
    }
  }
  return 0;
}

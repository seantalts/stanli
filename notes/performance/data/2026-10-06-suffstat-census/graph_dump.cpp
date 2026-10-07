// Throwaway spike tool: dump the bound log-prob graph with data values.
//   graph_dump mir.sexp data.json > out.txt
// Lines:  S id len is_param active
//         O idx name variant out out2 n_in ins... n_idata idata...
//         V slot len values...        (inactive slots only, after one forward)
#include <stanli/compile.hpp>
#include <stanli/graph.hpp>
#include <stanli/optable.hpp>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

static std::string slurp(const char* path) {
  std::ifstream f(path);
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

int main(int argc, char** argv) {
  if (argc < 3) return 2;
  try {
    const stanli::DataMap data = stanli::DataMap::from_json(slurp(argv[2]));
    stanli::CompiledModel cm = stanli::compile_model(slurp(argv[1]), data);
    stanli::Executor ex(std::move(cm.graph));
    cm.bind(ex);
    const int64_t n = ex.n_params();
    double* q = ex.params_data();
    for (int64_t i = 0; i < n; ++i)
      q[i] = 0.1 + 0.05 * (i % 7) - 0.15 * (i % 3);
    bool fwd_ok = true;
    try {
      ex.forward();
    } catch (const std::exception& e) {
      fwd_ok = false;
      std::fprintf(stderr, "graph_dump: forward failed: %s\n", e.what());
    }
    const stanli::Graph& g = ex.graph();
    std::vector<char> active(g.slots.size(), 0);
    for (size_t s = 0; s < g.slots.size(); ++s) active[s] = g.slots[s].is_param;
    for (const auto& op : g.ops) {
      bool a = false;
      for (int k = 0; k < op.n_in; ++k) a = a || active[op.in[k]];
      if (a) {
        if (op.out >= 0) active[op.out] = 1;
        if (op.out2 >= 0) active[op.out2] = 1;
      }
    }
    std::printf("H nparams %lld result %d fwd_ok %d\n", (long long)n,
                g.result_slot, (int)fwd_ok);
    for (size_t s = 0; s < g.slots.size(); ++s)
      std::printf("S %zu %lld %d %d\n", s, (long long)g.slots[s].len,
                  (int)g.slots[s].is_param, (int)active[s]);
    for (size_t i = 0; i < g.ops.size(); ++i) {
      const auto& op = g.ops[i];
      std::printf("O %zu %s %d %d %d %d", i, stanli::opcode_name(op.opcode),
                  (int)op.variant, op.out, op.out2, op.n_in);
      for (int k = 0; k < op.n_in; ++k) std::printf(" %d", op.in[k]);
      std::printf(" %lld", (long long)op.n_idata);
      for (int64_t k = 0; k < op.n_idata; ++k) std::printf(" %d", op.idata[k]);
      std::printf("\n");
    }
    const stanli::Executor& cex = ex;
    for (size_t s = 0; s < g.slots.size(); ++s) {
      if (active[s]) continue;
      const double* v = cex.value_ptr((int)s);
      std::printf("V %zu %lld", s, (long long)g.slots[s].len);
      for (int64_t k = 0; k < g.slots[s].len; ++k) std::printf(" %.17g", v[k]);
      std::printf("\n");
    }
  } catch (const std::exception& e) {
    std::fprintf(stderr, "graph_dump: %s\n", e.what());
    return 1;
  }
  return 0;
}

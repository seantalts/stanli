// Print a model's lowered op sequence, or (max_ops < 0) a shape census of
// the scalar work left. Feeds harnesses/ab_corpus.py and op_census.py.
#include <stanli/compile.hpp>
#include <stanli/graph.hpp>
#include <stanli/optable.hpp>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

// Short display name: opcode_name() minus the OP_ prefix.
static const char* shortname(uint16_t oc) {
  const char* n = stanli::opcode_name(oc);
  return n[0] == 'O' && n[1] == 'P' && n[2] == '_' ? n + 3 : n;
}

static std::string slurp(const char* p) {
  std::ifstream f(p);
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

// Aggregate shape census: how much of the graph is still scalar work.
// `scalar` counts ops whose output is one element, which is what pays the
// interpreter's ~17-20 ns dispatch + recorder tax per element of data.
static void summarize(const stanli::Graph& g) {
  std::vector<int64_t> per_op((size_t)stanli::OP_COUNT_, 0);
  std::vector<int64_t> per_op_scalar((size_t)stanli::OP_COUNT_, 0);
  int64_t scalar = 0;
  for (const stanli::Op& op : g.ops) {
    const bool s = g.slots[op.out].len == 1;
    scalar += s;
    if (op.opcode < stanli::OP_COUNT_) {
      ++per_op[op.opcode];
      per_op_scalar[op.opcode] += s;
    }
  }
  std::printf("SUMMARY ops=%zu scalar_out=%lld vector_out=%lld\n", g.ops.size(),
              (long long)scalar, (long long)(g.ops.size() - scalar));
  // Opcodes by scalar-output count: the re-roll pass's remaining targets.
  std::vector<std::pair<int64_t, uint16_t>> rank;
  for (uint16_t oc = 0; oc < stanli::OP_COUNT_; ++oc)
    if (per_op[oc]) rank.push_back({per_op_scalar[oc], oc});
  std::sort(rank.rbegin(), rank.rend());
  for (size_t k = 0; k < rank.size() && k < 8; ++k)
    std::printf("  %-24s total=%lld scalar=%lld\n", shortname(rank[k].second),
                (long long)per_op[rank[k].second], (long long)rank[k].first);
}

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr,
                 "usage: dump_ops mir.sexp data.json [max_ops|-1 summary]\n");
    return 2;
  }
  const int max_ops = argc > 3 ? std::atoi(argv[3]) : 200;
  stanli::DataMap data = stanli::DataMap::from_json(slurp(argv[2]));
  stanli::CompiledModel cm = stanli::compile_model(slurp(argv[1]), data);
  const stanli::Graph& g = cm.graph;
  std::printf("slots=%zu ops=%zu result=%d\n", g.slots.size(), g.ops.size(),
              g.result_slot);
  if (max_ops < 0) {
    summarize(g);
    return 0;
  }
  for (size_t i = 0; i < g.ops.size() && (int)i < max_ops; ++i) {
    const stanli::Op& op = g.ops[i];
    std::printf("%5zu %-10s v=%02x out=s%d(len%lld)", i, shortname(op.opcode),
                op.variant, op.out, (long long)g.slots[op.out].len);
    std::printf(" in=");
    for (int k = 0; k < op.n_in; ++k) {
      std::printf("%ss%d(l%lld%s)", k ? "," : "", op.in[k],
                  (long long)g.slots[op.in[k]].len,
                  g.slots[op.in[k]].is_param ? ",P" : "");
    }
    if (op.n_idata) {
      std::printf(" idata=[");
      for (int64_t k = 0; k < op.n_idata && k < 4; ++k)
        std::printf("%s%d", k ? "," : "", op.idata[k]);
      if (op.n_idata > 4) std::printf(",...x%lld", (long long)op.n_idata);
      std::printf("]");
    }
    std::printf("\n");
  }
  return 0;
}

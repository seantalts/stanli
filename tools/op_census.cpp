// Lists the ops of a bound executor with their input and output lengths.
//
//   op_census mir.sexp data.json
//
// One line per op: name, number of inputs, input lengths, output length.
#include <stanli/compile.hpp>
#include <stanli/graph.hpp>
#include <stanli/optable.hpp>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

namespace {

std::string slurp(const char* path) {
  std::ifstream f(path);
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: op_census mir.sexp data.json\n");
    return 2;
  }
  try {
    const stanli::DataMap data = stanli::DataMap::from_json(slurp(argv[2]));
    stanli::CompiledModel cm = stanli::compile_model(slurp(argv[1]), data);
    stanli::Executor ex(std::move(cm.graph));
    cm.bind(ex);
    const stanli::Graph& g = ex.graph();
    for (const auto& op : g.ops) {
      std::printf("%s %d", stanli::opcode_name(op.opcode), op.n_in);
      for (int k = 0; k < op.n_in; ++k)
        std::printf(" %lld", (long long)g.slots[op.in[k]].len);
      std::printf(" %lld\n", (long long)g.slots[op.out].len);
    }
  } catch (const std::exception& e) {
    std::fprintf(stderr, "op_census: %s\n", e.what());
    return 1;
  }
  return 0;
}

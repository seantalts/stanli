// Print every island in a compiled model at instruction level: the forward
// program (checkpoint saves included), the CALL payloads, and the generated
// adjoint program. What dump_ops is for the graph, this is for the layer
// below it.
#include <stanli/compile.hpp>
#include <stanli/graph_print.hpp>

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

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: dump_islands mir.sexp data.json\n");
    return 2;
  }
  DataMap data = DataMap::from_json(slurp(argv[2]));
  CompiledModel cm = compile_model(slurp(argv[1]), data);
  std::string out;
  print_islands(out, cm.graph);
  std::fputs(out.c_str(), stdout);
  return 0;
}

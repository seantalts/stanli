// Print a model's lowered op sequence, or (max_ops < 0) a shape census of
// the scalar work left. Feeds harnesses/ab_corpus.py.
#include <stanli/compile.hpp>
#include <stanli/graph.hpp>
#include <stanli/optable.hpp>
#include <stanli/execution_report.hpp>
#include <stanli/structured_loop.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
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

static void print_op(const stanli::Graph& g, size_t i, int depth,
                     const std::string& suffix = "") {
  const stanli::Op& op = g.ops[i];
  std::printf("%*s%5zu %-10s v=%02x out=s%d(len%lld)", 4 * depth, "", i,
              shortname(op.opcode), op.variant, op.out,
              (long long)g.slots[op.out].len);
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
  std::printf("%s\n", suffix.c_str());
}

static const char* control_name(stanli::StructuredLoop::Node::Kind kind) {
  using Node = stanli::StructuredLoop::Node;
  switch (kind) {
    case Node::Sequence:
      return "seq";
    case Node::KernelCall:
      return "call";
    case Node::Alias:
      return "alias";
    case Node::If:
      return "if";
    case Node::For:
      return "for";
    case Node::While:
      return "while";
    case Node::Break:
      return "break";
    case Node::Continue:
      return "continue";
    case Node::Target:
      return "target";
  }
  return "?";
}

static void print_control(const stanli::StructuredLoop& loop,
                          const stanli::StructuredLoop::Node& node, int depth) {
  using Node = stanli::StructuredLoop::Node;
  if (node.kind == Node::KernelCall) {
    static const char* const storage[] = {"retained", "transient", "inplace"};
    print_op(loop.body, (size_t)node.op, depth,
             std::string(" storage=") + storage[node.storage] +
                 " active=" + (node.active ? "1" : "0"));
    return;
  }
  std::printf("%*s      %s", 4 * depth, "", control_name(node.kind));
  const std::pair<const char*, int> fields[] = {
      {"cond", node.condition}, {"iter", node.iterator}, {"lower", node.lower},
      {"upper", node.upper},    {"dst", node.dst},       {"src", node.src}};
  for (const auto& f : fields)
    if (f.second >= 0) std::printf(" %s=s%d", f.first, f.second);
  std::printf("\n");
  for (const Node& child : node.children) print_control(loop, child, depth + 1);
}

static void print_loop_body(const stanli::StructuredLoop& loop, int depth) {
  std::printf("%*s      body slots=%zu ops=%zu outputs=", 4 * depth, "",
              loop.body.slots.size(), loop.body.ops.size());
  for (size_t k = 0; k < loop.outputs.size(); ++k)
    std::printf("%ss%d", k ? "," : "", loop.outputs[k]);
  std::printf("%s\n", loop.has_target ? " target" : "");
  print_control(loop, loop.root, depth + 1);
}

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr,
                 "usage: dump_ops model.mir data.json [max_ops|-1 "
                 "summary [--write-array]|--execution-json [--forbid-mir] "
                 "[--execution-report]]\n");
    return 2;
  }
  const bool execution_json =
      argc > 3 && std::string(argv[3]) == "--execution-json";
  const int max_ops = argc > 3 && !execution_json ? std::atoi(argv[3]) : 200;
  stanli::DataMap data = stanli::DataMap::from_json(slurp(argv[2]));
  if (execution_json) {
    stanli::ExecutionTrace trace;
    // --execution-report also emits the compiler's own manifest, from a
    // scope nested inside this one.
    stanli::CompileOptions options;
    for (int i = 4; i < argc; ++i) {
      const std::string flag = argv[i];
      if (flag == "--forbid-mir") {
        trace.forbid_mir = true;
      } else if (flag == "--execution-report") {
        options.execution_report = true;
      } else {
        std::fprintf(stderr,
                     "--execution-json accepts only --forbid-mir and "
                     "--execution-report\n");
        return 2;
      }
    }
    stanli::ExecutionTraceScope scope(trace);
    try {
      const auto cm = stanli::compile_model(slurp(argv[1]), data, 1, options);
      trace.check();
      std::puts(stanli::execution_report(cm, &trace).c_str());
      return 0;
    } catch (const std::exception& e) {
      std::puts(stanli::execution_trace_report(trace).c_str());
      std::fprintf(stderr, "%s\n", e.what());
      return 1;
    }
  }
  stanli::CompiledModel cm = stanli::compile_model(slurp(argv[1]), data);
  // --write-array shows the generated-quantities graph instead.
  const bool write_array = argc > 4 && std::string(argv[4]) == "--write-array";
  if (write_array && !cm.write_array) {
    std::fprintf(stderr, "model has no write_array graph\n");
    return 1;
  }
  const stanli::Graph& g = write_array ? cm.write_array->graph : cm.graph;
  std::printf("slots=%zu ops=%zu result=%d%s\n", g.slots.size(), g.ops.size(),
              g.result_slot, g.parallel_blas ? " parallel_blas" : "");
  if (max_ops < 0) {
    summarize(g);
    return 0;
  }
  for (size_t i = 0; i < g.ops.size() && (int)i < max_ops; ++i) {
    print_op(g, i, 0);
    const stanli::Op& op = g.ops[i];
    if (op.opcode == stanli::OP_LOOP && op.udata)
      print_loop_body(*static_cast<const stanli::StructuredLoop*>(op.udata), 1);
  }
  return 0;
}

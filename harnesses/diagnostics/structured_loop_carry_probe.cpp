// Inspect actual compiled write_array plan metadata; does not evaluate a draw.
// /usr/bin/c++ -std=c++17 -O1 -Iruntime/include \
//   harnesses/diagnostics/structured_loop_carry_probe.cpp tests/tbb_stub.cpp \
//   -Lbuild -lstanli_test_core -Wl,-rpath,"$PWD/build" \
//   -o build/structured_loop_carry_probe
// build/structured_loop_carry_probe model.mir.sexp data.json > plans.jsonl
#include <stanli/compile.hpp>
#include <stanli/optable.hpp>
#include <stanli/structured_loop.hpp>

#include <algorithm>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {
std::string read(const char* path) {
  std::ifstream stream(path);
  if (!stream) throw std::runtime_error(std::string("cannot open ") + path);
  std::ostringstream content;
  content << stream.rdbuf();
  return content.str();
}
void writes(const stanli::StructuredLoop::Node& node,
            const stanli::StructuredLoop& plan, std::set<int>& bases,
            std::vector<std::pair<int, int>>& aliases) {
  if (node.kind == stanli::StructuredLoop::Node::KernelCall &&
      node.storage == stanli::StructuredLoop::Node::InPlace)
    bases.insert(plan.body.ops[node.op].in[0]);
  if (node.kind == stanli::StructuredLoop::Node::Alias)
    aliases.emplace_back(node.dst, node.src);
  for (const auto& child : node.children) writes(child, plan, bases, aliases);
}
bool prefix(const stanli::StructuredLoop::Node& node,
            std::vector<const stanli::StructuredLoop::Node*>& nodes) {
  if (node.kind == stanli::StructuredLoop::Node::Sequence) {
    for (const auto& child : node.children)
      if (!prefix(child, nodes)) return false;
    return true;
  }
  nodes.push_back(&node);
  return node.kind == stanli::StructuredLoop::Node::Alias ||
         node.kind == stanli::StructuredLoop::Node::KernelCall;
}
}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) return 2;
  const auto model = stanli::compile_model(
      read(argv[1]), stanli::DataMap::from_json(read(argv[2])));
  if (!model.write_array) throw std::runtime_error("no write_array graph");
  const auto& graph = model.write_array->graph;
  int ordinal = 0;
  for (std::size_t op_index = 0; op_index < graph.ops.size(); ++op_index) {
    const auto& op = graph.ops[op_index];
    if (op.opcode != stanli::OP_LOOP) continue;
    const auto& plan = *static_cast<const stanli::StructuredLoop*>(op.udata);
    std::set<int> bases;
    std::vector<std::pair<int, int>> aliases;
    writes(plan.root, plan, bases, aliases);
    const auto output = [&](int slot) {
      return std::find(plan.outputs.begin(), plan.outputs.end(), slot) !=
             plan.outputs.end();
    };
    std::cout << "{\"ordinal\":" << ordinal++ << ",\"parent_op\":" << op_index
              << ",\"cache_execution_path\":"
              << (plan.cache_execution_path ? "true" : "false")
              << ",\"imports\":[";
    bool comma = false;
    for (const auto& import : plan.imports) {
      if (comma) std::cout << ',';
      comma = true;
      std::cout << "{\"slot\":" << import.slot << ",\"input\":" << import.input
                << ",\"offset\":" << import.offset
                << ",\"len\":" << plan.body.slots[import.slot].len
                << ",\"active\":" << (import.active ? "true" : "false")
                << ",\"data_only\":" << (import.data_only ? "true" : "false")
                << ",\"inplace_base\":"
                << (bases.count(import.slot) ? "true" : "false")
                << ",\"output\":" << (output(import.slot) ? "true" : "false")
                << '}';
    }
    std::cout << "],\"outputs\":[";
    comma = false;
    for (int slot : plan.outputs) {
      if (comma) std::cout << ',';
      comma = true;
      std::cout << "{\"slot\":" << slot
                << ",\"len\":" << plan.body.slots[slot].len
                << ",\"inplace_base\":"
                << (bases.count(slot) ? "true" : "false") << '}';
    }
    std::cout << "],\"aliases\":[";
    comma = false;
    for (const auto& alias : aliases) {
      if (comma) std::cout << ',';
      comma = true;
      std::cout << "{\"dst\":" << alias.first << ",\"src\":" << alias.second
                << ",\"len\":" << plan.body.slots[alias.first].len << '}';
    }
    std::cout << "],\"straight_prefix\":[";
    std::vector<const stanli::StructuredLoop::Node*> initial;
    prefix(plan.root, initial);
    comma = false;
    for (const auto* node : initial) {
      if (comma) std::cout << ',';
      comma = true;
      std::cout << "{\"kind\":" << static_cast<int>(node->kind)
                << ",\"dst\":" << node->dst << ",\"src\":" << node->src;
      if (node->kind == stanli::StructuredLoop::Node::KernelCall) {
        const auto& operation = plan.body.ops[node->op];
        std::cout << ",\"out\":" << operation.out
                  << ",\"out2\":" << operation.out2;
      }
      std::cout << '}';
    }
    std::cout << "]}\n";
  }
}

// Deterministic text rendering of the op graph. Output is diffed between
// passes, so op lines are keyed on the destination slot, never an op index.
#ifndef STANLI_GRAPH_PRINT_HPP
#define STANLI_GRAPH_PRINT_HPP

#include <stanli/graph.hpp>

#include <string>
#include <utility>
#include <vector>

namespace stanli {

struct GraphPrintInfo {
  std::vector<int> roots;
  std::vector<int> target_terms;
  std::vector<int> jac_slots;
  std::vector<std::pair<std::string, int>> views;
  const std::vector<std::pair<int, std::vector<double>>>* fills = nullptr;
  bool print_islands = true;
};

void print_graph(std::string& out, const Graph& g,
                 const GraphPrintInfo& info = {});

void print_islands(std::string& out, const Graph& g);

}  // namespace stanli

#endif

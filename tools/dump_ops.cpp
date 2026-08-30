// Print a model's lowered op sequence, or (max_ops < 0) a shape census of
// the scalar work left. Feeds harnesses/ab_corpus.py and op_census.py.
#include <stanli/compile.hpp>
#include <stanli/graph.hpp>
#include <stanli/optable.hpp>
#include <stanli/scan.hpp>

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

static int64_t reverse_value_cells(const stanli::IslandProg& program) {
  std::vector<uint8_t> needed(static_cast<size_t>(program.n_regs), 0);
  const auto mark = [&](int32_t reg, int32_t len) {
    if (reg < 0 || len < 0 || static_cast<int64_t>(reg) + len > program.n_regs)
      return;
    std::fill(needed.begin() + reg, needed.begin() + reg + len, 1);
  };
  for (const stanli::AdjInstr& instruction : program.adj.code) {
    using Code = stanli::Program;
    if (instruction.code == Code::CALL) {
      const auto& call = program.calls[static_cast<size_t>(instruction.a)];
      for (int k = 0; k < call.n_in; ++k)
        mark(call.bwd_value_in[k], call.in_len[k]);
      mark(call.bwd_value_out, call.out_len);
      mark(call.scratch, call.scratch_len);
      continue;
    }
    switch (instruction.code) {
      case Code::JZ:
        mark(instruction.va, 1);
        break;
      case Code::MUL:
      case Code::DIV:
      case Code::FMAX:
      case Code::FMIN:
      case Code::LSE2:
        mark(instruction.va, 1);
        mark(instruction.vb, 1);
        break;
      case Code::FMA:
        mark(instruction.va, 1);
        mark(instruction.vb, 1);
        break;
      case Code::POW:
        mark(instruction.va, 1);
        mark(instruction.vb, 1);
        mark(instruction.vd, 1);
        break;
      case Code::EXP:
      case Code::SQRT:
      case Code::INV_LOGIT:
        mark(instruction.vd, 1);
        break;
      case Code::LOG:
      case Code::SQUARE:
      case Code::INV:
      case Code::FABS:
      case Code::LOG1M:
      case Code::LOG1P_EXP:
      case Code::TANH:
        mark(instruction.va, 1);
        break;
      case Code::LOG_RANGE:
        mark(instruction.va, instruction.len);
        break;
      case Code::EXP_RANGE:
      case Code::SOFTMAX:
        mark(instruction.vd, instruction.len);
        break;
      case Code::DOT:
        mark(instruction.va, instruction.len);
        mark(instruction.vb, instruction.len);
        break;
      case Code::LSE_RANGE:
        mark(instruction.va, instruction.len);
        mark(instruction.vd, 1);
        break;
      case Code::LOG_MIX:
        mark(instruction.va, 1);
        mark(instruction.vb, 1);
        mark(instruction.vc, 1);
        break;
      case Code::DENSITY: {
        if (instruction.mask == 0) break;
        const int arity = stanli::program_density_arity(instruction.len);
        if (arity > 3) {
          mark(instruction.va, arity);
        } else {
          mark(instruction.va, 1);
          if (arity > 1) mark(instruction.vb, 1);
          if (arity > 2) mark(instruction.vc, 1);
        }
        break;
      }
      default:
        break;
    }
  }
  return std::count(needed.begin(), needed.end(), uint8_t{1});
}

static void summarize_scan(const stanli::ScanSpec& scan) {
  std::vector<int64_t> template_hits(scan.templates.size(), 0);
  if (scan.template_for_iteration.empty()) {
    if (!template_hits.empty()) template_hits[0] = scan.count;
  } else {
    for (uint32_t index : scan.template_for_iteration)
      if (index < template_hits.size()) ++template_hits[index];
  }
  std::printf(
      "      scan count=%lld templates=%zu carry=%lld output=%lld block=%lld\n",
      (long long)scan.count, scan.templates.size(), (long long)scan.carry_cells,
      (long long)scan.output_cells, (long long)scan.checkpoint_block);
  for (size_t i = 0; i < scan.templates.size(); ++i) {
    const auto& tm = scan.templates[i];
    int64_t call_scratch = 0;
    int64_t call_output = 0;
    int64_t island_calls = 0;
    int64_t active_island_calls = 0;
    int64_t native_island_calls = 0;
    int64_t island_code = 0;
    int64_t island_adj_code = 0;
    int64_t step_jz = 0;
    int64_t step_jmp = 0;
    std::vector<uint8_t> invariant_call(tm.step.calls.size(), 0);
    for (const auto& call : tm.invariant_calls)
      if (call.call_index >= 0 &&
          static_cast<size_t>(call.call_index) < invariant_call.size())
        invariant_call[static_cast<size_t>(call.call_index)] = 1;
    struct CallCensus {
      uint16_t opcode = 0;
      int64_t total = 0;
      int64_t repeated = 0;
      int64_t output = 0;
      int64_t scratch = 0;
    };
    std::vector<CallCensus> call_census;
    struct IslandCensus {
      size_t call = 0;
      int64_t code = 0;
      int64_t adj_code = 0;
      int64_t regs = 0;
      int64_t adj_regs = 0;
      int64_t jz = 0;
      int64_t jmp = 0;
      bool active = false;
      bool native = false;
      bool invariant = false;
      int64_t inputs = 0;
    };
    std::vector<IslandCensus> island_census;
    for (const auto& instruction : tm.step.code) {
      step_jz += instruction.code == stanli::Program::JZ;
      step_jmp += instruction.code == stanli::Program::JMP;
    }
    for (size_t call_index = 0; call_index < tm.step.calls.size();
         ++call_index) {
      const auto& call = tm.step.calls[call_index];
      call_scratch += call.scratch_len;
      call_output += call.out_len;
      auto census = std::find_if(
          call_census.begin(), call_census.end(),
          [&](const CallCensus& item) { return item.opcode == call.opcode; });
      if (census == call_census.end()) {
        call_census.push_back({call.opcode});
        census = call_census.end() - 1;
      }
      ++census->total;
      census->repeated += !invariant_call[call_index];
      census->output += call.out_len;
      census->scratch += call.scratch_len;
      if (call.opcode == stanli::OP_ISLAND) {
        ++island_calls;
        const auto* nested = static_cast<const stanli::IslandProg*>(call.udata);
        if (nested != nullptr) {
          active_island_calls += std::any_of(
              nested->ins.begin(), nested->ins.end(),
              [](const stanli::IslandProg::LiveIn& input) {
                return input.active;
              });
          native_island_calls += nested->native_adj || nested->selector_adj;
          island_code += static_cast<int64_t>(nested->code.size());
          island_adj_code += static_cast<int64_t>(nested->adj.code.size());
          const int64_t nested_jz = std::count_if(
              nested->code.begin(), nested->code.end(),
              [](const stanli::Program::Instr& instruction) {
                return instruction.code == stanli::Program::JZ;
              });
          const int64_t nested_jmp = std::count_if(
              nested->code.begin(), nested->code.end(),
              [](const stanli::Program::Instr& instruction) {
                return instruction.code == stanli::Program::JMP;
              });
          const int64_t nested_inputs = std::accumulate(
              nested->ins.begin(), nested->ins.end(), int64_t{0},
              [](int64_t total, const stanli::IslandProg::LiveIn& input) {
                return total + input.len;
              });
          island_census.push_back(
              {call_index,
               static_cast<int64_t>(nested->code.size()),
               static_cast<int64_t>(nested->adj.code.size()), nested->n_regs,
               nested->adj.n_regs, nested_jz, nested_jmp,
               std::any_of(nested->ins.begin(), nested->ins.end(),
                           [](const stanli::IslandProg::LiveIn& input) {
                             return input.active;
                           }),
               nested->native_adj || nested->selector_adj,
               invariant_call[call_index] != 0, nested_inputs});
        }
      }
    }
    std::printf(
        "        t%zu regs=%d adj_regs=%d code=%zu adj_code=%zu calls=%zu "
        "call_out=%lld call_scratch=%lld islands=%lld active_islands=%lld "
        "native=%lld island_code=%lld island_adj_code=%lld jz=%lld jmp=%lld "
        "invariants=%zu invariant_cells=%lld repeated_code=%zu hits=%lld "
        "reverse_value_cells=%lld "
        "liveins=%zu carries=%zu inputs=%zu sinks=%zu\n",
        i, tm.step.n_regs, tm.step.adj.n_regs, tm.step.code.size(),
        tm.step.adj.code.size(), tm.step.calls.size(), (long long)call_output,
        (long long)call_scratch, (long long)island_calls,
        (long long)active_island_calls, (long long)native_island_calls,
        (long long)island_code, (long long)island_adj_code,
        (long long)step_jz, (long long)step_jmp, tm.invariant_calls.size(),
        (long long)tm.invariant_cache_cells, tm.repeated_code.size(),
        (long long)template_hits[i], (long long)reverse_value_cells(tm.step),
        tm.step.ins.size(), tm.carry.size(), tm.inputs.size(), tm.sinks.size());
    const int64_t hits = template_hits[i];
    const int64_t fwd_restore_cells =
        hits > 0 ? (hits - 1) * tm.invariant_cache_cells : 0;
    const int64_t gradient_restore_cells =
        fwd_restore_cells +
        (scan.checkpoint_block > 1 ? hits * tm.invariant_cache_cells : 0) +
        hits * tm.invariant_cache_cells;
    std::printf(
        "          cache first_store_bytes=%lld fwd_restore_bytes=%lld "
        "gradient_restore_bytes=%lld\n",
        (long long)(tm.invariant_cache_cells * 8),
        (long long)(fwd_restore_cells * 8),
        (long long)(gradient_restore_cells * 8));
    std::sort(call_census.begin(), call_census.end(),
              [](const CallCensus& a, const CallCensus& b) {
                if (a.scratch != b.scratch) return a.scratch > b.scratch;
                return a.total > b.total;
              });
    for (size_t k = 0; k < call_census.size() && k < 10; ++k) {
      const auto& item = call_census[k];
      std::printf(
          "          call %-20s total=%lld repeated=%lld out=%lld scratch=%lld\n",
          shortname(item.opcode), (long long)item.total,
          (long long)item.repeated, (long long)item.output,
          (long long)item.scratch);
    }
    std::sort(island_census.begin(), island_census.end(),
              [](const IslandCensus& a, const IslandCensus& b) {
                if (a.code != b.code) return a.code > b.code;
                return a.call < b.call;
              });
    for (size_t k = 0; k < island_census.size() && k < 10; ++k) {
      const auto& item = island_census[k];
      std::printf(
          "          island call=%zu code=%lld adj=%lld regs=%lld adj_regs=%lld "
          "jz=%lld jmp=%lld inputs=%lld active=%d native=%d invariant=%d\n",
          item.call, (long long)item.code, (long long)item.adj_code,
          (long long)item.regs, (long long)item.adj_regs, (long long)item.jz,
          (long long)item.jmp, (long long)item.inputs, item.active, item.native,
          item.invariant);
    }
  }
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
    if (op.opcode == stanli::OP_SCAN && op.udata != nullptr)
      summarize_scan(*static_cast<const stanli::ScanSpec*>(op.udata));
  }
  return 0;
}

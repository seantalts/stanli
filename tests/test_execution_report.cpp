#include <stanli/execution_report.hpp>
#include <stanli/compile.hpp>
#include <stanli/island.hpp>
#include <stanli/mir_interp.hpp>
#include <stanli/optable.hpp>
#include <stanli/message_sink.hpp>
#include <stanli/wa_interp.hpp>
#include <rapidjson/document.h>
#include "env_helpers.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <thread>

namespace {
int failures = 0;
void check(bool ok, const char* msg) {
  if (!ok) {
    ++failures;
    std::printf("FAIL %s\n", msg);
  }
}
std::string slurp(const char* name) {
  std::ifstream f(name);
  std::ostringstream s;
  s << f.rdbuf();
  return s.str();
}
rapidjson::Document parse(const std::string& text) {
  rapidjson::Document d;
  d.Parse(text.c_str());
  if (d.HasParseError()) throw std::runtime_error("invalid report JSON");
  return d;
}
bool has(const std::string& text, const char* pattern) {
  return text.find(pattern) != std::string::npos;
}
}  // namespace

int main() {
  using namespace stanli;
  const std::map<std::string, const mir::FunDef*> funs;
  ExecutionTrace outer, inner;
  outer.forbid_mir = true;
  {
    ExecutionTraceScope a(outer);
    {
      ExecutionTraceScope b(inner);
      try {
        MirInterp<double> in(funs, "quoted\"context\n");
      } catch (const std::runtime_error&) {
      }  // speculative refusal must not hide it
    }
    check(outer.violation && !inner.violation,
          "nested scopes retain strict violation");
    bool refused = false;
    try {
      outer.check();
    } catch (const std::runtime_error&) {
      refused = true;
    }
    check(refused, "sticky strict check after a swallowed exception");
    std::thread worker([&] { MirInterp<double> in(funs, "other_thread"); });
    worker.join();
    check(outer.events.size() == 1, "thread-local trace isolation");
  }
  MirInterp<double> unobserved(funs, "outside_scope");
  check(outer.events.size() == 1 && inner.events.size() == 1,
        "scope restoration");
  auto escaped = parse(execution_trace_report(outer));
  check(std::string(escaped["interpreter_events"][0]["context"].GetString()) ==
            "quoted\"context\n",
        "JSON escapes diagnostic contexts");

  // A generated region still invokes a taped kernel. A value-only copy has
  // no backward consumer even if its native_adj flag happens to be set.
  CompiledModel synthetic;
  auto region = std::make_shared<IslandProg>();
  region->native_adj = true;
  const Kernel* softmax = find_kernel(OP_SOFTMAX);
  Program::Call call;
  call.opcode = OP_SOFTMAX;
  call.forward = softmax->forward;
  call.backward = softmax->backward;
  call.out_len = 2;
  region->calls.push_back(call);
  region->code.emplace_back(Program::CALL, 0, 0);
  int slot = synthetic.graph.add_slot(2, true);
  synthetic.graph.add_op(OP_ISLAND, {slot}, slot);
  synthetic.graph.ops.back().udata = region.get();
  synthetic.graph.udata_pool.push_back(region);
  synthetic.write_array.emplace();
  synthetic.write_array->graph = synthetic.graph;
  auto report = parse(execution_report(synthetic));
  auto& selected = report["log_prob"]["operations"][0]["region"];
  check(std::string(selected["derivative"].GetString()) ==
            "generated_reverse_program",
        "generated region classification");
  check(std::string(selected["kernel_calls"][0]["derivative"].GetString()) ==
            "nested_tape",
        "generated reverse does not hide taped child");
  auto& wa = report["write_array"]["graph"]["operations"][0]["region"];
  check(std::string(wa["derivative"].GetString()) == "not_used",
        "GQ derivative is unused, not var replay");
  region->native_adj = false;
  check(has(execution_report(synthetic), "var_replay"),
        "var replay classification");
  // Metadata belongs to the registered function; a different retained
  // function pointer must not borrow its classification.
  region->calls[0].backward = nullptr;
  auto replaced = parse(execution_report(synthetic));
  check(std::string(replaced["log_prob"]["operations"][0]["region"]
                            ["kernel_calls"][0]["derivative"]
                                .GetString()) == "unclassified",
        "retained CALL replacement is not misclassified");

  const std::string source =
      slurp("tests/fixtures/execution_legacy_ode.tmir.sexp");
  const auto data = DataMap::from_json("{}");
  ExecutionTrace trace;
  CompiledModel model;
  {
    ExecutionTraceScope scope(trace);
    model = compile_model(source, data);
  }
  std::string reasons;
  for (const auto& reason : model.interpreter_fallbacks) reasons += reason;
  check(has(reasons, "graph_rhs"), "legacy graph RHS fallback is reported");
  check(has(reasons, "region_rhs"), "legacy program RHS fallback is reported");
  auto manifest = execution_report(model, &trace);
  parse(manifest);
  check(has(manifest, "graph_rhs") && has(manifest, "region_rhs"),
        "manifest reaches nested callback sites");
  check(!trace.events.empty(), "preparation interpreter observations captured");
  {
    auto rng_model =
        compile_model(slurp("tests/fixtures/execution_rng.tmir.sexp"), data);
    check(rng_model.write_array && rng_model.write_array->interp,
          "dynamic-storage RNG fixture retains interpreter");
    check(rng_model.transform_inits && rng_model.transform_inits->interp,
          "initialization fixture retains interpreter");
    if (!rng_model.write_array || !rng_model.write_array->interp ||
        !rng_model.transform_inits || !rng_model.transform_inits->interp)
      return 1;
    ExecutionTrace entries;
    DataMap::Entry rate;
    rate.r = {2.0};
    std::map<std::string, DataMap::Entry> params{{"rate", rate}};
    WaRng rng(1234);
    {
      ExecutionTraceScope scope(entries);
      rng_model.transform_inits->interp->eval(params);
      rng_model.write_array->interp->eval(params, rng);
    }
    check(entries.events.count({"transform_inits", "construction"}) == 1,
          "constrained initialization is observable");
    check(entries.events.count({"write_array", "construction"}) == 1,
          "interpreted RNG execution is observable");
    auto rng_report = parse(execution_report(rng_model));
    check(!rng_report["write_array"].HasMember("graph"),
          "unselected partial GQ graph is not reported as executing");
  }
  test_setenv("STANLI_NO_INTERPRETER", "1", 1);
  bool rejected = false;
  try {
    compile_model(source, data);
  } catch (const CompileError& e) {
    rejected = has(e.what(), "graph_rhs") && has(e.what(), "region_rhs");
  }
  test_unsetenv("STANLI_NO_INTERPRETER");
  check(rejected,
        "legacy callback notes participate in existing refusal policy");

  // Diagnostics must not alter either selected graph or gradient bytes.
  std::vector<std::string> diagnostics;
  set_diagnostic_sink(
      [&](const char* p, size_t n) { diagnostics.emplace_back(p, n); });
  test_setenv("STANLI_EXECUTION_REPORT", "1", 1);
  auto observed = compile_model(source, data);
  test_unsetenv("STANLI_EXECUTION_REPORT");
  set_diagnostic_sink(nullptr);
  check(!diagnostics.empty(), "environment opt-in emits a manifest");
  check(execution_report(observed) == execution_report(model),
        "diagnostics preserve structure");
  Executor a(std::move(model.graph)), b(std::move(observed.graph));
  model.bind(a);
  observed.bind(b);
  for (double q : {-0.2, 0.3}) {
    a.params_data()[0] = b.params_data()[0] = q;
    double da = 0, db = 0;
    const double va = a.gradient(&da), vb = b.gradient(&db);
    check(std::memcmp(&va, &vb, sizeof(double)) == 0 &&
              std::memcmp(&da, &db, sizeof(double)) == 0,
          "tracing preserves values and gradients across changing branches");
  }
  if (!failures) std::puts("test_execution_report OK");
  return failures ? 1 : 0;
}

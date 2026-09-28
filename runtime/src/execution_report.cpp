#include <stanli/execution_report.hpp>
#include <stanli/compile.hpp>
#include <stanli/algebra.hpp>
#include <stanli/dae.hpp>
#include <stanli/ode.hpp>
#include <stanli/ode_adjoint.hpp>
#include <stanli/quadrature.hpp>
#include <stanli/reduce_sum.hpp>
#include <stanli/structured_loop.hpp>
#include <stanli/optable.hpp>
#include <stanli/message_sink.hpp>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <cstdlib>
#include <stdexcept>

namespace stanli {
namespace {
thread_local ExecutionTraceScope* current_scope = nullptr;

struct Report {
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> w{buffer};
  void str(const char* key, const std::string& value) {
    w.Key(key);
    w.String(value.data(), static_cast<rapidjson::SizeType>(value.size()));
  }
  void number(const char* key, int64_t value) {
    w.Key(key);
    w.Int64(value);
  }
  void flag(const char* key, bool value) {
    w.Key(key);
    w.Bool(value);
  }
  void start(const char* kind) {
    w.StartObject();
    number("schema_version", 1);
    str("kind", kind);
  }
  std::string finish() {
    w.EndObject();
    return buffer.GetString();
  }
  void trace(const ExecutionTrace* t) {
    w.Key("interpreter_events");
    if (!t) {
      w.Null();
      return;
    }
    w.StartArray();
    for (const auto& entry : t->events) {
      w.StartObject();
      str("context", entry.first.first);
      str("event", entry.first.second);
      w.Key("count");
      w.Uint64(entry.second);
      w.EndObject();
    }
    w.EndArray();
    flag("strict_violation", t->violation);
  }
  void program(const Program& p, const std::string& id, bool reverse,
               int depth) {
    number("registers", p.n_regs);
    number("instructions", p.code.size());
    w.Key("kernel_calls");
    w.StartArray();
    for (size_t i = 0; i < p.code.size(); ++i) {
      const auto& ins = p.code[i];
      if (ins.code != Program::CALL) continue;
      const auto& c = p.calls.at(static_cast<size_t>(ins.a));
      w.StartObject();
      str("site", id + "/call/" + std::to_string(i));
      str("operation", opcode_name(c.opcode));
      number("variant", c.variant);
      number("input_adjoint_mask", c.input_adjoint_mask);
      number("output_elements", c.out_len);
      str("value_engine", "kernel_call");
      const Kernel* registered = find_kernel(c.opcode);
      const bool same = registered && registered->backward == c.backward;
      str("derivative", !reverse ? "not_used"
                        : same   ? registered->derivative_mechanism
                                 : "unclassified");
      payload(c.opcode, c.udata_owner.get(), id + "/call/" + std::to_string(i),
              reverse, depth + 1);
      w.EndObject();
    }
    w.EndArray();
  }
  void callback(const RetainedCallback& c, const std::string& name,
                const std::string& id, bool reverse, int depth) {
    w.Key("callback");
    w.StartObject();
    str("name", name);
    str("value_engine", c.prog.ok ? "register_program" : "mir_interpreter");
    str("refusal", c.prog.why);
    str("derivative", !reverse ? "not_used" : "solver_dependent");
    flag("fresh_interpreter_per_call", !c.prog.ok);
    if (c.prog.ok) program(c.prog, id + "/callback", reverse, depth);
    w.EndObject();
  }
  void island(const IslandProg& p, const std::string& id, bool reverse,
              int depth) {
    str("value_engine", "register_program");
    str("derivative", !reverse       ? "not_used"
                      : p.native_adj ? "generated_reverse_program"
                                     : "var_replay");
    flag("native_adj", p.native_adj);
    number("adjoint_instructions", p.adj.code.size());
    program(p, id, reverse, depth);
  }
  void control(const StructuredLoop& loop, const StructuredLoop::Node& node,
               const std::string& id, bool reverse, int depth) {
    using Node = StructuredLoop::Node;
    if (node.kind == Node::KernelCall) {
      op(loop.body, loop.body.ops.at(static_cast<size_t>(node.op)), id,
         reverse && node.active, depth);
    } else if (node.kind == Node::Segment) {
      w.StartObject();
      str("site", id);
      str("operation", "structured_segment");
      island(loop.segments.at(static_cast<size_t>(node.segment)).program, id,
             reverse, depth);
      w.EndObject();
    }
    for (size_t i = 0; i < node.children.size(); ++i)
      control(loop, node.children[i], id + "/" + std::to_string(i), reverse,
              depth);
  }
  void payload(uint16_t code, const void* data, const std::string& id,
               bool reverse, int depth) {
    if (!data) return;
    if (depth > 64) {
      str("children_status", "depth_limit");
      return;
    }
    if (code == OP_ISLAND) {
      w.Key("region");
      w.StartObject();
      island(*static_cast<const IslandProg*>(data), id, reverse, depth);
      w.EndObject();
    } else if (code == OP_LOOP) {
      const auto& loop = *static_cast<const StructuredLoop*>(data);
      str("control_engine", "structured_loop");
      str("control_selection", "runtime");
      w.Key("retained_sites");
      w.StartArray();
      control(loop, loop.root, id + "/body", reverse, depth + 1);
      w.EndArray();
    } else if (code == OP_REDUCE_SUM) {
      const auto& reduction = *static_cast<const ReduceSumSpec*>(data);
      w.Key("children");
      w.StartArray();
      for (size_t i = 0; i < reduction.children.size(); ++i)
        graph(reduction.children[i].graph, id + "/child/" + std::to_string(i),
              reverse, depth + 1);
      w.EndArray();
    } else if (code == OP_ODE) {
      const auto& c = *static_cast<const OdeSpec*>(data);
      flag("direct_rk_payload", bool(c.direct_rk));
      flag("direct_rk_enabled", c.direct_rk_enabled);
      str("direct_rk_refusal", c.direct_rk_why);
      str("solver_derivative_selection",
          reverse ? "type_shape_and_mode_dependent" : "not_used");
      callback(c, c.rhs_name, id, reverse, depth);
    } else if (code == OP_ODE_ADJOINT) {
      const auto& c = *static_cast<const OdeAdjointSpec*>(data);
      callback(c, c.rhs_name, id, reverse, depth);
    } else if (code == OP_DAE) {
      const auto& c = *static_cast<const DaeSpec*>(data);
      callback(c, c.residual_name, id, reverse, depth);
    } else if (code == OP_ALGEBRA_SOLVER) {
      const auto& c = *static_cast<const AlgebraSpec*>(data);
      callback(c, c.system_name, id, reverse, depth);
    } else if (code == OP_QUADRATURE) {
      const auto& c = *static_cast<const QuadratureSpec*>(data);
      callback(c, c.callback_name, id, reverse, depth);
    }
  }
  void op(const Graph& g, const Op& o, const std::string& id, bool reverse,
          int depth) {
    w.StartObject();
    str("site", id);
    str("operation", opcode_name(o.opcode));
    number("variant", o.variant);
    str("value_engine", "bound_graph_kernel");
    const auto* k = find_kernel(o.opcode);
    str("derivative", !reverse ? "not_used"
                      : k      ? k->derivative_mechanism
                               : "unclassified");
    number("output_elements", o.out < 0 ? 0 : g.slots.at(o.out).len);
    // Slot lengths do not preserve full logical shapes or transitive activity.
    w.Key("logical_shape");
    w.Null();
    w.Key("input_elements");
    w.StartArray();
    for (int i = 0; i < o.n_in; ++i) w.Int64(g.slots.at(o.in[i]).len);
    w.EndArray();
    payload(o.opcode, o.udata, id, reverse, depth + 1);
    w.EndObject();
  }
  void graph(const Graph& g, const std::string& id, bool reverse,
             int depth = 0) {
    w.StartObject();
    str("site", id);
    w.Key("operations");
    w.StartArray();
    for (size_t i = 0; i < g.ops.size(); ++i)
      op(g, g.ops[i], id + "/op/" + std::to_string(i), reverse, depth);
    w.EndArray();
    w.EndObject();
  }
};
}  // namespace

ExecutionTraceScope::ExecutionTraceScope(ExecutionTrace& trace)
    : previous_(current_scope), trace_(trace) {
  current_scope = this;
}
ExecutionTraceScope::~ExecutionTraceScope() { current_scope = previous_; }
void ExecutionTrace::check() const {
  if (violation)
    throw std::runtime_error(
        "strict execution diagnostic: MIR interpreter use observed");
}
void record_interpreter_event(const std::string& phase, const char* event) {
  bool forbidden = false;
  for (auto* scope = current_scope; scope; scope = scope->previous_) {
    auto& trace = scope->trace_;
    ++trace.events[{phase, event}];
    trace.violation |= trace.forbid_mir;
    forbidden |= trace.forbid_mir;
  }
  if (forbidden)
    throw std::runtime_error(
        "strict execution diagnostic: MIR interpreter use observed");
}
std::string execution_trace_report(const ExecutionTrace& trace) {
  Report r;
  r.start("execution_trace");
  r.trace(&trace);
  return r.finish();
}
std::string execution_report(const CompiledModel& model,
                             const ExecutionTrace* trace) {
  Report r;
  r.start("execution_manifest");
  r.str("stage", "compiled_model");
  r.str("host_probe", "not_run");
  r.str("evidence", "selected_structure_not_execution_counts");
  r.trace(trace);
  r.w.Key("log_prob");
  r.graph(model.graph, "log_prob", true);
  r.w.Key("write_array");
  if (!model.write_array)
    r.w.Null();
  else {
    const auto& wa = *model.write_array;
    r.w.StartObject();
    r.str("value_engine", wa.interp ? "mir_interpreter" : "bound_graph");
    r.str("status", wa.interp ? "attached_unprobed" : "lowered");
    r.str("refusal", wa.truncated);
    // Partial or forced-interpreter graphs are not the driver-selected path.
    if (!wa.interp) {
      r.w.Key("graph");
      r.graph(wa.graph, "write_array", false);
    }
    r.w.EndObject();
  }
  r.w.Key("transform_inits");
  if (!model.transform_inits)
    r.w.Null();
  else {
    r.w.StartObject();
    r.str("value_engine",
          model.transform_inits->interp ? "mir_interpreter" : "unavailable");
    r.str("refusal", model.transform_inits->truncated);
    r.w.EndObject();
  }
  r.w.Key("reported_fallbacks");
  r.w.StartArray();
  for (const auto& reason : model.interpreter_fallbacks)
    r.w.String(reason.c_str());
  r.w.EndArray();
  return r.finish();
}
void report_execution_host(const char* host, const char* value_engine,
                           const char* probe_status,
                           const std::string& reason) {
  if (!execution_reporting_enabled()) return;
  Report r;
  r.start("execution_host_selection");
  r.str("host", host);
  r.str("phase", "write_array");
  r.str("value_engine", value_engine);
  r.str("probe_status", probe_status);
  r.str("reason", reason);
  emit_diagnostic(r.finish());
}
bool execution_reporting_enabled() {
  const char* flag = std::getenv("STANLI_EXECUTION_REPORT");
  return flag && *flag && *flag != '0';
}
}  // namespace stanli

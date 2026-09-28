#ifndef STANLI_EXECUTION_REPORT_HPP
#define STANLI_EXECUTION_REPORT_HPP

#include <cstdint>
#include <map>
#include <string>
#include <utility>

namespace stanli {
struct CompiledModel;

// Opt-in, thread-local observation of coarse interpreter boundaries. Counts
// describe constructions/probes, never instruction counts or exclusive time.
// A scope must outlive all operations it observes; worker threads need their
// own scopes. This is deliberately independent of STANLI_NO_INTERPRETER.
struct ExecutionTrace {
  std::map<std::pair<std::string, std::string>, uint64_t> events;
  bool forbid_mir = false;
  bool violation = false;
  void check() const;
};

class ExecutionTraceScope {
 public:
  explicit ExecutionTraceScope(ExecutionTrace& trace);
  ~ExecutionTraceScope();
  ExecutionTraceScope(const ExecutionTraceScope&) = delete;
  ExecutionTraceScope& operator=(const ExecutionTraceScope&) = delete;

 private:
  friend void record_interpreter_event(const std::string&, const char*);
  ExecutionTraceScope* previous_;
  ExecutionTrace& trace_;
};

void record_interpreter_event(const std::string& phase, const char* event);
bool execution_reporting_enabled();
void report_execution_function(const std::string& name, bool compiled,
                               const std::string& refusal, int registers);

// Inspect only the final selected model, before its graphs are moved into
// Executors. This does not execute/probe the model, predict dynamic paths, or
// infer a tape-free kernel from its opcode. Hosts report their later selection
// separately. JSON schema is versioned independently of the runtime C ABI.
std::string execution_report(const CompiledModel& model,
                             const ExecutionTrace* trace = nullptr);
std::string execution_trace_report(const ExecutionTrace& trace);
// Optional host-selection event through the existing diagnostic sink. No
// environment lookup is added to evaluation/gradient loops.
void report_execution_host(const char* host, const char* value_engine,
                           const char* probe_status, const std::string& reason);
}  // namespace stanli
#endif

// Where a model's print() statements go.
//
// A Stan program's print() is the one thing the runtime emits that is
// neither a return value nor an exception, and a host embedding the
// runtime has to be able to catch it: redirect it into its own logging,
// interleave it with its own output, or drop it. Two paths produce it --
// the OP_PRINT kernel for the model block and the MIR interpreter for
// transformed data and interpreted generated quantities -- and they used
// to write to stdout independently, so there was nothing to redirect.
//
// Both now call emit_message. The default sink writes the line to stdout,
// which is what they did before, so nothing changes for a caller that
// installs nothing.
//
// The sink is process-global, which is what the hosts asking for it want
// (BridgeStan's bs_set_print_callback has the same scope) and what a
// kernel deep in a forward sweep can reach without threading a handle
// through every call. Installing and emitting are serialized, so a
// concurrent sampler's chains cannot interleave halves of two lines or
// call a sink that is being replaced.
#ifndef STANLI_MESSAGE_SINK_HPP
#define STANLI_MESSAGE_SINK_HPP

#include <cstddef>
#include <functional>
#include <string>

namespace stanli {

// text is NOT null-terminated and carries no trailing newline: a sink
// writing to a stream adds one, a sink collecting lines does not have to
// strip one.
using MessageSink = std::function<void(const char* text, std::size_t len)>;

// Install a sink, or pass nullptr to restore the stdout default.
void set_message_sink(MessageSink sink);

// One print() line, from either path.
void emit_message(const std::string& text);

// Hold messages instead of writing them to stdout, for a host that prints
// them itself. The sampler emits from worker threads, which a host such as
// R or Python must not print from, so the host calls take_captured_messages
// on its own thread: each message comes back as its line plus a newline, in
// the order it was emitted. An installed sink still takes precedence.
void set_message_capture(bool on);
std::string take_captured_messages();

// Install a sink for emit_diagnostic, or pass nullptr to restore the
// stderr default.
void set_diagnostic_sink(MessageSink sink);

// One operator-facing trace line: STANLI_DEBUG_* traces, the preparation
// profiler, and structured-loop diagnostics.
void emit_diagnostic(const std::string& text);

}  // namespace stanli

#endif

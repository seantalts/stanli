#include <stanli/message_sink.hpp>

#include <cstdio>
#include <mutex>
#include <utility>

namespace stanli {

namespace {

std::mutex& sink_mutex() {
  static std::mutex m;
  return m;
}

MessageSink& sink() {
  static MessageSink s;
  return s;
}

}  // namespace

void set_message_sink(MessageSink s) {
  std::lock_guard<std::mutex> lock(sink_mutex());
  sink() = std::move(s);
}

void emit_message(const std::string& text) {
  std::lock_guard<std::mutex> lock(sink_mutex());
  if (sink()) {
    sink()(text.data(), text.size());
    return;
  }
  // The default, and what both paths did before there was a sink: the
  // line and its newline to stdout. One fwrite per call rather than two,
  // so a line cannot be split by another thread's output.
  std::string line = text;
  line += '\n';
  std::fwrite(line.data(), 1, line.size(), stdout);
}

}  // namespace stanli

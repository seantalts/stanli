#include <stanli/message_sink.hpp>

#include <atomic>
#include <cstdio>
#include <mutex>
#include <utility>

namespace stanli {

namespace {

std::mutex& sink_mutex() {
  static std::mutex m;
  return m;
}

MessageSink& current_sink() {
  static MessageSink s;
  return s;
}

bool& capturing() {
  static bool on = false;
  return on;
}

std::string& captured() {
  static std::string text;
  return text;
}

std::atomic<bool>& captured_pending() {
  static std::atomic<bool> pending{false};
  return pending;
}

std::mutex& diagnostic_mutex() {
  static std::mutex m;
  return m;
}

MessageSink& current_diagnostic_sink() {
  static MessageSink s;
  return s;
}

}  // namespace

void set_message_sink(MessageSink s) {
  std::lock_guard<std::mutex> lock(sink_mutex());
  current_sink() = std::move(s);
}

void emit_message(const std::string& text) {
  std::lock_guard<std::mutex> lock(sink_mutex());
  if (current_sink()) {
    current_sink()(text.data(), text.size());
    return;
  }
  if (capturing()) {
    captured().append(text);
    captured() += '\n';
    captured_pending().store(true, std::memory_order_release);
    return;
  }
#ifndef STANLI_NO_STDIO
  // The default, and what both paths did before there was a sink: the
  // line and its newline to stdout. One fwrite per call rather than two,
  // so a line cannot be split by another thread's output.
  std::string line = text;
  line += '\n';
  std::fwrite(line.data(), 1, line.size(), stdout);
#endif
}

void set_message_capture(bool on) {
  std::lock_guard<std::mutex> lock(sink_mutex());
  capturing() = on;
}

std::string take_captured_messages() {
  if (!captured_pending().load(std::memory_order_acquire)) return {};
  std::lock_guard<std::mutex> lock(sink_mutex());
  std::string out;
  out.swap(captured());
  captured_pending().store(false, std::memory_order_relaxed);
  return out;
}

void set_diagnostic_sink(MessageSink s) {
  std::lock_guard<std::mutex> lock(diagnostic_mutex());
  current_diagnostic_sink() = std::move(s);
}

void emit_diagnostic(const std::string& text) {
  std::lock_guard<std::mutex> lock(diagnostic_mutex());
  if (current_diagnostic_sink()) {
    current_diagnostic_sink()(text.data(), text.size());
    return;
  }
#ifndef STANLI_NO_STDIO
  std::string line = text;
  line += '\n';
  std::fwrite(line.data(), 1, line.size(), stderr);
#endif
}

}  // namespace stanli

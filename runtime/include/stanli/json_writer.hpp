#ifndef STANLI_JSON_WRITER_HPP
#define STANLI_JSON_WRITER_HPP

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace stanli {

class JsonWriter {
 public:
  void StartObject() { open('{'); }
  void EndObject() { close('}'); }
  void StartArray() { open('['); }
  void EndArray() { close(']'); }
  void Key(const char* key) {
    separate();
    quote(key, std::strlen(key));
    out_.push_back(':');
    after_key_ = true;
  }
  void String(const char* s, size_t n) {
    value();
    quote(s, n);
  }
  void String(const char* s) { String(s, std::strlen(s)); }
  void Null() { literal("null"); }
  void Bool(bool b) { literal(b ? "true" : "false"); }
  void Int64(int64_t v) {
    value();
    out_ += std::to_string(v);
  }
  void Uint64(uint64_t v) {
    value();
    out_ += std::to_string(v);
  }
  const std::string& str() const { return out_; }

 private:
  void separate() {
    if (!has_member_.empty()) {
      if (has_member_.back()) out_.push_back(',');
      has_member_.back() = true;
    }
  }
  void value() {
    if (after_key_)
      after_key_ = false;
    else
      separate();
  }
  void open(char c) {
    value();
    out_.push_back(c);
    has_member_.push_back(false);
  }
  void close(char c) {
    has_member_.pop_back();
    out_.push_back(c);
  }
  void literal(const char* text) {
    value();
    out_ += text;
  }
  void quote(const char* s, size_t n) {
    static const char hex[] = "0123456789ABCDEF";
    out_.push_back('"');
    for (size_t i = 0; i < n; ++i) {
      const unsigned char c = static_cast<unsigned char>(s[i]);
      switch (c) {
        case '"':
          out_ += "\\\"";
          break;
        case '\\':
          out_ += "\\\\";
          break;
        case '\b':
          out_ += "\\b";
          break;
        case '\f':
          out_ += "\\f";
          break;
        case '\n':
          out_ += "\\n";
          break;
        case '\r':
          out_ += "\\r";
          break;
        case '\t':
          out_ += "\\t";
          break;
        default:
          if (c < 0x20) {
            out_ += "\\u00";
            out_.push_back(hex[c >> 4]);
            out_.push_back(hex[c & 15]);
          } else {
            out_.push_back(static_cast<char>(c));
          }
      }
    }
    out_.push_back('"');
  }

  std::string out_;
  std::vector<bool> has_member_;
  bool after_key_ = false;
};

}  // namespace stanli

#endif

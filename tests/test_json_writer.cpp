#include <stanli/json_writer.hpp>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

namespace {
int failures = 0;

struct Reference {
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> w{buffer};
  void StartObject() { w.StartObject(); }
  void EndObject() { w.EndObject(); }
  void StartArray() { w.StartArray(); }
  void EndArray() { w.EndArray(); }
  void Key(const char* k) { w.Key(k); }
  void String(const char* s, size_t n) {
    w.String(s, static_cast<rapidjson::SizeType>(n));
  }
  void String(const char* s) { w.String(s); }
  void Null() { w.Null(); }
  void Bool(bool b) { w.Bool(b); }
  void Int64(int64_t v) { w.Int64(v); }
  void Uint64(uint64_t v) { w.Uint64(v); }
  std::string str() const { return buffer.GetString(); }
};

template <class W>
std::string run(void (*scenario)(W&)) {
  W w;
  scenario(w);
  return w.str();
}

template <class W>
void strings_in_array(W& w, const std::vector<std::string>& values) {
  w.StartObject();
  w.Key("values");
  w.StartArray();
  for (const auto& v : values) w.String(v.data(), v.size());
  w.EndArray();
  w.EndObject();
}

std::vector<std::string> sample_strings() {
  std::vector<std::string> v = {
      "",
      "plain",
      "with \"quotes\"",
      "back\\slash",
      "line\nbreak\r\ttab\b\f",
      "slash/ and del\x7f",
      "caf\xc3\xa9 \xe2\x82\xac \xf0\x9f\x98\x80",
      "invalid \xff\xfe \xc3",
      std::string("nul\0inside", 10),
      "\"\\\"\\\\\"",
  };
  for (int c = 0; c < 256; ++c) {
    v.push_back(std::string(1, static_cast<char>(c)));
    v.push_back("a" + std::string(1, static_cast<char>(c)) + "b");
  }
  std::string all;
  for (int c = 0; c < 256; ++c) all.push_back(static_cast<char>(c));
  v.push_back(all);
  v.push_back(std::string(5000, 'x') + "\n" + std::string(5000, '"'));
  return v;
}

template <class W>
void strings(W& w) {
  strings_in_array(w, sample_strings());
}

template <class W>
void numbers(W& w) {
  w.StartObject();
  w.Key("int64");
  w.StartArray();
  for (int64_t v :
       {int64_t(0), int64_t(1), int64_t(-1), int64_t(9), int64_t(10),
        int64_t(-10), int64_t(1234567890123LL), int64_t(-1234567890123LL),
        std::numeric_limits<int64_t>::max(),
        std::numeric_limits<int64_t>::min(),
        std::numeric_limits<int64_t>::min() + 1})
    w.Int64(v);
  w.EndArray();
  w.Key("uint64");
  w.StartArray();
  for (uint64_t v :
       {uint64_t(0), uint64_t(7), uint64_t(10), uint64_t(4294967296ULL),
        std::numeric_limits<uint64_t>::max()})
    w.Uint64(v);
  w.EndArray();
  w.Key("flags");
  w.StartArray();
  w.Bool(true);
  w.Bool(false);
  w.Null();
  w.EndArray();
  w.EndObject();
}

template <class W>
void nesting(W& w) {
  w.StartObject();
  w.Key("schema_version");
  w.Int64(1);
  w.Key("empty_object");
  w.StartObject();
  w.EndObject();
  w.Key("empty_array");
  w.StartArray();
  w.EndArray();
  w.Key("null");
  w.Null();
  w.Key("operations");
  w.StartArray();
  for (int i = 0; i < 3; ++i) {
    w.StartObject();
    w.Key("site");
    w.String("log_prob/op/0");
    w.Key("inputs");
    w.StartArray();
    for (int j = 0; j < i; ++j) w.Int64(j * 100);
    w.EndArray();
    w.Key("region");
    w.StartObject();
    w.Key("flag");
    w.Bool(i % 2 == 0);
    w.Key("children");
    w.StartArray();
    w.StartObject();
    w.EndObject();
    w.StartArray();
    w.EndArray();
    w.EndArray();
    w.EndObject();
    w.EndObject();
  }
  w.EndArray();
  w.Key("k\"ey\n");
  w.String("v");
  w.Key("last");
  w.String("a c string");
  w.EndObject();
}

template <class W>
void cstring_values(W& w) {
  w.StartObject();
  w.Key("empty");
  w.String("");
  w.Key("text");
  w.String("na\"me\\x");
  w.Key("truncated");
  const char buf[] = {'a', 'b', '\0', 'c', '\0'};
  w.String(buf);
  w.EndObject();
}

void compare(const char* name, const std::string& expected,
             const std::string& actual) {
  if (expected != actual) {
    ++failures;
    std::printf("FAIL %s\n  expected: %.200s\n  actual:   %.200s\n", name,
                expected.c_str(), actual.c_str());
  }
}
}  // namespace

int main() {
  compare("strings", run<Reference>(strings<Reference>),
          run<stanli::JsonWriter>(strings<stanli::JsonWriter>));
  compare("numbers", run<Reference>(numbers<Reference>),
          run<stanli::JsonWriter>(numbers<stanli::JsonWriter>));
  compare("nesting", run<Reference>(nesting<Reference>),
          run<stanli::JsonWriter>(nesting<stanli::JsonWriter>));
  compare("cstring", run<Reference>(cstring_values<Reference>),
          run<stanli::JsonWriter>(cstring_values<stanli::JsonWriter>));
  if (!failures) std::puts("test_json_writer OK");
  return failures ? 1 : 0;
}

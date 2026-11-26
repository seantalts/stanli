// Fuzzes the "whiten" member of the BridgeStan "__stanli" manifest, which
// bs_model_construct reads from the caller's data JSON. The harness
// supplies the build id, because the reader refuses a manifest from another
// build before it looks at "whiten".
//
// With an even first byte, the rest of the input is the member's JSON text.
// With an odd one, the rest encodes k and the arrays, and the harness writes
// the JSON and base64 itself. Mutations of that form change the sizes and
// widths directly, which random edits of base64 text seldom do.
#include "../../runtime/src/build_id.hpp"

#include <stanli/bridgestan_internal.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <type_traits>

namespace {

struct Bytes {
  const uint8_t* p;
  size_t left;

  // Reads past the end as zero bytes.
  template <class T>
  T take() {
    T v{};
    const size_t m = std::min(left, sizeof(T));
    std::memcpy(&v, p, m);
    p += m;
    left -= m;
    return v;
  }
};

std::string base64(const std::string& bytes) {
  static const char* digits =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  for (size_t i = 0; i < bytes.size(); i += 3) {
    uint32_t word = 0;
    const size_t m = std::min<size_t>(3, bytes.size() - i);
    for (size_t j = 0; j < 3; ++j)
      word = word << 8 | (j < m ? (uint8_t)bytes[i + j] : 0u);
    for (size_t j = 0; j < 4; ++j)
      out.push_back(j <= m ? digits[(word >> (18 - 6 * j)) & 63] : '=');
  }
  return out;
}

// One array member. Its length is `expected` unless the length byte has its
// top bit set, which gives the length in the low bits instead. Entries come
// from one signed byte each: indices as is, values scaled by 1/4.
template <class T>
std::string member(Bytes& in, const char* name, int64_t expected) {
  const auto code = in.take<uint8_t>();
  const int64_t len = code & 0x80 ? code & 0x7f : expected;
  std::string bytes;
  for (int64_t i = 0; i < len; ++i) {
    const auto b = in.take<int8_t>();
    T v;
    if constexpr (std::is_floating_point_v<T>)
      v = b * 0.25;
    else
      v = b;
    bytes.append(reinterpret_cast<const char*>(&v), sizeof v);
  }
  return std::string("\"") + name + "\": \"" + base64(bytes) + "\", ";
}

std::string structured(Bytes in) {
  const int64_t n = in.take<uint8_t>() % 9;
  const auto tag = in.take<uint8_t>();
  const int64_t k = tag & 1 ? in.take<int64_t>() : tag >> 1;
  const int64_t nnz = in.take<uint8_t>() % 33;
  // An n by k array that could not exist is written short.
  int64_t nk = 0;
  if (k < 0 || __builtin_mul_overflow(n, k, &nk) || nk > 4096) nk = 1;
  std::string s = "{";
  s += member<double>(in, "q_hat", n);
  s += member<int64_t>(in, "perm", n);
  s += member<int64_t>(in, "col_ptr", n + 1);
  s += member<int64_t>(in, "row_idx", nnz);
  s += member<double>(in, "values", nnz);
  s += member<double>(in, "c", n);
  s += member<double>(in, "b", nk);
  s += member<double>(in, "w", nk);
  return s + "\"k\": " + std::to_string(k) + "}";
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  static const std::string head = std::string("{\"build_id\": \"") +
                                  stanli::runtime_build_id() +
                                  "\", \"mir\": \"x\", \"whiten\": ";
  if (size == 0) return 0;
  const std::string whiten =
      data[0] & 1
          ? structured(Bytes{data + 1, size - 1})
          : std::string(reinterpret_cast<const char*>(data) + 1, size - 1);
  stanli::BsManifest man;
  std::string err;
  stanli::bs_read_manifest(head + whiten + "}", &man, &err);
  return 0;
}

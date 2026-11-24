// Runtime data container: built programmatically by tests, parsed from JSON
// with CmdStan's conventions, or copied out of a Stan var_context.
#ifndef STANLI_DATA_HPP
#define STANLI_DATA_HPP

#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

// Declared, not included: a caller who never builds a DataMap from a
// var_context needs neither the stan headers on its include path nor
// data_var_context.cpp in its build.
namespace stan {
namespace io {
class var_context;
}
}  // namespace stan

namespace stanli {

class DataMap {
 public:
  struct Entry {
    bool is_int = false;
    std::vector<double> r;
    std::vector<int> i;
    std::vector<int64_t> dims;  // empty = scalar
  };

  void set_int(const std::string& name, long v) {
    Entry e;
    e.is_int = true;
    e.i = {static_cast<int>(v)};
    e.r = {static_cast<double>(v)};  // ints are also usable as reals
    m_[name] = std::move(e);
  }
  void set_real(const std::string& name, double v) {
    Entry e;
    e.r = {v};
    m_[name] = std::move(e);
  }
  // `dims` defaults to one axis of length v.size(), so a scalar needs
  // set_int or set_real instead. Pass `dims` for a nested array, whose values
  // are the first-index-fastest flattening the JSON reader also produces:
  // one convention for every rank, which the lowering permutes into graph
  // order once, at materialization.
  void set_int_array(const std::string& name, std::vector<int> v,
                     std::vector<int64_t> dims = {}) {
    Entry e;
    e.is_int = true;
    e.dims = dims.empty() ? std::vector<int64_t>{static_cast<int64_t>(v.size())}
                          : std::move(dims);
    e.r.assign(v.begin(), v.end());  // ints are also usable as reals
    e.i = std::move(v);
    m_[name] = std::move(e);
  }
  void set_real_array(const std::string& name, std::vector<double> v,
                      std::vector<int64_t> dims = {}) {
    Entry e;
    e.dims = dims.empty() ? std::vector<int64_t>{static_cast<int64_t>(v.size())}
                          : std::move(dims);
    e.r = std::move(v);
    m_[name] = std::move(e);
  }

  bool has(const std::string& name) const { return m_.count(name) > 0; }
  const Entry& at(const std::string& name) const {
    auto it = m_.find(name);
    if (it == m_.end())
      throw std::runtime_error("data: variable not provided: " + name);
    return it->second;
  }
  const std::map<std::string, Entry>& entries() const { return m_; }

  // Each factory lives in its own translation unit -- data.cpp for the JSON
  // pair, data_var_context.cpp for this one -- so a build can drop either.
  static DataMap from_json_file(const std::string& path);
  static DataMap from_json(const std::string& text);
  static DataMap from_var_context(const stan::io::var_context& context);

 private:
  std::map<std::string, Entry> m_;
};

// Whether two lists of dims are equal once every extent of 1 is removed from
// both. Removing an axis of length 1 does not change the order in which the
// values are stored, so a value that passes holds the declared elements in
// the declared order.
inline bool same_without_unit_extents(const std::vector<int64_t>& a,
                                      const std::vector<int64_t>& b) {
  size_t i = 0, j = 0;
  for (;;) {
    while (i < a.size() && a[i] == 1) ++i;
    while (j < b.size() && b[j] == 1) ++j;
    if (i == a.size() || j == b.size()) return i == a.size() && j == b.size();
    if (a[i++] != b[j++]) return false;
  }
}

// Which of CmdStan's var_context rules a value's dims are checked by.
enum class VarStage { Data, ParameterInit };

// CmdStan's check of a value's dims against its declaration, with Stan's
// messages. For data it is stan::json::json_data::validate_dims: a scalar
// counts as zero elements, and any empty value satisfies a declaration with
// a zero extent, because JSON's [] cannot say which axis is empty. For
// starting values it is stan::io::validate_dims, which has no such escape:
// CmdStan rejects [] for a matrix[0, 3] and accepts only its absence. In
// both, a transposed matrix or a flat list for an array is rejected rather
// than read in the wrong order. Unlike CmdStan, stanli also accepts any value
// whose dims differ from the declared ones only by extents of 1, such as a
// scalar for vector[1], [x] for a real or an N x 1 matrix for vector[N].
// R drops or adds such extents (a length-1 vector has no dim, and scale(x)
// returns a matrix), so R users cannot always control them. The caller binds
// the value with the declared dims. `int_declared` adds Stan's check that an
// int variable holds ints.
inline void check_declared_dims(VarStage stage, const std::string& name,
                                const std::vector<int64_t>& declared,
                                const DataMap::Entry& found,
                                bool int_declared) {
  if (stage == VarStage::Data) {
    int64_t want = 1;
    for (int64_t d : declared) want *= d;
    int64_t have = found.dims.empty() ? 0 : 1;
    for (int64_t d : found.dims) have *= d;
    if (have == 0 && want == 0) return;
  }
  const auto tuple = [](const std::vector<int64_t>& dims) {
    std::string s = "(";
    for (size_t k = 0; k < dims.size(); ++k) {
      if (k) s += ',';
      s += std::to_string(dims[k]);
    }
    return s + ")";
  };
  const std::string where = std::string("; processing stage=") +
                            (stage == VarStage::Data ? "data" : "parameter") +
                            " initialization; variable name=" + name;
  const std::string shapes = "; dims declared=" + tuple(declared) +
                             "; dims found=" + tuple(found.dims);
  if (!same_without_unit_extents(declared, found.dims)) {
    if (found.dims.size() != declared.size())
      throw std::invalid_argument(
          "mismatch in number dimensions declared and found in context" +
          where + shapes);
    for (size_t k = 0; k < declared.size(); ++k)
      if (found.dims[k] != declared[k])
        throw std::invalid_argument(
            "mismatch in dimension declared and found in context" + where +
            "; position=" + std::to_string(k) + shapes);
  }
  if (int_declared && !found.is_int)
    throw std::invalid_argument("int variable contained non-int values" +
                                where + "; base type=int");
}

}  // namespace stanli

#endif

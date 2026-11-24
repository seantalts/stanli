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
  // `dims` defaults to one axis; pass it for a nested array, whose values
  // are the first-index-fastest flattening the JSON reader also produces --
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

// Whether `found` is a scalar given for a declaration of rank 1 or more whose
// extents are all 1.
inline bool scalar_for_ones(const std::vector<int64_t>& declared,
                            const DataMap::Entry& found) {
  if (!found.dims.empty() || declared.empty()) return false;
  for (int64_t d : declared)
    if (d != 1) return false;
  return true;
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
// than read in the wrong order. Unlike CmdStan, stanli accepts a scalar for a
// declaration whose extents are all 1, because R cannot write a length-1
// array; the caller binds it with the declared dims. `int_declared` adds
// Stan's check that an int variable holds ints.
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
  if (!scalar_for_ones(declared, found)) {
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

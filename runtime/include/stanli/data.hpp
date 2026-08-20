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

  // Each factory lives in its own translation unit -- data.cpp for the JSON
  // pair, data_var_context.cpp for this one -- so a build can drop either.
  static DataMap from_json_file(const std::string& path);
  static DataMap from_json(const std::string& text);
  static DataMap from_var_context(const stan::io::var_context& context);

 private:
  std::map<std::string, Entry> m_;
};

}  // namespace stanli

#endif

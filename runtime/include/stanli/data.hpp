// Runtime data container: built programmatically by tests, or parsed from
// JSON with CmdStan's conventions.
#ifndef STANLI_DATA_HPP
#define STANLI_DATA_HPP

#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

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
  void set_int_array(const std::string& name, std::vector<int> v) {
    Entry e;
    e.is_int = true;
    e.dims = {static_cast<int64_t>(v.size())};
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

  static DataMap from_json_file(const std::string& path);
  static DataMap from_json(const std::string& text);

 private:
  std::map<std::string, Entry> m_;
};

}  // namespace stanli

#endif

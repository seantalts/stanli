#include <stanli/data.hpp>

#include "../third_party/nlohmann_json.hpp"

#include <fstream>
#include <functional>
#include <sstream>

namespace stanli {

using nlohmann::json;

static DataMap::Entry entry_from_json(const std::string& name, const json& v) {
  DataMap::Entry e;
  if (v.is_number_integer()) {
    e.is_int = true;
    e.i = {v.get<int>()};
    e.r = {v.get<double>()};  // int scalars are usable wherever reals are
    return e;
  }
  if (v.is_number()) {
    e.r = {v.get<double>()};
    return e;
  }
  if (v.is_array()) {
    if (v.empty()) {
      // An empty array is vacuously all-int (R's integer(0) arrives as []),
      // and its empty real side satisfies a real declaration just as well,
      // so claiming int here never misleads: empty satisfies both types.
      e.is_int = true;
      e.dims = {0};
      return e;
    }
    if (v[0].is_array()) {
      // Nested array -> matrix (row-major), or deeper arrays flattened with
      // dims outer-to-inner.
      std::vector<int64_t> dims;
      const json* cur = &v;
      while (cur->is_array() && !cur->empty()) {
        dims.push_back(static_cast<int64_t>(cur->size()));
        cur = &(*cur)[0];
      }
      e.dims = dims;
      if (dims.size() == 2) {
        // Column-major, the Stan/Eigen convention (and what stanc's data
        // reconstruction loops assume for the flat read buffer).
        const int64_t R = dims[0], C = dims[1];
        e.r.resize(R * C);
        bool all_int = true;
        for (int64_t i = 0; i < R; ++i)
          for (int64_t j = 0; j < C; ++j) {
            e.r[j * R + i] = v[i][j].get<double>();
            if (!v[i][j].is_number_integer()) all_int = false;
          }
        if (all_int) {
          e.is_int = true;
          e.i.resize(R * C);
          for (int64_t i = 0; i < R; ++i)
            for (int64_t j = 0; j < C; ++j) e.i[j * R + i] = v[i][j].get<int>();
        }
        return e;
      }
      // N-D (>2): column-major like everything else (first index fastest).
      {
        const std::vector<int64_t>& D = e.dims;
        std::vector<int64_t> stride(D.size());
        int64_t total = 1;
        for (size_t d = 0; d < D.size(); ++d) {
          stride[d] = total;
          total *= D[d];
        }
        e.r.assign(total, 0.0);
        bool all_int = true;
        std::vector<int64_t> ix(D.size(), 0);
        std::function<void(const json&, size_t)> walk = [&](const json& node,
                                                            size_t depth) {
          if (depth == D.size()) {
            int64_t flatpos = 0;
            for (size_t d = 0; d < D.size(); ++d) flatpos += ix[d] * stride[d];
            e.r[flatpos] = node.get<double>();
            if (!node.is_number_integer()) all_int = false;
            return;
          }
          for (size_t k = 0; k < node.size(); ++k) {
            ix[depth] = (int64_t)k;
            walk(node[k], depth + 1);
          }
        };
        walk(v, 0);
        if (all_int) {
          e.is_int = true;
          e.i.resize(total);
          for (int64_t k = 0; k < total; ++k) e.i[k] = (int)e.r[k];
        }
        return e;
      }
    }
    bool all_int = true;
    for (const auto& k : v)
      if (!k.is_number_integer()) all_int = false;
    e.dims = {static_cast<int64_t>(v.size())};
    if (all_int) {
      e.is_int = true;
      for (const auto& k : v) e.i.push_back(k.get<int>());
      // Int arrays also usable as reals.
      for (const auto& k : v) e.r.push_back(k.get<double>());
    } else {
      for (const auto& k : v) e.r.push_back(k.get<double>());
    }
    return e;
  }
  throw std::runtime_error("data: unsupported JSON value for " + name);
}

DataMap DataMap::from_json(const std::string& text) {
  json root = json::parse(text);
  if (!root.is_object())
    throw std::runtime_error("data: top-level JSON must be an object");
  DataMap d;
  for (auto it = root.begin(); it != root.end(); ++it)
    d.m_[it.key()] = entry_from_json(it.key(), it.value());
  return d;
}

DataMap DataMap::from_json_file(const std::string& path) {
  std::ifstream f(path);
  if (!f) throw std::runtime_error("data: cannot open " + path);
  std::ostringstream ss;
  ss << f.rdbuf();
  return from_json(ss.str());
}

}  // namespace stanli

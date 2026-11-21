// The BridgeStan C ABI over stanli.
//
// BridgeStan's premise is "a Stan model as a differentiable shared
// library". Reference BridgeStan gets there by compiling the model into
// the library; stanli ships one universal library and the model arrives
// at construct time, as a manifest embedded in the data argument under
// "__stanli" (see bs_model_construct), so the same clients work with no
// C++ toolchain on the user's machine.
//
// The compatibility stance is narrow on purpose: every call either behaves
// exactly as the reference header documents, or returns -1 with a message
// saying what stanli cannot do. Nothing here serves a plausible number for
// flags it did not implement -- a sampler that silently gets log_prob with
// no Jacobian where it asked for one produces a wrong posterior and no
// error, which is far worse than a refusal at load time.
//
// The unsupported set, all refusing honestly:
//   * bs_log_density_hessian / ..._vector_product (no second derivatives),
//   * any density flags other than propto=true, jacobian=true (that is the
//     single quantity a stanli graph bakes in).
//
// One handle is evaluated from many threads -- walnutpie runs a jthread
// per chain over a shared bs_model -- so every evaluation borrows an
// executor from an ExecutorPool for the length of the call. The RNG is the
// caller's, in a bs_rng handle, so two chains drawing generated quantities
// through one model do not share a stream.
// BS_PUBLIC is __declspec(dllimport) on Windows unless this is defined,
// which would make these definitions, and any reference to them, look for
// a DLL that does not exist. The build sets it for everything that links
// the runtime; this keeps the file correct when compiled on its own.
#ifndef BRIDGESTAN_EXPORT
#define BRIDGESTAN_EXPORT 1
#endif
#include "../ext/bridgestan.h"

#include <stanli/bridgestan_internal.hpp>
#include <stanli/compile.hpp>
#include <stanli/execution_report.hpp>
#include <stanli/executor_pool.hpp>
#include <stanli/graph.hpp>
#include <stanli/message_sink.hpp>
#include <stanli/nuts.hpp>
#include <stanli/optable.hpp>
#include <stanli/rejection.hpp>
#include <stanli/wa_interp.hpp>
#include <stanli/whiten.hpp>

#include "build_id.hpp"
#include "copy_doubles.hpp"
#include "data_document.hpp"

#include <boost/random/uniform_real_distribution.hpp>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// The version of the reference header vendored in runtime/ext.
#define STANLI_BS_VERSION_STR "2.9.0"

namespace {

void set_error(char** error_msg, const std::string& what) {
  if (error_msg == nullptr) return;
  char* p = static_cast<char*>(std::malloc(what.size() + 1));
  if (p != nullptr) std::memcpy(p, what.c_str(), what.size() + 1);
  *error_msg = p;
}

// Every refusal takes this shape: exactly -1, with a message that says
// what stanli cannot do rather than what went wrong.
int refuse(char** error_msg, const std::string& what) {
  set_error(error_msg, what);
  return -1;
}

const char kFlagLimitation[] =
    "stanli supports only propto=true, jacobian=true: its graphs bake in "
    "the dropped constants and the change-of-variables terms, so the other "
    "flag combinations would have to be a different compilation of the "
    "model rather than a different call";

// `data` per the ABI: null or empty for no data, a path ending in ".json",
// or a JSON literal.
stanli::DataMap load_data(const char* data) {
  if (data == nullptr || data[0] == '\0') return stanli::DataMap();
  const std::string s(data);
  if (s.size() >= 5 && s.compare(s.size() - 5, 5, ".json") == 0)
    return stanli::DataMap::from_json_file(s);
  return stanli::DataMap::from_json(s);
}

// A flat constrained block, split into the named values the init interpreter
// reads. `theta` is bs_param_num(m, false, false) values: the constrained
// parameters only, in CSV order, which is each parameter's serial order in
// declaration order -- exactly what `views` describes.
std::map<std::string, stanli::DataMap::Entry> inits_from_flat(
    const stanli::CompiledModel& cm, const double* theta);

std::string join_csv(const std::vector<std::string>& names) {
  std::string s;
  for (const auto& n : names) {
    if (!s.empty()) s += ',';
    s += n;
  }
  return s;
}

// The print callback the client installed, read by the message sink.
// Installing and emitting are serialized inside set_message_sink /
// emit_message, so a plain pointer is enough.
STREAM_CALLBACK g_print_callback = nullptr;

}  // namespace

// The model handle. `class` rather than `struct` to match the reference
// header's forward declaration.
class bs_model {
 public:
  stanli::CompiledModel cm;  // graph moved out into ex
  std::unique_ptr<stanli::Executor> ex;
  std::unique_ptr<stanli::ExecutorPool> pool;
  // Set when the manifest asked for a whitening: the free vector is z.
  std::shared_ptr<const stanli::WhitenSpec> whiten;

  void unconstrain_json(const char* json, double* out) const {
    if (!whiten) return stanli::unconstrain_inits_json(cm, json, out);
    std::vector<double> q((size_t)whiten->n);
    stanli::unconstrain_inits_json(cm, json, q.data());
    whiten->to_z(q.data(), out);
  }
  void unconstrain_values(
      const std::map<std::string, stanli::DataMap::Entry>& inits,
      double* out) const {
    if (!whiten) return stanli::unconstrain_inits(cm, inits, out);
    std::vector<double> q((size_t)whiten->n);
    stanli::unconstrain_inits(cm, inits, q.data());
    whiten->to_z(q.data(), out);
  }

  // The write_array side: a second graph plus its own pool, or the
  // per-draw interpreter for the sections the graph cannot express.
  std::unique_ptr<stanli::Executor> wa_ex;
  std::unique_ptr<stanli::ExecutorPool> wa_pool;
  std::shared_ptr<stanli::WaInterp> wa_interp;

  // Every CSV column in order, and where its three sections meet.
  // Everything before n_tp_start is a constrained parameter,
  // [n_tp_start, n_gq_start) a transformed parameter, the rest a
  // generated quantity.
  std::vector<stanli::CompiledModel::ParamView> cols;
  size_t n_tp_start = 0;
  size_t n_gq_start = 0;

  std::string name = "stanli_model";
  std::string info;
  std::string unc_names;
  // One per (include_tp, include_gq) combination, indexed below. Owned by
  // the model and freed with it, as the header promises.
  std::string names[4];

  static size_t flag_index(bool tp, bool gq) {
    return (tp ? 1u : 0u) | (gq ? 2u : 0u);
  }

  // The column ranges a flag pair selects, in output order.
  std::vector<std::pair<size_t, size_t>> slices(bool tp, bool gq) const {
    std::vector<std::pair<size_t, size_t>> r;
    r.emplace_back(size_t(0), tp ? n_gq_start : n_tp_start);
    if (gq) r.emplace_back(n_gq_start, cols.size());
    return r;
  }

  int64_t count(bool tp, bool gq) const {
    int64_t n = 0;
    for (const auto& s : slices(tp, gq))
      for (size_t i = s.first; i < s.second; ++i) n += cols[i].len;
    return n;
  }

  // Every CSV column for one unconstrained draw, in column order. `rng`
  // may be null, which the ABI allows whenever generated quantities were
  // not requested.
  std::vector<double> full_row(const double* theta_unc,
                               stanli::WaRng* rng) const {
    std::vector<double> row;
    if (wa_interp) {
      auto lease = pool->acquire();
      stanli::copy_doubles(lease->params_data(), theta_unc, lease->n_params());
      lease->run_forward_only();
      // The interpreter has no per-section switch: one eval produces the
      // whole row, generated quantities included. A caller who asked for
      // no generated quantities passes no stream, so the discarded draws
      // come off a scratch one rather than off anybody's.
      stanli::WaRng scratch(1);
      // Safe to share one WaInterp between threads: its column discovery
      // ran at construction, and eval keeps its per-draw state local.
      return wa_interp->eval(cm.constrained_env(*lease),
                             rng == nullptr ? scratch : *rng);
    }
    stanli::ExecutorPool& p = wa_pool ? *wa_pool : *pool;
    auto lease = p.acquire();
    stanli::copy_doubles(lease->params_data(), theta_unc, lease->n_params());
    // The compiled graph, like WaInterp above, always evaluates the complete
    // row. Discarded generated quantities therefore draw from a scratch
    // stream when the caller did not request them.
    stanli::WaRng scratch(1);
    lease->run_forward_only(stanli::EvalState{rng == nullptr ? &scratch : rng});
    for (const auto& c : cols) {
      const double* v = std::as_const(*lease).value_ptr(c.slot);
      for (int64_t i = 0; i < c.len; ++i) row.push_back(v[c.storage_index(i)]);
    }
    return row;
  }

  // The constrained parameters alone, from the log_prob graph, which runs
  // the model block and checks the transformed parameters' bounds but runs
  // no generated quantities.
  void params_row(const double* theta_unc, double* out) const {
    auto lease = pool->acquire();
    stanli::copy_doubles(lease->params_data(), theta_unc, lease->n_params());
    lease->run_forward_only();
    for (const auto& v : cm.views) {
      const double* p = std::as_const(*lease).value_ptr(v.slot);
      for (int64_t i = 0; i < v.len; ++i) *out++ = p[v.storage_index(i)];
    }
  }

  // forward(), or forward + reverse when grad is non-null.
  double density(const double* theta_unc, double* grad) const {
    auto lease = pool->acquire();
    stanli::copy_doubles(lease->params_data(), theta_unc, lease->n_params());
    return grad == nullptr ? lease->forward() : lease->gradient(grad);
  }
};

class bs_rng {
 public:
  explicit bs_rng(unsigned int seed) : rng(seed) {}
  stanli::WaRng rng;
};

namespace stanli {

namespace {

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "the whitening manifest stores little-endian bytes"
#endif

std::string base64_decode(const std::string& text, const char* field) {
  auto value = [](unsigned char c) -> int {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
  };
  const std::string bad = std::string("whiten.") + field + " is not base64";
  if (text.size() % 4 != 0) throw std::runtime_error(bad);
  std::string out;
  out.reserve(text.size() / 4 * 3);
  for (size_t i = 0; i < text.size(); i += 4) {
    const bool last = i + 4 == text.size();
    uint32_t word = 0;
    int pad = 0;
    for (size_t j = 0; j < 4; ++j) {
      const char c = text[i + j];
      int v = 0;
      if (c == '=' && last && j >= 2) {
        ++pad;
      } else {
        v = value(static_cast<unsigned char>(c));
        if (v < 0 || pad > 0) throw std::runtime_error(bad);
      }
      word = word << 6 | (uint32_t)v;
    }
    out.push_back(static_cast<char>(word >> 16));
    if (pad < 2) out.push_back(static_cast<char>(word >> 8));
    if (pad < 1) out.push_back(static_cast<char>(word));
  }
  return out;
}

template <class T>
std::vector<T> manifest_array(const rapidjson::Value& obj, const char* field,
                              bool required) {
  const auto it = obj.FindMember(field);
  if (it == obj.MemberEnd()) {
    if (required)
      throw std::runtime_error(std::string("whiten has no \"") + field +
                               "\" field");
    return {};
  }
  if (!it->value.IsString())
    throw std::runtime_error(std::string("whiten.") + field +
                             " is not a string");
  const std::string bytes = base64_decode(
      std::string(it->value.GetString(), it->value.GetStringLength()), field);
  if (bytes.size() % sizeof(T) != 0)
    throw std::runtime_error(std::string("whiten.") + field +
                             " is not a whole number of 8-byte values");
  std::vector<T> out(bytes.size() / sizeof(T));
  if (!out.empty()) std::memcpy(out.data(), bytes.data(), bytes.size());
  return out;
}

std::shared_ptr<const WhitenSpec> read_whiten(const rapidjson::Value& obj) {
  if (!obj.IsObject()) throw std::runtime_error("whiten is not an object");
  const auto q_hat = manifest_array<double>(obj, "q_hat", true);
  const auto perm = manifest_array<int64_t>(obj, "perm", true);
  const auto col_ptr = manifest_array<int64_t>(obj, "col_ptr", true);
  const auto row_idx = manifest_array<int64_t>(obj, "row_idx", true);
  const auto values = manifest_array<double>(obj, "values", true);
  int64_t k = 0;
  const auto kit = obj.FindMember("k");
  if (kit != obj.MemberEnd()) {
    if (!kit->value.IsInt64() || kit->value.GetInt64() < 0)
      throw std::runtime_error("whiten.k is not a non-negative integer");
    k = kit->value.GetInt64();
  }
  const auto c = manifest_array<double>(obj, "c", k > 0);
  const auto b = manifest_array<double>(obj, "b", k > 0);
  const auto w = manifest_array<double>(obj, "w", k > 0);
  const auto n = (int64_t)q_hat.size();
  if ((int64_t)perm.size() != n || (int64_t)col_ptr.size() != n + 1 ||
      row_idx.size() != values.size())
    throw std::runtime_error("whiten arrays disagree on the parameter count");
  if (k > 0 && ((int64_t)c.size() != n || (int64_t)b.size() != n * k ||
                (int64_t)w.size() != n * k))
    throw std::runtime_error("whiten low-rank arrays have the wrong sizes");
  return make_whiten_spec(
      q_hat.data(), n, perm.data(), col_ptr.data(), row_idx.data(),
      values.data(), (int64_t)values.size(), k, k > 0 ? c.data() : nullptr,
      k > 0 ? b.data() : nullptr, k > 0 ? w.data() : nullptr);
}

bool read_manifest(const rapidjson::Value& root, BsManifest* out,
                   std::string* err) {
  auto say = [&](const std::string& s) {
    if (err != nullptr) *err = s;
    return false;
  };
  auto text_field = [&](const char* key) -> const rapidjson::Value* {
    const auto it = root.FindMember(key);
    return it != root.MemberEnd() && it->value.IsString() ? &it->value
                                                          : nullptr;
  };
  if (!root.IsObject()) return say("manifest is not a JSON object");
  const rapidjson::Value* id_field = text_field("build_id");
  if (id_field == nullptr)
    return say("manifest has no string \"build_id\" field");
  const rapidjson::Value* mir_field = text_field("mir");
  if (mir_field == nullptr) return say("manifest has no string \"mir\" field");
  const std::string id(id_field->GetString(), id_field->GetStringLength());
  // A stale pair must fail loudly. The MIR dialect and the lowering that
  // reads it move together, so a manifest written by another build could
  // lower to something subtly different rather than not lower at all.
  if (id != runtime_build_id())
    return say("manifest was written by a different stanli build: it says \"" +
               id + "\", this runtime is \"" + std::string(runtime_build_id()) +
               "\"");
  out->build_id = id;
  out->mir.assign(mir_field->GetString(), mir_field->GetStringLength());
  const rapidjson::Value* name_field = text_field("name");
  out->name = name_field != nullptr ? std::string(name_field->GetString(),
                                                  name_field->GetStringLength())
                                    : std::string();
  if (out->name.empty()) out->name = "stanli_model";
  out->whiten = nullptr;
  const auto whiten = root.FindMember("whiten");
  if (whiten != root.MemberEnd()) {
    try {
      out->whiten = read_whiten(whiten->value);
    } catch (const std::exception& e) {
      return say(e.what());
    }
  }
  return true;
}

}  // namespace

bool bs_read_manifest(const std::string& text, BsManifest* out,
                      std::string* err) {
  rapidjson::Document root;
  try {
    root = parse_data_document(text);
  } catch (const std::exception& e) {
    if (err != nullptr)
      *err = std::string("manifest is not valid JSON: ") + e.what();
    return false;
  }
  return read_manifest(root, out, err);
}

}  // namespace stanli

namespace {

std::unique_ptr<stanli::Executor> whitened_executor(
    const stanli::Executor& inner,
    const std::shared_ptr<const stanli::WhitenSpec>& spec) {
  if (spec->n != inner.n_params())
    throw std::invalid_argument("the whitening has " + std::to_string(spec->n) +
                                " parameters, the model has " +
                                std::to_string(inner.n_params()));
  return std::make_unique<stanli::Executor>(
      stanli::whiten_graph(inner.graph(), spec), inner);
}

bs_model* model_from_data(
    const char* mir, stanli::DataMap dm, unsigned int seed, char** error_msg,
    const char* name,
    const std::shared_ptr<const stanli::WhitenSpec>& whiten = nullptr) {
  try {
    if (mir == nullptr) {
      set_error(error_msg, "null MIR text");
      return nullptr;
    }
    std::unique_ptr<bs_model> m(new bs_model());
    if (name != nullptr && name[0] != '\0') m->name = name;
    // Reference BridgeStan seeds transformed-data RNG calls with this, and
    // so does stanli's compile: the section runs once here, at construction.
    m->cm = stanli::compile_model(mir, dm, seed);
    dm = stanli::DataMap();
    m->ex.reset(new stanli::Executor(std::move(m->cm.graph)));
    m->cm.bind(*m->ex);
    m->cm.fills = {};
    if (whiten) {
      m->ex = whitened_executor(*m->ex, whiten);
      m->whiten = whiten;
    }
    m->pool.reset(new stanli::ExecutorPool(*m->ex));

    std::string probe_failure;
    if (m->cm.write_array) {
      auto& wa = *m->cm.write_array;
      if (wa.interp) {
        auto lease = m->pool->acquire();
        if (stanli::probe_wa_columns(*wa.interp, m->cm, *lease,
                                     probe_failure) >= 0) {
          m->wa_interp = wa.interp;
          m->cols = m->wa_interp->columns();
          m->n_tp_start = m->wa_interp->n_tp_start();
          m->n_gq_start = m->wa_interp->n_gq_start();
        }
      } else if (!wa.columns.empty()) {
        m->wa_ex.reset(new stanli::Executor(std::move(wa.graph)));
        wa.bind(*m->wa_ex);
        wa.fills = {};
        if (whiten) m->wa_ex = whitened_executor(*m->wa_ex, whiten);
        m->wa_pool.reset(new stanli::ExecutorPool(*m->wa_ex));
        m->cols = wa.columns;
        m->n_tp_start = wa.n_tp_start;
        m->n_gq_start = wa.n_gq_start;
      }
    }
    stanli::report_execution_host("bridgestan", m->cm, m->wa_interp != nullptr,
                                  m->wa_ex != nullptr, "parameters_only",
                                  probe_failure);
    if (!probe_failure.empty())
      throw std::runtime_error(
          stanli::interpreter_probe_error(m->cm, probe_failure));
    {
      const std::string note = stanli::interpreter_warning(m->cm);
      if (!note.empty()) stanli::emit_message(note);
    }
    if (m->cols.empty()) {
      // No write_array: the constrained parameters are the whole CSV, and
      // there is no tp or gq section.
      m->cols = m->cm.views;
      m->n_tp_start = m->cols.size();
      m->n_gq_start = m->cols.size();
    }

    // The name strings the ABI hands out are owned by the model, so they
    // are built once here rather than per call.
    for (int f = 0; f < 4; ++f) {
      const bool tp = (f & 1) != 0, gq = (f & 2) != 0;
      std::vector<std::string> flat;
      for (const auto& s : m->slices(tp, gq))
        for (size_t i = s.first; i < s.second; ++i)
          m->cols[i].append_names(flat);
      m->names[(size_t)f] = join_csv(flat);
    }
    {
      std::vector<std::string> flat;
      if (m->whiten) {
        for (int64_t i = 1; i <= m->whiten->n; ++i)
          flat.push_back("z." + std::to_string(i));
      } else {
        for (const auto& p : m->cm.unc_params) p.append_names(flat);
      }
      m->unc_names = join_csv(flat);
    }
    m->info = "stanli runtime (build id " +
              std::string(stanli::runtime_build_id()) +
              ") implementing the BridgeStan C ABI " STANLI_BS_VERSION_STR
              "; model \"" +
              m->name + "\"; " + std::to_string(m->cm.n_unconstrained) +
              " unconstrained parameters; -ffp-contract=off" +
              // BridgeStan's info reports the make invocation, so clients
              // (walnutpie among them) grep for the make-args spelling
              // "STAN_THREADS=true" and refuse to run multi-chain without it.
              (stanli::thread_safe_build() ? "; STAN_THREADS=true" : "") +
              "; lp__ matches CmdStan exactly";
    return m.release();
  } catch (const std::exception& e) {
    set_error(error_msg, e.what());
    return nullptr;
  } catch (...) {
    set_error(error_msg, "unknown error constructing the model");
    return nullptr;
  }
}

}  // namespace

bs_model* bs_model_from_mir_whitened(
    const char* mir, const char* data, unsigned int seed, char** error_msg,
    const char* name, std::shared_ptr<const stanli::WhitenSpec> whiten) {
  stanli::DataMap dm;
  try {
    dm = load_data(data);
  } catch (const std::exception& e) {
    set_error(error_msg, e.what());
    return nullptr;
  }
  return model_from_data(mir, std::move(dm), seed, error_msg, name, whiten);
}

bs_model* bs_model_from_mir(const char* mir, const char* data,
                            unsigned int seed, char** error_msg,
                            const char* name) {
  stanli::DataMap dm;
  try {
    dm = load_data(data);
  } catch (const std::exception& e) {
    set_error(error_msg, e.what());
    return nullptr;
  }
  return model_from_data(mir, std::move(dm), seed, error_msg, name);
}

extern "C" {

const int bs_major_version = 2;
const int bs_minor_version = 9;
const int bs_patch_version = 0;

bs_model* bs_model_construct(const char* data, unsigned int seed,
                             char** error_msg) {
  // The model arrives inside the data argument: the JSON carries the
  // manifest under the reserved key "__stanli" (never a data variable:
  // Stan identifiers begin with a letter). The key is validated,
  // stripped, and the rest is the model's data. No filesystem, no
  // dladdr, so one library serves every model, on every platform.
  //
  // This replaced the lib pair -- a per-model copy of the runtime with a
  // sidecar manifest found through dladdr -- which cost ~29 MB per model
  // and could not work on Windows. A caller that still hands us plain
  // data gets an error saying where the model should have been.
  try {
    if (data != nullptr && data[0] != '\0') {
      // Same resolution rule as load_data: a path ending in ".json" means
      // the file's contents, anything else is the JSON itself.
      std::string text(data);
      if (text.size() >= 5 && text.compare(text.size() - 5, 5, ".json") == 0) {
        std::ifstream f(text);
        if (!f) {
          set_error(error_msg, "could not read data file " + text);
          return nullptr;
        }
        std::ostringstream ss;
        ss << f.rdbuf();
        text = ss.str();
      }
      rapidjson::Document root = stanli::parse_data_document(text);
      if (root.IsObject()) {
        const auto member = root.FindMember("__stanli");
        if (member != root.MemberEnd()) {
          stanli::BsManifest man;
          std::string err;
          if (!stanli::read_manifest(member->value, &man, &err)) {
            set_error(error_msg, "embedded __stanli manifest: " + err);
            return nullptr;
          }
          root.EraseMember(member);
          return model_from_data(man.mir.c_str(),
                                 stanli::data_from_document(root), seed,
                                 error_msg, man.name.c_str(), man.whiten);
        }
      }
    }
  } catch (const std::exception& e) {
    set_error(error_msg, std::string("data argument: ") + e.what());
    return nullptr;
  } catch (...) {
    set_error(error_msg, "unknown error reading the embedded manifest");
    return nullptr;
  }
  set_error(error_msg,
            "the data argument carries no \"__stanli\" manifest: this "
            "runtime takes the model embedded in the data JSON "
            "(stanli.bridgestan_model() builds it), not a per-model "
            "library");
  return nullptr;
}

void bs_model_destruct(bs_model* m) { delete m; }

void bs_free_error_msg(char* error_msg) { std::free(error_msg); }

const char* bs_name(const bs_model* m) { return m->name.c_str(); }

const char* bs_model_info(const bs_model* m) { return m->info.c_str(); }

const char* bs_param_names(const bs_model* m, bool include_tp,
                           bool include_gq) {
  return m->names[bs_model::flag_index(include_tp, include_gq)].c_str();
}

const char* bs_param_unc_names(const bs_model* m) {
  return m->unc_names.c_str();
}

int bs_param_num(const bs_model* m, bool include_tp, bool include_gq) {
  return (int)m->count(include_tp, include_gq);
}

int bs_param_unc_num(const bs_model* m) { return (int)m->cm.n_unconstrained; }

int bs_param_constrain(const bs_model* m, bool include_tp, bool include_gq,
                       const double* theta_unc, double* theta, bs_rng* rng,
                       char** error_msg) {
  try {
    if (include_gq && rng == nullptr)
      return refuse(error_msg,
                    "bs_param_constrain needs a bs_rng when include_gq is "
                    "true: generated quantities may draw from it");
    // Neither block asked for: the log_prob graph gives the parameters
    // without running generated quantities, and the write_array row gives
    // them without running the model block. Either serves; the second is
    // tried when the first is rejected.
    if (!include_tp && !include_gq) {
      try {
        m->params_row(theta_unc, theta);
        return 0;
      } catch (const std::exception& e) {
        if (stanli::ends_run(e)) throw;
      }
    }
    // The write_array graph has no prefix that stops before generated
    // quantities, so asking for transformed parameters runs them too.
    const std::vector<double> row =
        m->full_row(theta_unc, rng == nullptr ? nullptr : &rng->rng);
    // Column index -> offset into the row, so a slice can be copied out.
    size_t at = 0, out = 0;
    std::vector<size_t> offset(m->cols.size() + 1, 0);
    for (size_t i = 0; i < m->cols.size(); ++i) {
      offset[i] = at;
      at += (size_t)m->cols[i].len;
    }
    offset[m->cols.size()] = at;
    if (row.size() != at)
      return refuse(error_msg, "write_array produced " +
                                   std::to_string(row.size()) + " values for " +
                                   std::to_string(at) + " columns");
    for (const auto& s : m->slices(include_tp, include_gq))
      for (size_t i = s.first; i < s.second; ++i)
        for (int64_t k = 0; k < m->cols[i].len; ++k)
          theta[out++] = row[offset[i] + (size_t)k];
    return 0;
  } catch (const std::exception& e) {
    return refuse(error_msg, e.what());
  } catch (...) {
    return refuse(error_msg, "unknown error in bs_param_constrain");
  }
}

// Keep the implementation-only initialization helpers in C++ linkage. GCC
// rejects a C++ declaration followed by a definition inside this exported
// C-linkage block, while Clang accepts the mismatch.
}  // extern "C"

namespace {

// The constrained columns as starting values, keyed as CmdStan's JSON names
// them. The columns of an array of tuples come element by element, which is
// how CmdStan's reader stores each leaf under its key.
std::map<std::string, stanli::DataMap::Entry> inits_from_flat(
    const stanli::CompiledModel& cm, const double* theta) {
  std::map<std::string, std::pair<std::string, std::vector<int64_t>>> keys;
  for (const auto& leaf : cm.param_leaves)
    if (leaf.column < 0) keys[leaf.own.key()] = {leaf.init_key, leaf.own.dims};
  std::map<std::string, stanli::DataMap::Entry> out;
  int64_t at = 0;
  for (const auto& view : cm.views) {
    const auto found = keys.find(view.key());
    const std::string& key =
        found == keys.end() ? view.key() : found->second.first;
    stanli::DataMap::Entry& e = out[key];
    e.dims = found == keys.end() ? view.dims : found->second.second;
    e.r.insert(e.r.end(), theta + at, theta + at + view.len);
    at += view.len;
  }
  return out;
}

}  // namespace

extern "C" {

int bs_param_unconstrain(const bs_model* m, const double* theta,
                         double* theta_unc, char** error_msg) {
  try {
    int64_t declared = 0;
    for (const auto& view : m->cm.views) declared += view.len;
    if (declared != (int64_t)m->count(false, false))
      return refuse(error_msg,
                    "the declared parameters hold " + std::to_string(declared) +
                        " constrained values but this model reports " +
                        std::to_string(m->count(false, false)) +
                        "; theta cannot be split");
    m->unconstrain_values(inits_from_flat(m->cm, theta), theta_unc);
    return 0;
  } catch (const std::exception& e) {
    return refuse(error_msg, e.what());
  } catch (...) {
    return refuse(error_msg, "unknown error in bs_param_unconstrain");
  }
}

int bs_param_unconstrain_json(const bs_model* m, const char* json,
                              double* theta_unc, char** error_msg) {
  try {
    m->unconstrain_json(json, theta_unc);
    return 0;
  } catch (const std::exception& e) {
    return refuse(error_msg, e.what());
  } catch (...) {
    return refuse(error_msg, "unknown error in bs_param_unconstrain_json");
  }
}

int bs_param_initialize(const bs_model* m, const char* json, bs_rng* rng,
                        double init_radius, int max_tries, bool jacobian,
                        double* theta_unc, char** error_msg) {
  try {
    if (!jacobian) return refuse(error_msg, kFlagLimitation);
    if (json != nullptr) {
      // An explicit point is taken as given: the retries below exist to find
      // a random start in support, and silently replacing a point the caller
      // chose would hide the real problem.
      m->unconstrain_json(json, theta_unc);
      double lp = 0;
      try {
        lp = m->density(theta_unc, nullptr);
      } catch (const std::exception& e) {
        return refuse(error_msg, std::string("the supplied starting point is "
                                             "outside the model's support: ") +
                                     e.what());
      }
      if (!std::isfinite(lp))
        return refuse(error_msg,
                      "the supplied starting point has a non-finite log "
                      "density");
      return 0;
    }
    if (rng == nullptr)
      return refuse(error_msg, "bs_param_initialize needs a bs_rng");
    if (!(init_radius >= 0.0))
      return refuse(error_msg, "init_radius must be non-negative");

    const int64_t n = m->cm.n_unconstrained;
    boost::random::uniform_real_distribution<double> unif(-init_radius,
                                                          init_radius);
    for (int t = 0; t < max_tries; ++t) {
      for (int64_t i = 0; i < n; ++i)
        theta_unc[(size_t)i] = unif(rng->rng.gen());
      double lp = 0;
      try {
        lp = m->density(theta_unc, nullptr);
      } catch (const std::exception&) {
        // A point out of the model's support is a rejected draw, not an
        // error: that is exactly what the retries are for.
        continue;
      }
      if (std::isfinite(lp)) return 0;
    }
    return refuse(error_msg,
                  "initialization failed to find a point with finite log "
                  "density in " +
                      std::to_string(max_tries) + " attempts within [-" +
                      std::to_string(init_radius) + ", " +
                      std::to_string(init_radius) + ")");
  } catch (const std::exception& e) {
    return refuse(error_msg, e.what());
  } catch (...) {
    return refuse(error_msg, "unknown error in bs_param_initialize");
  }
}

int bs_log_density(const bs_model* m, bool propto, bool jacobian,
                   const double* theta_unc, double* lp, char** error_msg) {
  if (!propto || !jacobian) return refuse(error_msg, kFlagLimitation);
  try {
    *lp = m->density(theta_unc, nullptr);
    return 0;
  } catch (const std::exception& e) {
    return refuse(error_msg, e.what());
  } catch (...) {
    return refuse(error_msg, "unknown error in bs_log_density");
  }
}

int bs_log_density_gradient(const bs_model* m, bool propto, bool jacobian,
                            const double* theta_unc, double* val, double* grad,
                            char** error_msg) {
  if (!propto || !jacobian) return refuse(error_msg, kFlagLimitation);
  try {
    *val = m->density(theta_unc, grad);
    return 0;
  } catch (const std::exception& e) {
    return refuse(error_msg, e.what());
  } catch (...) {
    return refuse(error_msg, "unknown error in bs_log_density_gradient");
  }
}

int bs_log_density_hessian(const bs_model* m, bool propto, bool jacobian,
                           const double* theta_unc, double* val, double* grad,
                           double* hessian, char** error_msg) {
  (void)m;
  (void)propto;
  (void)jacobian;
  (void)theta_unc;
  (void)val;
  (void)grad;
  (void)hessian;
  return refuse(error_msg,
                "bs_log_density_hessian is not available: stanli computes "
                "first derivatives only");
}

int bs_log_density_hessian_vector_product(const bs_model* m, bool propto,
                                          bool jacobian,
                                          const double* theta_unc,
                                          const double* vector, double* val,
                                          double* hvp, char** error_msg) {
  (void)m;
  (void)propto;
  (void)jacobian;
  (void)theta_unc;
  (void)vector;
  (void)val;
  (void)hvp;
  return refuse(error_msg,
                "bs_log_density_hessian_vector_product is not available: "
                "stanli computes first derivatives only");
}

bs_rng* bs_rng_construct(unsigned int seed, char** error_msg) {
  try {
    return new bs_rng(seed);
  } catch (const std::exception& e) {
    set_error(error_msg, e.what());
    return nullptr;
  }
}

void bs_rng_destruct(bs_rng* rng) { delete rng; }

int bs_set_print_callback(STREAM_CALLBACK callback, char** error_msg) {
  try {
    g_print_callback = callback;
    if (callback == nullptr) {
      stanli::set_message_sink(nullptr);
      return 0;
    }
    stanli::set_message_sink([](const char* text, std::size_t len) {
      STREAM_CALLBACK cb = g_print_callback;
      if (cb == nullptr) return;
      // The sink delivers one line with no terminator; reference
      // BridgeStan hands the client the bytes Stan wrote to the print
      // stream, and Stan's print() ends its line. Put the newline back so
      // a client concatenating chunks reconstructs the same text.
      std::string line(text, len);
      line.push_back('\n');
      cb(line.data(), line.size());
    });
    return 0;
  } catch (const std::exception& e) {
    return refuse(error_msg, e.what());
  } catch (...) {
    return refuse(error_msg, "unknown error in bs_set_print_callback");
  }
}

}  // extern "C"

// The BridgeStan C ABI over stanli.
//
// BridgeStan's premise is "a Stan model as a differentiable shared
// library". Reference BridgeStan gets there by compiling the model into
// the library; stanli ships one universal library and puts the model in a
// sidecar manifest next to a clone of it, so the same clients work with no
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
//   * bs_param_unconstrain / bs_param_unconstrain_json and
//     bs_param_initialize with non-null JSON (stanli has forward
//     constraint transforms only),
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
#include "../third_party/bridgestan.h"

#include <stanli/bridgestan_internal.hpp>
#include <stanli/compile.hpp>
#include <stanli/executor_pool.hpp>
#include <stanli/graph.hpp>
#include <stanli/message_sink.hpp>
#include <stanli/nuts.hpp>
#include <stanli/optable.hpp>
#include <stanli/wa_interp.hpp>

#include "../third_party/nlohmann_json.hpp"

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

#ifndef _WIN32
#include <dlfcn.h>
#endif

// Identifies this runtime binary. Read from the compile definition rather
// than through stanli_build_id(), because capi.cpp is not part of the
// static library this file also lands in; both targets set the macro from
// the same CMake variable, so the two answers are the same string.
#ifndef STANLI_BUILD_ID
#define STANLI_BUILD_ID "unknown"
#endif

// The version of the reference header vendored in runtime/third_party.
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

// The names of one declared parameter's UNCONSTRAINED values.
//
// These are not the constrained CSV names: a simplex[3] is three
// constrained values and two unconstrained ones, and a
// cholesky_factor_corr[K] is K*K against K*(K-1)/2. So the declared dims
// index the unconstrained vector only when they multiply out to its
// length; where they do not, the fallback is flat 1..len under the
// parameter's own name.
//
// The exact index shape reference BridgeStan uses for structured
// transforms is pinned later by the differential conformance test against
// a real BridgeStan build. This is the best the current metadata supports:
// lowering records the declared dims and the unconstrained length, not the
// unconstrained SHAPE, and inventing one would be a guess.
void append_unc_names(const stanli::CompiledModel::UncParam& p,
                      std::vector<std::string>& out) {
  int64_t prod = 1;
  bool sane = true;
  for (int64_t d : p.dims) {
    if (d < 0) sane = false;
    prod *= d;
  }
  if (sane && prod == p.len && !p.dims.empty()) {
    // Last-index-major, the way Stan serializes everything: the FIRST
    // index varies fastest, so matrix[2,3] m is m.1.1, m.2.1, m.1.2, ...
    std::vector<int64_t> idx(p.dims.size(), 0);
    for (int64_t k = 0; k < p.len; ++k) {
      std::string s = p.name;
      for (size_t d = 0; d < idx.size(); ++d)
        s += "." + std::to_string(idx[d] + 1);
      out.push_back(std::move(s));
      for (size_t d = 0; d < idx.size(); ++d) {
        if (++idx[d] < p.dims[d]) break;
        idx[d] = 0;
      }
    }
    return;
  }
  if (p.dims.empty() && p.len == 1) {
    out.push_back(p.name);
    return;
  }
  for (int64_t i = 0; i < p.len; ++i)
    out.push_back(p.name + "." + std::to_string(i + 1));
}

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
  unsigned int seed = 0;

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
      std::memcpy(lease->params_data(), theta_unc,
                  sizeof(double) * (size_t)lease->n_params());
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
    std::memcpy(lease->params_data(), theta_unc,
                sizeof(double) * (size_t)lease->n_params());
    lease->run_forward_only();
    for (const auto& c : cols) {
      const double* v = lease->value_ptr(c.slot);
      for (int64_t i = 0; i < c.len; ++i) row.push_back(v[c.storage_index(i)]);
    }
    return row;
  }

  // forward(), or forward + reverse when grad is non-null.
  double density(const double* theta_unc, double* grad) const {
    auto lease = pool->acquire();
    std::memcpy(lease->params_data(), theta_unc,
                sizeof(double) * (size_t)lease->n_params());
    return grad == nullptr ? lease->forward() : lease->gradient(grad);
  }
};

class bs_rng {
 public:
  explicit bs_rng(unsigned int seed) : rng(seed) {}
  stanli::WaRng rng;
};

namespace stanli {

const char* bs_build_id() { return STANLI_BUILD_ID; }

bool bs_read_manifest(const std::string& text, BsManifest* out,
                      std::string* err) {
  auto say = [&](const std::string& s) {
    if (err != nullptr) *err = s;
    return false;
  };
  nlohmann::json root;
  try {
    root = nlohmann::json::parse(text);
  } catch (const std::exception& e) {
    return say(std::string("manifest is not valid JSON: ") + e.what());
  }
  if (!root.is_object()) return say("manifest is not a JSON object");
  if (!root.contains("build_id") || !root["build_id"].is_string())
    return say("manifest has no string \"build_id\" field");
  if (!root.contains("mir") || !root["mir"].is_string())
    return say("manifest has no string \"mir\" field");
  const std::string id = root["build_id"].get<std::string>();
  // A stale pair must fail loudly. The MIR dialect and the lowering that
  // reads it move together, so a manifest written by another build could
  // lower to something subtly different rather than not lower at all.
  if (id != bs_build_id())
    return say("manifest was written by a different stanli build: it says \"" +
               id + "\", this runtime is \"" + std::string(bs_build_id()) +
               "\"");
  out->build_id = id;
  out->mir = root["mir"].get<std::string>();
  out->name = (root.contains("name") && root["name"].is_string())
                  ? root["name"].get<std::string>()
                  : std::string("stanli_model");
  if (out->name.empty()) out->name = "stanli_model";
  return true;
}

}  // namespace stanli

bs_model* bs_model_from_mir(const char* mir, const char* data,
                            unsigned int seed, char** error_msg,
                            const char* name) {
  try {
    if (mir == nullptr) {
      set_error(error_msg, "null MIR text");
      return nullptr;
    }
    std::unique_ptr<bs_model> m(new bs_model());
    if (name != nullptr && name[0] != '\0') m->name = name;
    // Reference BridgeStan seeds transformed-data RNG calls with this.
    // stanli's compiler rejects any _rng in transformed data outright
    // ("unsupported function ..._rng"), so the seed is recorded and that
    // CompileError is what a caller sees -- pretending to support it
    // would mean silently ignoring the seed.
    m->seed = seed;

    stanli::DataMap dm = load_data(data);
    m->cm = stanli::compile_model(mir, dm);
    m->ex.reset(new stanli::Executor(std::move(m->cm.graph)));
    m->cm.bind(*m->ex);
    m->pool.reset(new stanli::ExecutorPool(*m->ex));

    if (m->cm.write_array) {
      auto& wa = *m->cm.write_array;
      if (wa.interp) {
        // The interpreted section's columns only exist after one
        // evaluation, and a model can be out of support at one probe
        // point and fine at the next, so walk all three variants.
        m->wa_interp = wa.interp;
        std::vector<double> q((size_t)m->ex->n_params());
        bool found = false;
        for (int variant = 0; variant < 3 && !found; ++variant) {
          for (size_t i = 0; i < q.size(); ++i)
            q[i] = stanli::wa_probe_point((int64_t)i, variant);
          try {
            // A scratch stream: discovery is the runtime probing the
            // model, not a draw any caller asked for.
            stanli::WaRng probe(1);
            auto lease = m->pool->acquire();
            std::memcpy(lease->params_data(), q.data(),
                        sizeof(double) * q.size());
            lease->run_forward_only();
            (void)m->wa_interp->eval(m->cm.constrained_env(*lease), probe);
            found = true;
          } catch (const std::exception&) {
          }
        }
        if (found) {
          m->cols = m->wa_interp->columns();
          m->n_tp_start = m->wa_interp->n_tp_start();
          m->n_gq_start = m->wa_interp->n_gq_start();
        } else {
          m->wa_interp.reset();
        }
      } else if (!wa.columns.empty()) {
        m->wa_ex.reset(new stanli::Executor(std::move(wa.graph)));
        wa.bind(*m->wa_ex);
        m->wa_pool.reset(new stanli::ExecutorPool(*m->wa_ex));
        m->cols = wa.columns;
        m->n_tp_start = wa.n_tp_start;
        m->n_gq_start = wa.n_gq_start;
      }
    }
    if (m->cols.empty()) {
      // No write_array (or none that could be evaluated): the constrained
      // parameters are the whole CSV, and there is no tp or gq section.
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
      for (const auto& p : m->cm.unc_params) append_unc_names(p, flat);
      m->unc_names = join_csv(flat);
    }
    m->info = "stanli runtime (build id " + std::string(STANLI_BUILD_ID) +
              ") implementing the BridgeStan C ABI " STANLI_BS_VERSION_STR
              "; model \"" +
              m->name + "\"; " + std::to_string(m->cm.n_unconstrained) +
              " unconstrained parameters; -ffp-contract=off" +
              // BridgeStan's info reports the make invocation, so clients
              // (walnutpie among them) grep for the make-args spelling
              // "STAN_THREADS=true" and refuse to run multi-chain without it.
              (stanli::thread_safe_build() ? "; STAN_THREADS=true" : "") +
              (stanli::exact_lp_build()
                   ? "; lp__ matches CmdStan exactly"
                   : "; lp__ offset from CmdStan by a per-model constant");
    return m.release();
  } catch (const std::exception& e) {
    set_error(error_msg, e.what());
    return nullptr;
  } catch (...) {
    set_error(error_msg, "unknown error constructing the model");
    return nullptr;
  }
}

#ifndef _WIN32
// What dladdr resolves to this library's path. STATIC on purpose: an
// exported bs_ symbol can be interposed by another BridgeStan library
// already loaded in the process, and dladdr would then name that library's
// file instead of this one's -- so the model would be read from someone
// else's manifest.
static void bs_dladdr_anchor() {}
#endif

extern "C" {

const int bs_major_version = 2;
const int bs_minor_version = 9;
const int bs_patch_version = 0;

bs_model* bs_model_construct(const char* data, unsigned int seed,
                             char** error_msg) {
#ifdef _WIN32
  (void)data;
  (void)seed;
  set_error(error_msg,
            "bs_model_construct is not supported on this platform: finding "
            "the sidecar manifest next to this library needs dladdr");
  return nullptr;
#else
  try {
    Dl_info info;
    if (dladdr(reinterpret_cast<const void*>(&bs_dladdr_anchor), &info) == 0 ||
        info.dli_fname == nullptr || info.dli_fname[0] == '\0') {
      set_error(error_msg,
                "could not locate this library's own path: dladdr failed");
      return nullptr;
    }
    // <libstem>.stanli.json next to the library: /a/b/libmodel.dylib pairs
    // with /a/b/libmodel.stanli.json.
    const std::string lib = info.dli_fname;
    const size_t slash = lib.find_last_of('/');
    const size_t dot = lib.find_last_of('.');
    const bool has_ext =
        dot != std::string::npos && (slash == std::string::npos || dot > slash);
    const std::string path =
        (has_ext ? lib.substr(0, dot) : lib) + ".stanli.json";

    std::ifstream f(path);
    if (!f) {
      set_error(error_msg,
                "no stanli sidecar manifest at " + path +
                    ": this library was loaded directly rather than as a "
                    "model pair written by Model.bridgestan_lib()");
      return nullptr;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    stanli::BsManifest man;
    std::string err;
    if (!stanli::bs_read_manifest(ss.str(), &man, &err)) {
      set_error(error_msg, path + ": " + err);
      return nullptr;
    }
    return bs_model_from_mir(man.mir.c_str(), data, seed, error_msg,
                             man.name.c_str());
  } catch (const std::exception& e) {
    set_error(error_msg, e.what());
    return nullptr;
  } catch (...) {
    set_error(error_msg, "unknown error constructing the model");
    return nullptr;
  }
#endif
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

int bs_param_unconstrain(const bs_model* m, const double* theta,
                         double* theta_unc, char** error_msg) {
  (void)m;
  (void)theta;
  (void)theta_unc;
  return refuse(error_msg,
                "bs_param_unconstrain is not available: stanli implements "
                "the forward constraint transforms only, so it cannot map a "
                "constrained point back to the unconstrained scale");
}

int bs_param_unconstrain_json(const bs_model* m, const char* json,
                              double* theta_unc, char** error_msg) {
  (void)m;
  (void)json;
  (void)theta_unc;
  return refuse(error_msg,
                "bs_param_unconstrain_json is not available: stanli "
                "implements the forward constraint transforms only, so it "
                "cannot map a constrained point back to the unconstrained "
                "scale");
}

int bs_param_initialize(const bs_model* m, const char* json, bs_rng* rng,
                        double init_radius, int max_tries, bool jacobian,
                        double* theta_unc, char** error_msg) {
  try {
    if (json != nullptr)
      return refuse(error_msg,
                    "bs_param_initialize with an explicit JSON point needs "
                    "the inverse constraint transforms, which stanli does "
                    "not have; pass NULL to initialize randomly");
    if (!jacobian) return refuse(error_msg, kFlagLimitation);
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

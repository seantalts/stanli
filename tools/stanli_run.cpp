// stanli_run: model.stan + data.json -> posterior draws CSV on stdout.
// The full user path: gets MIR from stanc3, compiles the graph, samples
// with NUTS, emits constrained parameter draws.
//
// Usage: stanli_run model.stan data.json [--seed N] [--warmup N]
//        [--samples N] [--delta X] [--max-depth N] [--stanc PATH]
//        [--sampler-stats]
//
// Built with the stanc3 embed object this needs nothing else on the
// machine: no C++ toolchain, no separate compiler binary. Without it,
// --stanc (or $STANC) points at a stanc3 to shell out to.
//
// --sampler-stats prepends CmdStan's seven sampler columns (lp__,
// accept_stat__, stepsize__, treedepth__, n_leapfrog__, divergent__,
// energy__) to each CSV row, which is what tools/sampler_trace.py diffs
// against a real CmdStan run.
#include <stanli/compile.hpp>
#include <stanli/nuts.hpp>
#include <stanli/wa_interp.hpp>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <vector>

#ifdef STANLI_EMBED_STANC
extern "C" char* stanli_stanc_tmir(const char* stan_code);
extern "C" void stanli_stanc_free(char* p);

// The compiler linked into this binary. Returns the MIR, or throws with
// stanc's own error text -- there is no stanc binary to find, no
// subprocess, and no temp file.
static std::string embedded_stanc(const std::string& model) {
  std::string src;
  {
    std::unique_ptr<FILE, int (*)(FILE*)> f(std::fopen(model.c_str(), "rb"),
                                            std::fclose);
    if (!f) throw std::runtime_error("cannot read " + model);
    std::array<char, 1 << 16> buf;
    size_t n;
    while ((n = fread(buf.data(), 1, buf.size(), f.get())) > 0)
      src.append(buf.data(), n);
  }
  char* res = stanli_stanc_tmir(src.c_str());
  const std::string out(res ? res : "ERRstanc returned nothing");
  if (res) stanli_stanc_free(res);
  if (out.compare(0, 3, "ERR") == 0)
    throw std::runtime_error("stanc: " + out.substr(3));
  return out.substr(2);  // strip "OK"
}
#endif

static std::string run_stanc(const std::string& stanc,
                             const std::string& model) {
  const std::string cmd = stanc + " --debug-transformed-mir '" + model +
                          "' 2>/dev/null";
  std::unique_ptr<FILE, int (*)(FILE*)> pipe(popen(cmd.c_str(), "r"), pclose);
  if (!pipe) throw std::runtime_error("cannot run stanc: " + cmd);
  std::string out;
  std::array<char, 1 << 16> buf;
  size_t n;
  while ((n = fread(buf.data(), 1, buf.size(), pipe.get())) > 0)
    out.append(buf.data(), n);
  if (out.empty())
    throw std::runtime_error("stanc produced no MIR (compile error?)");
  return out;
}

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr,
                 "usage: stanli_run model.stan data.json [--seed N] "
                 "[--warmup N] [--samples N] [--delta X] "
                 "[--max-depth N] [--stanc PATH] [--sampler-stats]\n");
    return 2;
  }
  std::string model = argv[1], datafile = argv[2];
  std::string stanc = "deps/stanc3/stanc";
  bool stanc_explicit = false;
  stanli::NutsConfig cfg;
  cfg.seed = 1;
  cfg.warmup = 1000;
  cfg.samples = 1000;
  bool want_stats = false;
  for (int i = 3; i < argc; ++i) {
    const std::string k = argv[i];
    if (k == "--sampler-stats") { want_stats = true; continue; }
    if (i + 1 >= argc) break;
    const std::string v = argv[++i];
    if (k == "--seed") cfg.seed = (uint32_t)std::stoul(v);
    else if (k == "--warmup") cfg.warmup = std::stoi(v);
    else if (k == "--samples") cfg.samples = std::stoi(v);
    else if (k == "--delta") cfg.delta = std::stod(v);
    else if (k == "--max-depth") cfg.max_depth = std::stoi(v);
    else if (k == "--stanc") { stanc = v; stanc_explicit = true; }
  }
  if (const char* env = std::getenv("STANC")) {
    stanc = env;
    stanc_explicit = true;
  }

  try {
    stanli::DataMap data = stanli::DataMap::from_json_file(datafile);
    // The embedded compiler wins unless the caller named a stanc
    // explicitly (--stanc or $STANC), which is how a build with both can
    // still be pointed at a different stanc3 for a bisect.
#ifdef STANLI_EMBED_STANC
    const std::string mir =
        stanc_explicit ? run_stanc(stanc, model) : embedded_stanc(model);
#else
    const std::string mir = run_stanc(stanc, model);
#endif
    stanli::CompiledModel cm = stanli::compile_model(mir, data);
    stanli::Executor ex(std::move(cm.graph));
    cm.bind(ex);
    // STANLI_PROFILE=1: per-opcode accounting for the whole sampling run,
    // printed to stderr alongside the gradient-evaluation count.
    const char* prof_env = std::getenv("STANLI_PROFILE");
    if (prof_env && prof_env[0] != '0') ex.set_profile(true);
    stanli::SamplerStats stats;
    auto draws = stanli::run_nuts(ex, cfg, want_stats ? &stats : nullptr);

    // Draws are written through the write_array graph when there is one --
    // that is what supplies transformed parameters and generated quantities,
    // and it fixes the column order to CmdStan's. When the graph could not
    // express the whole section, the per-draw interpreter produces every
    // column instead (slower, but generated quantities run once per stored
    // draw). Without either we still report the constrained parameters the
    // log_prob graph already computes.
    stanli::WaInterp* wi =
        cm.write_array && cm.write_array->interp ? cm.write_array->interp.get()
                                                 : nullptr;
    const bool have_wa =
        !wi && cm.write_array && !cm.write_array->columns.empty();
    if (cm.write_array && !cm.write_array->truncated.empty())
      std::fprintf(stderr, "stanli_run: write_array %s: %s\n",
                   wi ? "interpreted (graph could not express)" : "truncated",
                   cm.write_array->truncated.c_str());
    std::unique_ptr<stanli::Executor> wex;
    if (have_wa) {
      wex = std::make_unique<stanli::Executor>(
          std::move(cm.write_array->graph));
      cm.write_array->bind(*wex);
    }

    // Interpreted rows are computed up front: the header needs the first
    // evaluation's column discovery, and the RNG stream runs across draws.
    const auto constrained_by_name = [&](const std::vector<double>& q) {
      for (size_t i = 0; i < q.size(); ++i) ex.params_data()[i] = q[i];
      ex.run_forward_only();
      std::map<std::string, stanli::DataMap::Entry> ps;
      for (const auto& v : cm.views) {
        stanli::DataMap::Entry en;
        const double* p = ex.value_ptr(v.slot);
        en.r.assign(p, p + v.len);
        if (v.rows > 0)
          en.dims = {v.rows, v.len / v.rows};
        else if (v.len > 1)
          en.dims = {v.len};
        ps[v.name] = std::move(en);
      }
      return ps;
    };
    std::vector<std::vector<double>> irows;
    if (wi) {
      wi->seed(cfg.seed);
      irows.reserve(draws.size());
      // A draw whose generated quantities cannot be evaluated is written
      // as NaNs and the run continues, which is what CmdStan does
      // (mcmc_writer.hpp catches domain_error, logs it, and pads the row).
      // Aborting instead threw away every draw of an otherwise good run:
      // one lognormal_rng on a marginal ODE solution took out the whole
      // CSV.
      size_t n_bad = 0;
      std::string first_bad;
      for (const auto& q : draws) {
        try {
          irows.push_back(wi->eval(constrained_by_name(q)));
        } catch (const std::domain_error& e) {
          if (n_bad++ == 0) first_bad = e.what();
          irows.emplace_back();  // widened to nan below, once cols is known
        }
      }
      if (n_bad)
        std::fprintf(stderr,
                     "stanli_run: %zu of %zu draws could not produce "
                     "generated quantities, written as nan: %s\n",
                     n_bad, draws.size(), first_bad.c_str());
    }

    const auto& cols =
        wi ? wi->columns() : (have_wa ? cm.write_array->columns : cm.views);
    stanli::Executor& out = have_wa ? *wex : ex;
    if (wi) {
      const size_t w = stanli::CompiledModel::csv_names(cols).size();
      for (auto& r : irows)
        if (r.size() != w)
          r.assign(w, std::numeric_limits<double>::quiet_NaN());
    }

    std::string hdr;
    if (want_stats)
      hdr = "lp__,accept_stat__,stepsize__,treedepth__,n_leapfrog__,"
            "divergent__,energy__";
    for (const auto& n : stanli::CompiledModel::csv_names(cols)) {
      if (!hdr.empty()) hdr += ',';
      hdr += n;
    }
    std::printf("%s\n", hdr.c_str());
    for (size_t d = 0; d < draws.size(); ++d) {
      bool first = true;
      if (want_stats) {
        for (double v : stats.rows[d]) {
          std::printf(first ? "%.17g" : ",%.17g", v);
          first = false;
        }
      }
      if (wi) {
        for (double v : irows[d]) {
          std::printf(first ? "%.17g" : ",%.17g", v);
          first = false;
        }
        std::printf("\n");
        continue;
      }
      const auto& q = draws[d];
      for (size_t i = 0; i < q.size(); ++i) out.params_data()[i] = q[i];
      out.run_forward_only();
      for (const auto& v : cols) {
        const double* p = out.value_ptr(v.slot);
        for (int64_t i = 0; i < v.len; ++i) {
          std::printf(first ? "%.17g" : ",%.17g", p[i]);
          first = false;
        }
      }
      std::printf("\n");
    }
    // Gradient evaluations = leapfrog steps + init probes. Reported so a
    // sampling-time comparison can be split into "cost per gradient" and
    // "how many gradients the sampler asked for", which are different
    // claims and can move in opposite directions.
    std::fprintf(stderr, "stanli_run: %lld gradient evaluations\n",
                 (long long)ex.n_grad_evals());
    const std::string prof = ex.profile_report();
    if (!prof.empty()) std::fprintf(stderr, "%s", prof.c_str());
  } catch (const std::exception& e) {
    std::fprintf(stderr, "stanli_run: %s\n", e.what());
    return 1;
  }
  return 0;
}

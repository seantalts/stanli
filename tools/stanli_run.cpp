// stanli_run: model.stan + data.json -> posterior draws CSV on stdout.
// The full user path: gets MIR from stanc3, compiles the graph, samples
// with NUTS, emits constrained parameter draws.
//
// Usage: stanli_run model.stan data.json [--seed N] [--warmup N]
//        [--samples N] [--delta X] [--max-depth N] [--stanc PATH]
//        [--sampler-stats] [--chains N] [--num-threads N] [--thin N]
//        [--save-warmup] [--init-radius X] [--summary]
//
// --chains runs N chains and concatenates their draws in chain order, so
// a reader that expects one chain still parses the CSV. --summary is
// where the chain structure is used: it prints stansummary's table and
// the convergence checks (divergences, treedepth saturation, E-BFMI,
// rank-normalized split-Rhat, bulk/tail ESS) to stderr, leaving stdout a
// clean CSV.
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
#include <stanli/diagnose.hpp>
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
  const std::string cmd =
      stanc + " --debug-transformed-mir '" + model + "' 2>/dev/null";
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
                 "[--max-depth N] [--stanc PATH] [--sampler-stats] "
                 "[--chains N] [--num-threads N] [--thin N] "
                 "[--save-warmup] [--init-radius X] [--summary]\n");
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
  bool want_summary = false;
  int n_chains = 1;
  // 0 means "one thread per chain", resolved once n_chains is known.
  // Threading does not change the draws -- they are byte-identical to a
  // sequential run -- so there is nothing to opt into.
  int n_threads = 0;
  bool threads_asked = false;
  for (int i = 3; i < argc; ++i) {
    const std::string k = argv[i];
    if (k == "--sampler-stats") {
      want_stats = true;
      continue;
    }
    if (k == "--summary") {
      want_summary = true;
      continue;
    }
    if (k == "--save-warmup") {
      cfg.save_warmup = true;
      continue;
    }
    if (i + 1 >= argc) break;
    const std::string v = argv[++i];
    if (k == "--seed")
      cfg.seed = (uint32_t)std::stoul(v);
    else if (k == "--warmup")
      cfg.warmup = std::stoi(v);
    else if (k == "--samples")
      cfg.samples = std::stoi(v);
    else if (k == "--delta")
      cfg.delta = std::stod(v);
    else if (k == "--max-depth")
      cfg.max_depth = std::stoi(v);
    else if (k == "--chains")
      n_chains = std::stoi(v);
    else if (k == "--num-threads") {
      n_threads = std::stoi(v);
      threads_asked = true;
    } else if (k == "--thin")
      cfg.thin = std::stoi(v);
    else if (k == "--init-radius")
      cfg.init_radius = std::stod(v);
    else if (k == "--stanc") {
      stanc = v;
      stanc_explicit = true;
    }
  }
  if (n_chains < 1) n_chains = 1;
  if (n_threads <= 0) n_threads = n_chains;
  // Asking for threads on a build that cannot honour them is worth a word:
  // the run is correct either way, but it will not be as fast as asked.
  if (threads_asked && n_threads > 1 && !stanli::thread_safe_build()) {
    std::fprintf(stderr,
                 "stanli_run: --num-threads %d ignored; this build is "
                 "single-threaded (stan-math's autodiff stack is not "
                 "thread-local without STAN_THREADS)\n",
                 n_threads);
    n_threads = 1;
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
    // One executor per chain: the arenas are per-evaluation mutable
    // state, and cloning copies the bound graph rather than lowering the
    // model again. Chain 0 runs on `ex` itself, so a one-chain run
    // allocates nothing extra and behaves exactly as it always has.
    auto clones = stanli::clone_executors(ex, n_chains - 1);
    std::vector<stanli::Executor*> execs{&ex};
    for (auto& c : clones) execs.push_back(c.get());
    auto chain_res = stanli::run_nuts_chains(execs, cfg, n_threads);
    for (size_t c = 0; c < chain_res.size(); ++c)
      if (!chain_res[c].error.empty())
        throw std::runtime_error("chain " + std::to_string(cfg.chain_id + c) +
                                 ": " + chain_res[c].error);

    // The CSV concatenates the chains in order, which is what the
    // single-chain readers already expect; --summary is where the chain
    // structure is actually used.
    std::vector<std::vector<double>> draws;
    stanli::SamplerStats stats;
    for (const auto& r : chain_res) {
      draws.insert(draws.end(), r.draws.begin(), r.draws.end());
      stats.rows.insert(stats.rows.end(), r.stats.rows.begin(),
                        r.stats.rows.end());
    }
    const int64_t per_chain =
        chain_res.empty() ? 0 : (int64_t)chain_res[0].draws.size();

    // Draws are written through the write_array graph when there is one --
    // that is what supplies transformed parameters and generated quantities,
    // and it fixes the column order to CmdStan's. When the graph could not
    // express the whole section, the per-draw interpreter produces every
    // column instead (slower, but generated quantities run once per stored
    // draw). Without either we still report the constrained parameters the
    // log_prob graph already computes.
    stanli::WaInterp* wi = cm.write_array && cm.write_array->interp
                               ? cm.write_array->interp.get()
                               : nullptr;
    const bool have_wa =
        !wi && cm.write_array && !cm.write_array->columns.empty();
    if (cm.write_array && !cm.write_array->truncated.empty())
      std::fprintf(stderr, "stanli_run: write_array %s: %s\n",
                   wi ? "interpreted (graph could not express)" : "truncated",
                   cm.write_array->truncated.c_str());
    std::unique_ptr<stanli::Executor> wex;
    if (have_wa) {
      wex =
          std::make_unique<stanli::Executor>(std::move(cm.write_array->graph));
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
    stanli::WaRng wa_rng(cfg.seed);
    if (wi) {
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
          irows.push_back(wi->eval(constrained_by_name(q), wa_rng));
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
      hdr =
          "lp__,accept_stat__,stepsize__,treedepth__,n_leapfrog__,"
          "divergent__,energy__";
    for (const auto& n : stanli::CompiledModel::csv_names(cols)) {
      if (!hdr.empty()) hdr += ',';
      hdr += n;
    }
    std::printf("%s\n", hdr.c_str());
    const std::vector<std::string> col_names =
        stanli::CompiledModel::csv_names(cols);
    // Only materialized for --summary: on the largest corpus models the
    // full draw matrix is hundreds of megabytes, and streaming is why the
    // default path can print them at all.
    std::vector<double> summary_draws;
    if (want_summary) summary_draws.reserve(draws.size() * col_names.size());

    std::vector<double> row;
    for (size_t d = 0; d < draws.size(); ++d) {
      row.clear();
      if (wi) {
        row = irows[d];
      } else {
        const auto& q = draws[d];
        for (size_t i = 0; i < q.size(); ++i) out.params_data()[i] = q[i];
        out.run_forward_only();
        for (const auto& v : cols) {
          const double* p = out.value_ptr(v.slot);
          row.insert(row.end(), p, p + v.len);
        }
      }
      bool first = true;
      if (want_stats) {
        for (double v : stats.rows[d]) {
          std::printf(first ? "%.17g" : ",%.17g", v);
          first = false;
        }
      }
      for (double v : row) {
        std::printf(first ? "%.17g" : ",%.17g", v);
        first = false;
      }
      std::printf("\n");
      if (want_summary)
        summary_draws.insert(summary_draws.end(), row.begin(), row.end());
    }

    if (want_summary && per_chain > 0) {
      // The draws were concatenated chain by chain above, which is
      // already the chain-major packing the diagnostics read.
      stanli::DrawSet ds{summary_draws.data(), (int64_t)chain_res.size(),
                         per_chain, (int64_t)col_names.size()};
      const auto sm = stanli::summarize(ds, col_names);
      std::fprintf(stderr, "\n%s\n", stanli::format_summary(sm).c_str());
      std::vector<double> flat_stats;
      flat_stats.reserve(stats.rows.size() * stanli::N_SAMPLER_COLS);
      for (const auto& r : stats.rows)
        flat_stats.insert(flat_stats.end(), r.begin(), r.end());
      const auto fd =
          stanli::diagnose(ds, sm, flat_stats.data(), cfg.max_depth);
      std::fprintf(stderr, "%s", stanli::format_diagnostics(fd).c_str());
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

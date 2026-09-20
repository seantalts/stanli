// stanli_run: model.stan + data.json -> posterior draws CSV on stdout.
// The full user path: gets MIR from stanc3, compiles the graph, samples
// with NUTS, emits constrained parameter draws.
//
// Usage: stanli_run model.stan data.json [--seed N] [--warmup N]
//        [--samples N] [--delta X] [--max-depth N]
//        [--stanli-compile PATH | --stanc PATH]
//        [--sampler-stats] [--chains N] [--num-threads N] [--threads-per-chain
//        N] [--thin N]
//        [--save-warmup] [--init-radius X] [--summary] [--timings]
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
// stanli-compile is found beside the executable or on PATH. --stanli-compile
// selects it explicitly; --stanc (or $STANC) selects stock stanc instead.
//
// --sampler-stats prepends CmdStan's seven sampler columns (lp__,
// accept_stat__, stepsize__, treedepth__, n_leapfrog__, divergent__,
// energy__) to each CSV row, which is what tools/sampler_trace.py diffs
// against a real CmdStan run.
#include <stanli/compile.hpp>
#include <stanli/diagnose.hpp>
#include <stanli/nuts.hpp>
#include <stanli/reduce_sum.hpp>
#include <stanli/optable.hpp>
#include <algorithm>
#include <stanli/wa_interp.hpp>

#include "chain_csv.hpp"
#include "stanc_embedded.hpp"
#include "stanc_process.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <vector>

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(
        stderr,
        "usage: stanli_run model.stan data.json [--seed N] "
        "[--warmup N] [--samples N] [--delta X] "
        "[--max-depth N] [--stanli-compile PATH | --stanc PATH] "
        "[--sampler-stats] "
        "[--chains N] [--num-threads N] [--threads-per-chain N] [--thin N] "
        "[--save-warmup] [--init-radius X] [--summary] [--timings]\n");
    return 2;
  }
  std::string model = argv[1], datafile = argv[2];
  std::string stanc;
  std::string compiler;
  stanli::NutsConfig cfg;
  cfg.seed = 1;
  cfg.warmup = 1000;
  cfg.samples = 1000;
  bool want_stats = false;
  bool want_summary = false;
  bool want_timings = false;
  int n_chains = 1;
  // 0 means "one thread per chain", resolved once n_chains is known.
  // Threading does not change the draws -- they are byte-identical to a
  // sequential run -- so there is nothing to opt into.
  int n_threads = 0;
  bool threads_asked = false;
  int threads_per_chain = 1;
  for (int i = 3; i < argc; ++i) {
    const std::string k = argv[i];
    if (k == "--timings") {
      want_timings = true;
      continue;
    }
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
    } else if (k == "--threads-per-chain")
      threads_per_chain = std::stoi(v);
    else if (k == "--thin")
      cfg.thin = std::stoi(v);
    else if (k == "--init-radius")
      cfg.init_radius = std::stod(v);
    else if (k == "--stanc")
      stanc = v;
    else if (k == "--stanli-compile")
      compiler = v;
  }
  if (threads_per_chain < 1) {
    std::fprintf(stderr, "stanli_run: --threads-per-chain must be positive\n");
    return 2;
  }
  if (threads_per_chain > 1 && !stanli::thread_safe_build()) {
    std::fprintf(stderr,
                 "stanli_run: within-chain threading requires a TLS-safe "
                 "native build\n");
    return 2;
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
    if (*env) stanc = env;
  }
  if (!stanc.empty() && !compiler.empty()) {
    std::fprintf(stderr,
                 "stanli_run: --stanc (or STANC) and --stanli-compile are "
                 "mutually exclusive\n");
    return 2;
  }

  try {
    using Clock = std::chrono::steady_clock;
    const auto timing_start = want_timings ? Clock::now() : Clock::time_point{};
    stanli::DataMap data = stanli::DataMap::from_json_file(datafile);
    const std::string mir =
        stanli::tooling::compile_source(stanc, compiler, model);
    if (mir.empty())
      throw std::runtime_error("the compiler produced no MIR (compile error?)");
    stanli::CompileOptions compile_options;
    compile_options.reduce_sum_threads = threads_per_chain;
    stanli::CompiledModel cm =
        stanli::compile_model(mir, data, cfg.seed, compile_options);
    const auto reductions = std::count_if(
        cm.graph.ops.begin(), cm.graph.ops.end(), [](const stanli::Op& op) {
          return op.opcode == stanli::OP_REDUCE_SUM;
        });
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
    // Only retained reductions need worker teams. Each executor owns its
    // mutable child state and borrows one persistent per-chain team.
    std::vector<std::unique_ptr<stanli::ReduceExecutionContext>> reduce_teams;
    if (threads_per_chain > 1) {
      std::fprintf(stderr,
                   "stanli_run: %td retained reductions; up to %lld active "
                   "sampling threads\n",
                   reductions,
                   static_cast<long long>(std::min(n_threads, n_chains)) *
                       (reductions ? threads_per_chain : 1));
      for (const auto& reason : cm.reduce_sum_fallbacks)
        std::fprintf(stderr, "stanli_run: serial reduce_sum: %s\n",
                     reason.c_str());
      if (reductions)
        for (auto* executor : execs) {
          reduce_teams.push_back(
              std::make_unique<stanli::ReduceExecutionContext>(
                  threads_per_chain));
          executor->set_reduce_context(reduce_teams.back().get());
        }
    }
    // Discover interpreted columns with a scratch stream, never the sampler's
    // RNG. Once discovered, WaInterp evaluation has no shared mutable state.
    auto wi = cm.write_array ? cm.write_array->interp : nullptr;
    const bool have_wa =
        !wi && cm.write_array && !cm.write_array->columns.empty();
    const std::string note = stanli::interpreter_warning(cm);
    if (!note.empty()) std::fprintf(stderr, "%s\n", note.c_str());
    bool columns_known = !wi;
    if (wi) {
      for (int variant = 0; variant < 3 && !columns_known; ++variant) {
        for (int64_t i = 0; i < ex.n_params(); ++i)
          ex.params_data()[i] = stanli::wa_probe_point(i, variant);
        try {
          ex.run_forward_only();
          stanli::WaRng probe_rng(1);
          wi->eval(cm.constrained_env(ex), probe_rng);
          columns_known = true;
        } catch (const std::exception&) {
          // The real chain may reach support where all probe points fail.
          // Such chains discover their columns independently on their first
          // successful row; failed prefixes stay in bounded CSV spools.
        }
      }
    }
    std::vector<std::string> col_names =
        columns_known
            ? stanli::CompiledModel::csv_names(wi ? wi->columns()
                                               : have_wa
                                                   ? cm.write_array->columns
                                                   : cm.views)
            : std::vector<std::string>{};
    const auto print_header = [&] {
      std::string header;
      if (want_stats)
        header =
            "lp__,accept_stat__,stepsize__,treedepth__,n_leapfrog__,"
            "divergent__,energy__";
      for (const auto& name : col_names) {
        if (!header.empty()) header += ',';
        header += name;
      }
      std::printf("%s\n", header.c_str());
    };
    if (columns_known) print_header();

    std::vector<std::unique_ptr<stanli::Executor>> wa_execs;
    if (have_wa) {
      wa_execs.push_back(
          std::make_unique<stanli::Executor>(std::move(cm.write_array->graph)));
      cm.write_array->bind(*wa_execs[0]);
      for (int c = 1; c < n_chains; ++c)
        wa_execs.push_back(std::make_unique<stanli::Executor>(*wa_execs[0]));
    }
    struct ChainOutput {
      std::shared_ptr<stanli::WaInterp> interp;
      std::unique_ptr<stanli::tooling::ChainCsv> csv;
      std::vector<double> row;
      std::vector<std::string> names;
      std::vector<double> late_summary;
      bool known = false;
      size_t pending_errors = 0;
      size_t errors = 0;
      int64_t stored = 0;
      std::string first_error;
    };
    const int64_t thin = std::max(1, cfg.thin);
    const int64_t expected =
        ((int64_t)cfg.samples + thin - 1) / thin +
        (cfg.save_warmup ? ((int64_t)cfg.warmup + thin - 1) / thin : 0);
    std::vector<double> summary_draws;
    if (want_summary && columns_known)
      summary_draws.resize((size_t)n_chains * expected * col_names.size());
    std::vector<ChainOutput> output((size_t)n_chains);
    for (int c = 0; c < n_chains; ++c) {
      auto& o = output[(size_t)c];
      o.known = columns_known;
      o.names = col_names;
      o.interp =
          wi && !columns_known ? std::make_shared<stanli::WaInterp>(*wi) : wi;
      o.csv =
          std::make_unique<stanli::tooling::ChainCsv>(columns_known && c == 0);
    }
    const stanli::StoredDrawWriter write =
        [&](int c, int64_t index, const double* q, stanli::WaRng& rng,
            const stanli::SamplerRow& stats) {
          auto& o = output[(size_t)c];
          ++o.stored;
          stanli::Executor& model_ex = *execs[(size_t)c];
          bool stream_arena = false;
          try {
            if (o.interp) {
              std::copy_n(q, model_ex.n_params(), model_ex.params_data());
              model_ex.run_forward_only();
              o.row = o.interp->eval(cm.constrained_env(model_ex), rng);
              if (!o.known) {
                o.names = stanli::CompiledModel::csv_names(o.interp->columns());
                o.known = true;
                if (want_summary)
                  o.late_summary.assign(
                      o.pending_errors * o.names.size(),
                      std::numeric_limits<double>::quiet_NaN());
              }
            } else {
              stanli::Executor& out = have_wa ? *wa_execs[(size_t)c] : model_ex;
              std::copy_n(q, out.n_params(), out.params_data());
              out.run_forward_only(stanli::EvalState{&rng});
              // CSV can read a completed graph's values directly. Materialize
              // a row only when a summary also needs to retain those values.
              stream_arena = !want_summary;
              if (!stream_arena) {
                o.row.clear();
                const auto& cols = have_wa ? cm.write_array->columns : cm.views;
                for (const auto& v : cols) {
                  const double* p = std::as_const(out).value_ptr(v.slot);
                  for (int64_t i = 0; i < v.len; ++i)
                    o.row.push_back(p[v.storage_index(i)]);
                }
              }
            }
            if (!stream_arena && o.row.size() != o.names.size())
              throw std::domain_error("write_array changed its output width");
          } catch (const std::domain_error& e) {
            if (o.errors++ == 0) o.first_error = e.what();
            if (!o.known) ++o.pending_errors;
            o.row.assign(o.names.size(),
                         std::numeric_limits<double>::quiet_NaN());
          }
          auto& csv = o.csv->writer();
          if (want_stats)
            for (double value : stats) csv.value(value);
          if (stream_arena) {
            const stanli::Executor& out =
                have_wa ? *wa_execs[(size_t)c] : model_ex;
            const auto& cols = have_wa ? cm.write_array->columns : cm.views;
            for (const auto& v : cols) {
              const double* p = out.value_ptr(v.slot);
              for (int64_t i = 0; i < v.len; ++i)
                csv.value(p[v.storage_index(i)]);
            }
          } else {
            for (double value : o.row) csv.value(value);
          }
          csv.end_row();
          if (want_summary && o.known) {
            if (columns_known)
              std::copy(o.row.begin(), o.row.end(),
                        summary_draws.begin() +
                            ((int64_t)c * expected + index) * col_names.size());
            else
              o.late_summary.insert(o.late_summary.end(), o.row.begin(),
                                    o.row.end());
          }
        };

    const auto prepared = want_timings ? Clock::now() : Clock::time_point{};
    cfg.retain_draws = false;  // the callback already writes/retains all output
    auto chain_res =
        stanli::run_nuts_chains(execs, cfg, n_threads, {}, {}, 1, {}, write);
    const auto sampled = want_timings ? Clock::now() : Clock::time_point{};
    for (size_t c = 0; c < chain_res.size(); ++c) {
      output[c].csv->finish();
      if (!chain_res[c].error.empty())
        throw std::runtime_error("chain " + std::to_string(cfg.chain_id + c) +
                                 ": " + chain_res[c].error);
    }
    const int64_t per_chain = output.empty() ? 0 : output[0].stored;
    if (!columns_known) {
      bool discovered = false;
      for (const auto& o : output)
        if (o.known) {
          col_names = o.names;
          discovered = true;
          break;
        }
      if (!discovered && per_chain > 0)
        throw std::runtime_error(
            "write_array column discovery failed on every probe and stored "
            "draw");
      print_header();
    }
    stanli::SamplerStats stats;
    for (size_t c = 0; c < output.size(); ++c) {
      auto& o = output[c];
      if (o.known && o.names != col_names)
        throw std::runtime_error(
            "write_array column names differ between chains");
      o.csv->copy_to(stdout, o.pending_errors, col_names.size());
      if (o.errors)
        std::fprintf(
            stderr,
            "stanli_run: chain %zu: %zu of %zu draws could not produce "
            "generated quantities, written as nan: %s\n",
            cfg.chain_id + c, o.errors, (size_t)o.stored,
            o.first_error.c_str());
      if (want_summary) {
        stats.rows.insert(stats.rows.end(), chain_res[c].stats.rows.begin(),
                          chain_res[c].stats.rows.end());
        if (!columns_known) {
          if (!o.known)
            o.late_summary.assign(o.stored * col_names.size(),
                                  std::numeric_limits<double>::quiet_NaN());
          summary_draws.insert(summary_draws.end(), o.late_summary.begin(),
                               o.late_summary.end());
        }
      }
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
    if (want_timings) {
      // Generated quantities and CSV formatting now run during sampling.
      // Output time covers ordered spool copying and the final flush.
      std::fflush(stdout);
      const auto written = Clock::now();
      const auto seconds = [](auto duration) {
        return std::chrono::duration<double>(duration).count();
      };
      std::fprintf(
          stderr,
          "stanli_run: timings prep_s=%.9g sample_s=%.9g output_s=%.9g\n",
          seconds(prepared - timing_start), seconds(sampled - prepared),
          seconds(written - sampled));
    }
  } catch (const std::exception& e) {
    std::fprintf(stderr, "stanli_run: %s\n", e.what());
    return 1;
  }
  return 0;
}

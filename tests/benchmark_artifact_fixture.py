"""Small synthetic v5 artifacts shared by renderer and publication tests."""
import copy
import csv
import gzip
import json
import statistics


POINTS = 3


def raw_trial(ns, protocol=5, dropped=()):
    """One driver output whose every finite point costs ns per gradient."""
    if protocol == 4:
        return dict(protocol="stanli-gradient-v2", iterations=3000000,
                    elapsed_ns=ns * 3000000, batch=1, warmup_iterations=2000000,
                    warmup_elapsed_ns=200000000, values=[-1.0, 2.0])
    points = []
    for k in range(POINTS):
        if k in dropped:
            points.append(dict(point=k, finite=False))
        else:
            points.append(dict(point=k, finite=True, values=[-1.0 - k, 2.0 + k],
                               iterations=1000000, elapsed_ns=ns * 1000000, batch=1))
    return dict(protocol="stanli-gradient-v3", warmup_iterations=200000,
                warmup_elapsed_ns=20000000, points=points)


def artifact_fixture(protocol=5, dropped=()):
    config = dict(corpus="all", filter="", rounds=6, gradient_budget=20000,
                  stancflags="", warmup_ms=200 if protocol == 4 else 20,
                  measure_ms=250 if protocol == 4 else 40, gradient_timeout=60)
    if protocol == 5:
        config["points"] = POINTS
    manifest = dict(run_id="current", started_utc="2026-09-21T00:00:00Z",
        identity=dict(protocol=f"stanli-corpus-v{protocol}", config=config,
            inputs={name: dict(stan=name + "-stan", data=name + "-data", collection="posteriordb")
                    for name in ("good", "failed")},
            executables=dict(bench="bench-hash", vectorize_probe="probe-hash", stanc="b" * 64),
            threads={"STAN_NUM_THREADS": "1"},
            sources={"harnesses/corpus_bench.py": "harness-hash", "tools/bench_cmdstan_grad.cpp": "driver-hash",
                     "tools/benchmark_timer.hpp": "timer-hash"},
            machine=dict(platform="macOS-arm64", host="host", processor="arm", logical_cpus=32)))
    pairs = []
    for index, (s, c) in enumerate(zip((100, 200, 300, 400, 500, 600),
                                      (1000, 300, 400, 900, 600, 800))):
        pair = dict(order=["stanli", "cmdstan"] if index % 2 == 0 else ["cmdstan", "stanli"],
                    max_scaled_error=0)
        if protocol == 5:
            pair.update(kept_points=[k for k in range(POINTS) if k not in dropped],
                        dropped_points=list(dropped))
        for engine, ns in (("stanli", s), ("cmdstan", c)):
            pair[engine] = raw_trial(ns, protocol, dropped)
        pairs.append(pair)
    row = dict(model="good", run_id="current", params=1, gradient_budget=20000,
               paired_rounds=6, stanli_compile_s=.1, stanli_prep_s=.02,
               cmdstan_stanc_s=.2, cmdstan_build_s=2, note="")
    if protocol == 5:
        row.update(points_kept=POINTS - len(dropped), points_dropped=len(dropped))
    for field, values in (("stanli_ns_grad", [100,200,300,400,500,600]),
                          ("cmdstan_ns_grad", [1000,300,400,900,600,800]),
                          ("paired_speedup", [10,1.5,4/3,2.25,1.2,4/3])):
        row[field] = statistics.median(values)
        row[field + "_mad"] = statistics.median(abs(value-row[field]) for value in values)
    row.update(stanli_estimated_s=.1 + .02 + 20000 * 350 / 1e9,
               cmdstan_estimated_s=.2 + 2 + 20000 * 700 / 1e9)
    setup = {key: dict(phase="good/" + phase, status="ok", returncode=0, elapsed_s=elapsed)
             for key, phase, elapsed in (("stanli_compile", "stanli-mir", .1),
                ("cmdstan_stanc", "stanc-cpp", .2), ("cmdstan_build", "cmdstan-build", 2))}
    records = [dict(model="good", inputs=dict(stan="good-stan", data="good-data"),
                    row=row, gradients=pairs, setup=setup, preparation_s=[.02] * 6, status="ok"),
               dict(model="failed", inputs=dict(stan="failed-stan", data="failed-data"),
                    row=dict(model="failed", run_id="current", gradient_budget=20000,
                             note="gradient build failed"),
                    gradients=[], setup={}, preparation_s=[], status="failed")]
    return manifest, records, [copy.deepcopy(record["row"]) for record in records]


def write_artifacts(summary, records_path, manifest_path, rows, records, manifest):
    fields = list(dict.fromkeys(key for row in rows for key in row))
    with summary.open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields, delimiter="\t")
        writer.writeheader()
        writer.writerows(rows)
    with gzip.open(records_path, "wt") as stream:
        json.dump(records, stream)
    manifest_path.write_text(json.dumps(manifest))

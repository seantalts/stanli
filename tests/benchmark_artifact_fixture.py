"""Small synthetic v4 artifacts shared by renderer and publication tests."""
import copy
import csv
import gzip
import json
import statistics


def artifact_fixture():
    config = dict(corpus="all", filter="", rounds=6, gradient_budget=20000,
                  stancflags="", warmup_ms=200, measure_ms=250, gradient_timeout=60)
    manifest = dict(run_id="current", started_utc="2026-09-21T00:00:00Z",
        identity=dict(protocol="stanli-corpus-v4", config=config,
            inputs={name: dict(stan=name + "-stan", data=name + "-data", collection="posteriordb")
                    for name in ("good", "failed")},
            executables=dict(bench="bench-hash", vectorize_probe="probe-hash", stanc="b" * 64),
            threads={"STAN_NUM_THREADS": "1"}))
    pairs = []
    for index, (s, c) in enumerate(zip((100, 200, 300, 400, 500, 600),
                                      (1000, 300, 400, 900, 600, 800))):
        pair = dict(order=["stanli", "cmdstan"] if index % 2 == 0 else ["cmdstan", "stanli"],
                    max_scaled_error=0)
        for engine, ns in (("stanli", s), ("cmdstan", c)):
            pair[engine] = dict(protocol="stanli-gradient-v2", iterations=3000000,
                elapsed_ns=ns * 3000000, batch=1, warmup_iterations=2000000,
                warmup_elapsed_ns=200000000, values=[-1.0, 2.0])
        pairs.append(pair)
    row = dict(model="good", run_id="current", params=1, gradient_budget=20000,
               paired_rounds=6, stanli_compile_s=.1, stanli_prep_s=.02,
               cmdstan_stanc_s=.2, cmdstan_build_s=2, note="")
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

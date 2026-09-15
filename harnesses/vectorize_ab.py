#!/usr/bin/env python3
"""Measure a source MIR pass against its pass-off oracle.

Each selected Stan source is compiled exactly twice by the test-only OCaml
probe. The default candidate is upstream loop vectorization; Stanli-owned
passes can use the same semantic and measurement gates. The saved portable MIR
is then reused for every semantic point and for four graph cells (source pass
off/on crossed with the C++ re-roll pass off/on).

Hard failures are semantic: result categories, vector shapes, write_array
names and shapes, nonfinite behavior, and the existing CmdStan reference
gates. Different finite bits within those gates are reported as arithmetic
order changes, with one bit-pattern/ULP row per changed off/on value.
Preparation timing and, for a model whose portable MIR the pass changes,
op counts (the lowered log_prob graph growing, or the final log_prob
graph growing by more than 10%, both in the runtime-reroll-on cells) are
diagnostics: they never fail the run on their own. The execution gate is
gradient time on the candidate-pass measurement set (GRADIENT_MODELS)
against the pass-off cell: a model whose on/off ratio exceeds
GRADIENT_RATIO_THRESHOLD is re-measured fresh, and only fails the run if
the re-run also exceeds it.

That gate, like the op-count diagnostic, compares a run's own pass-off
cell against its pass-on cell, so a change that moves both identically is
invisible to either. --baseline DIR diffs final ops, lowered ops, island
regions, and slots against a previous --output-dir's graphs.jsonl and
reports every model whose final ops grew or shrank; this comparison never
fails the run either.

Complete semantic report (130 recorded models plus PDB A/B-only models):
  python3 harnesses/vectorize_ab.py deps/posteriordb \
    --output-dir build/vectorize-ab

Bounded report:
  python3 harnesses/vectorize_ab.py deps/posteriordb \
    election88_full iohmm_reg dogs radon_county s2_nlf \
    one_comp_mm_elim_abs soil_incubation covid19imperial_v2 \
    --output-dir build/vectorize-ab

The no-model form covers the complete committed reference set, including
the language fixtures under tests/stanc3, plus every model in posteriordb's
one-data-per-model census. A model with no CmdStan reference is explicitly
reported as A/B-only. Gradient timing stays bounded to a small candidate-pass
measurement set in either form.
"""

import argparse
import datetime
import hashlib
import json
import math
import os
import pathlib
import platform
import re
import shutil
import statistics
import struct
import subprocess
import sys
import tempfile
import time

REPO = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "tools"))
from verify_refs import (KNOWN_GAPS, POINTS, accepted, corpus_input,  # noqa: E402
                         corpus_models, gate_for, load_refs, model_files,
                         pair_dev, parse_status, parse_wa, worst_pair)

VECTORIZE_LOOPS = "vectorize-loops"
CANDIDATE_PASSES = (VECTORIZE_LOOPS,)
EXPECTED_MIR_CHANGES = {
    VECTORIZE_LOOPS: frozenset((
        "covid19imperial_v2",
        "covid19imperial_v3",
        "normal_mixture",
        "radon_pooled",
        "soil_incubation",
    )),
}
GRADIENT_MODELS = {
    VECTORIZE_LOOPS: frozenset((
        "2pl_latent_reg_irt",
        "covid19imperial_v2",
        "covid19imperial_v3",
        "dogs",
        "dogs_log",
        "election88_full",
        "gpcm_latent_reg_irt",
        "grsm_latent_reg_irt",
        "iohmm_reg",
        "log10earn_height",
        "losscurve_sislob",
        "lotka_volterra",
        "lsat_model",
        "mother",
        "multi_occupancy",
        "normal_mixture",
        "one_comp_mm_elim_abs",
        "pilots",
        "radon_county",
        "radon_pooled",
        "s2_car",
        "s2_logistic_normal",
        "s2_nlf",
        "soil_incubation",
        "surgical_model",
        "sw_nonlinear",
    )),
}
FINAL_OPS_GROWTH_PERCENT = 10
# Models whose lowered op count grows with the pass on because the pass
# turns a zero-trip loop into an indexed assignment whose right-hand side
# Stan evaluates; the loop form evaluates nothing. The final graph is
# unchanged, so only the lowered-ops half of the gate is waived.
LOWERED_GROWTH_EXPECTED = frozenset(("s2_logistic_normal",))

GRADIENT_RATIO_THRESHOLD = 1.04

RUNTIME_ENV_KEYS = (
    "STANLI_DEBUG_ALGEBRA",
    "STANLI_DEBUG_INIT",
    "STANLI_DEBUG_ISLAND",
    "STANLI_DEBUG_ODE",
    "STANLI_ISLAND_ALWAYS",
    "STANLI_NO_CONSTFOLD",
    "STANLI_NO_CSE",
    "STANLI_NO_DATA_PRELOAD",
    "STANLI_NO_DENSITY_MASK",
    "STANLI_NO_INPLACE",
    "STANLI_NO_ISLAND",
    "STANLI_NO_ISLAND_COMPACT",
    "STANLI_NO_ISLAND_JOIN_GUARD",
    "STANLI_NO_NATIVE_ADJ",
    "STANLI_NO_PARTITION",
    "STANLI_NO_REROLL",
    "STANLI_PACKET_MATH",
    "STANLI_PROFILE",
    "STANLI_PROFILE_PREP",
    "STANLI_WA_FORCE_INTERP",
)
CLEAN_RUNTIME_ENV = {key: None for key in RUNTIME_ENV_KEYS}

REROLL_FIELDS = (
    "regions",
    "packed_rows",
    "term_density",
    "element_density",
    "term_widen",
    "element_store",
)

REQUIRED_PREP_ROWS = {
    ("driver", "total"): ("ns",),
    ("compile", "total"): ("ns",),
    ("log_prob", "lower"): ("ns", "ops"),
    ("log_prob", "total"): ("ns", "ops", "slots"),
    ("write_array", "total"): ("ns", "ops", "slots"),
    ("log_prob", "reroll"): ("ns",) + REROLL_FIELDS,
    ("write_array", "reroll"): ("ns",) + REROLL_FIELDS,
}


def sha256_file(path):
    path = pathlib.Path(path)
    if not path.is_file():
        return None
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def setup_value(key):
    pattern = re.compile(rf"^{re.escape(key)}=([^ ]+)")
    for line in (REPO / "tools" / "dev_setup.sh").read_text().splitlines():
        match = pattern.match(line)
        if match:
            return match.group(1)
    return None


def git_revision(path):
    path = pathlib.Path(path).resolve()
    try:
        return subprocess.check_output(
            ["git", "-c", f"safe.directory={path}", "-C", str(path),
             "rev-parse", "HEAD"],
            text=True, stderr=subprocess.DEVNULL).strip()
    except (OSError, subprocess.SubprocessError):
        return None


def git_tracked_dirty(path):
    path = pathlib.Path(path).resolve()
    proc = subprocess.run(
        ["git", "-c", f"safe.directory={path}", "-C", str(path),
         "status", "--porcelain", "--untracked-files=no"],
        capture_output=True, text=True)
    return None if proc.returncode != 0 else bool(proc.stdout.strip())


def read_stamp(path):
    fields = {}
    if not path.is_file():
        return fields
    for line in path.read_text().splitlines():
        key, separator, value = line.partition("=")
        if separator:
            fields[key] = value
    return fields


def probe_provenance_matches(probe, stanc_pin, opam_switch):
    script = (
        "source tools/stanc_embed/provenance.sh && "
        "stanc_embed_artifact_matches \"$1\" \"$2\" \"$3\""
    )
    proc = subprocess.run(
        ["bash", "-c", script, "probe-provenance", str(probe), stanc_pin,
         opam_switch], cwd=REPO, capture_output=True, text=True)
    return proc.returncode == 0, proc.stderr.strip()


def command_version(command):
    executable = shutil.which(command)
    if executable is None:
        return None
    proc = subprocess.run([executable, "--version"], capture_output=True,
                          text=True)
    lines = (proc.stdout or proc.stderr).strip().splitlines()
    return {
        "path": executable,
        "returncode": proc.returncode,
        "first_line": lines[0] if lines else "",
    }


def cmake_cache_metadata(tool):
    cache = tool.parent / "CMakeCache.txt"
    wanted = {
        "CMAKE_BUILD_TYPE",
        "CMAKE_CXX_COMPILER",
        "CMAKE_CXX_COMPILER_VERSION",
        "CMAKE_GENERATOR",
        "CMAKE_SYSTEM_NAME",
        "CMAKE_SYSTEM_PROCESSOR",
    }
    result = {}
    if cache.is_file():
        for line in cache.read_text(errors="replace").splitlines():
            key_type, separator, value = line.partition("=")
            key = key_type.partition(":")[0]
            if separator and key in wanted:
                result[key] = value
    return result


def execution_metadata(check):
    environment_keys = (
        "CI", "GITHUB_ACTIONS", "RUNNER_OS", "RUNNER_ARCH", "CC", "CXX",
        "LANG", "LC_ALL", "TZ",
    )
    return {
        "platform": {
            "platform": platform.platform(),
            "system": platform.system(),
            "release": platform.release(),
            "machine": platform.machine(),
        },
        "python": {
            "executable": sys.executable,
            "version": platform.python_version(),
            "implementation": platform.python_implementation(),
        },
        "toolchain": {
            "cmake": command_version("cmake"),
            "cxx": command_version(os.environ.get("CXX", "c++")),
            "cmake_cache": cmake_cache_metadata(check),
        },
        "environment": {
            "ambient": {key: os.environ[key] for key in environment_keys
                        if key in os.environ},
            "controlled": {
                "cleared_for_runtime_commands": list(RUNTIME_ENV_KEYS),
                "graph_runtime_reroll_off": "STANLI_NO_REROLL=1",
                "preparation_STANLI_PROFILE_PREP": "1",
            },
        },
    }


def _prepare_env(env):
    process_env = dict(os.environ)
    if env:
        for key, value in env.items():
            if value is None:
                process_env.pop(key, None)
            else:
                process_env[key] = value
    return process_env


def _exit_code_from_status(status):
    if os.WIFSIGNALED(status):
        return -os.WTERMSIG(status)
    if os.WIFEXITED(status):
        return os.WEXITSTATUS(status)
    return None


def _maxrss_bytes(rusage):
    if rusage is None or rusage.ru_maxrss <= 0:
        return None
    return rusage.ru_maxrss if sys.platform == "darwin" \
        else rusage.ru_maxrss * 1024


def _run_command_fallback(command, timeout, env=None):
    process_env = _prepare_env(env)
    started = time.monotonic_ns()
    try:
        proc = subprocess.run(
            [str(part) for part in command], cwd=REPO, env=process_env,
            capture_output=True, text=True, timeout=timeout)
        return {
            "returncode": proc.returncode,
            "stdout": proc.stdout,
            "stderr": proc.stderr,
            "timeout": False,
            "elapsed_ns": time.monotonic_ns() - started,
            "maxrss_bytes": None,
        }
    except subprocess.TimeoutExpired as error:
        stdout = error.stdout or ""
        stderr = error.stderr or ""
        if isinstance(stdout, bytes):
            stdout = stdout.decode(errors="replace")
        if isinstance(stderr, bytes):
            stderr = stderr.decode(errors="replace")
        return {
            "returncode": None,
            "stdout": stdout,
            "stderr": stderr,
            "timeout": True,
            "elapsed_ns": time.monotonic_ns() - started,
            "maxrss_bytes": None,
        }


def _run_command_wait4(command, timeout, env=None):
    process_env = _prepare_env(env)
    started = time.monotonic_ns()
    deadline = time.monotonic() + timeout
    with tempfile.TemporaryFile() as out_file, \
            tempfile.TemporaryFile() as err_file:
        proc = subprocess.Popen(
            [str(part) for part in command], cwd=REPO, env=process_env,
            stdout=out_file, stderr=err_file)
        reaped, status, rusage = 0, None, None
        while reaped == 0:
            reaped, status, rusage = os.wait4(proc.pid, os.WNOHANG)
            if reaped != 0 or time.monotonic() >= deadline:
                break
            time.sleep(0.005)
        timed_out = reaped == 0
        if timed_out:
            proc.kill()
            _, status, rusage = os.wait4(proc.pid, 0)
        elapsed_ns = time.monotonic_ns() - started
        proc.returncode = _exit_code_from_status(status)
        returncode = None if timed_out else proc.returncode
        out_file.seek(0)
        err_file.seek(0)
        stdout = out_file.read().decode(errors="replace")
        stderr = err_file.read().decode(errors="replace")
    return {
        "returncode": returncode,
        "stdout": stdout,
        "stderr": stderr,
        "timeout": timed_out,
        "elapsed_ns": elapsed_ns,
        "maxrss_bytes": _maxrss_bytes(rusage),
    }


def run_command(command, timeout, env=None):
    if hasattr(os, "wait4"):
        return _run_command_wait4(command, timeout, env)
    return _run_command_fallback(command, timeout, env)


def metric(value):
    """A strict-JSON representation of a possibly nonfinite metric."""
    if isinstance(value, float) and not math.isfinite(value):
        if math.isnan(value):
            return "nan"
        return "inf" if value > 0 else "-inf"
    return value


def same_float(left, right):
    if math.isnan(left) and math.isnan(right):
        return True
    return struct.pack("!d", left) == struct.pack("!d", right)


def float_class(value):
    if math.isnan(value):
        return "nan"
    if math.isinf(value):
        return "positive_infinity" if value > 0 else "negative_infinity"
    return "finite"


def float_bits(value):
    return f"0x{struct.unpack('!Q', struct.pack('!d', value))[0]:016x}"


def compare_vectors(left, right, include_differences=True):
    """Shape, aggregate metrics, and one bit/ULP row per changed value."""
    if len(left) != len(right):
        return {
            "same_shape": False,
            "left_size": len(left),
            "right_size": len(right),
            "changed": None,
            "finite_changed": None,
            "nonfinite_match": None,
            "max_rel": None,
            "max_ulp": None,
            "differences": [],
        }
    left_values = [float(value) for value in left]
    right_values = [float(value) for value in right]
    rel, ulp = worst_pair(left_values, right_values)
    differences = []
    changed = 0
    finite_changed = 0
    nonfinite_match = True
    for index, (a, b) in enumerate(zip(left_values, right_values)):
        a_class, b_class = float_class(a), float_class(b)
        if a_class != b_class:
            nonfinite_match = False
        if same_float(a, b):
            continue
        changed += 1
        if a_class == "finite" and b_class == "finite":
            finite_changed += 1
        if not include_differences:
            continue
        value_rel, value_ulp = pair_dev(a, b)
        differences.append({
            "index": index,
            "left": metric(a),
            "right": metric(b),
            "left_bits": float_bits(a),
            "right_bits": float_bits(b),
            "relative": metric(value_rel),
            "ulp": value_ulp,
        })
    return {
        "same_shape": True,
        "left_size": len(left),
        "right_size": len(right),
        "changed": changed,
        "finite_changed": finite_changed,
        "nonfinite_match": nonfinite_match,
        "max_rel": metric(rel),
        "max_ulp": ulp,
        "differences": differences,
    }


def parse_wa_outcome(stdout):
    """Preserve absent, successful, and failed write_array outcomes."""
    parsed = parse_wa(stdout)
    if parsed is not None:
        return {
            "category": "success",
            "names": parsed[0],
            "values": parsed[1],
        }
    for line in stdout.splitlines():
        if line == "WANAMES FAIL" or line.startswith("WANAMES FAIL "):
            return {
                "category": "failure",
                "reason": line[len("WANAMES FAIL"):].strip(),
            }
    return {"category": "absent"}


def compare_wa_outcomes(left, right):
    category_match = left["category"] == right["category"]
    result = {
        "category_match": category_match,
        "off_category": left["category"],
        "on_category": right["category"],
        "reason_match": True,
        "names_match": True,
        "values": None,
    }
    if not category_match:
        return result
    if left["category"] == "failure":
        result["reason_match"] = left.get("reason") == right.get("reason")
        result["off_reason"] = left.get("reason", "")
        result["on_reason"] = right.get("reason", "")
    elif left["category"] == "success":
        result["names_match"] = left["names"] == right["names"]
        result["values"] = compare_vectors(left["values"], right["values"])
    return result


def parse_key_values(line):
    fields = {}
    for key, raw in re.findall(r"([A-Za-z_]+)=([^\s]+)", line):
        try:
            fields[key] = int(raw)
        except ValueError:
            fields[key] = raw
    return fields


def parse_prep(stderr):
    rows = []
    for line in stderr.splitlines():
        if not line.startswith("stanli_prep "):
            continue
        fields = parse_key_values(line)
        if "graph" in fields and "stage" in fields and "ns" in fields:
            rows.append(fields)
    return rows


def parse_dump(stdout):
    result = {"opcodes": {}}
    for line in stdout.splitlines():
        if line.startswith("slots="):
            result.update(parse_key_values(line))
        elif line.startswith("SUMMARY "):
            result["summary"] = parse_key_values(line)
        elif line.startswith("  "):
            fields = line.split()
            if fields:
                result["opcodes"][fields[0]] = parse_key_values(line)
    return result


def dump_problems(parsed):
    problems = []
    for field in ("slots", "ops", "result"):
        if not isinstance(parsed.get(field), int):
            problems.append(f"missing {field}")
    summary = parsed.get("summary")
    if not isinstance(summary, dict):
        problems.append("missing SUMMARY")
        return problems
    for field in ("ops", "scalar_out", "vector_out"):
        if not isinstance(summary.get(field), int):
            problems.append(f"missing SUMMARY {field}")
    if (isinstance(parsed.get("ops"), int)
            and isinstance(summary.get("ops"), int)
            and parsed["ops"] != summary["ops"]):
        problems.append("op totals disagree")
    return problems


def prep_problems(rows):
    problems = []
    for (graph, stage), fields in REQUIRED_PREP_ROWS.items():
        row = prep_row(rows, graph, stage)
        if not row:
            problems.append(f"missing {graph}/{stage}")
            continue
        for field in fields:
            if not isinstance(row.get(field), int) or row[field] < 0:
                problems.append(f"missing {graph}/{stage} {field}")
    return problems


def prep_row(rows, graph, stage):
    return next(
        (row for row in rows
         if row.get("graph") == graph and row.get("stage") == stage),
        {})


def compile_source(compiler, source, output, candidate_pass, enabled,
                   timeout):
    selected = "on" if enabled else "off"
    if candidate_pass == VECTORIZE_LOOPS:
        pass_arguments = ["--vectorize-loops", selected]
    else:
        raise ValueError(f"unknown candidate pass: {candidate_pass}")
    command = [compiler, *pass_arguments, "--output", output, source]
    proc = run_command(command, timeout)
    produced = output.is_file()
    info = {
        "command": [str(part) for part in command],
        "returncode": proc["returncode"],
        "timeout": proc["timeout"],
        "elapsed_ns": proc["elapsed_ns"],
        "stderr": proc["stderr"].strip(),
        "produced": produced,
    }
    if produced:
        info.update({
            "bytes": output.stat().st_size,
            "sha256": sha256_file(output),
        })
    return proc, info


def default_selection(refs, pdb_entries, known_gaps=KNOWN_GAPS):
    return sorted((set(refs) | set(pdb_entries)) - set(known_gaps))


def reference_comparison(model, fields, point_reference, max_rel):
    if point_reference is None:
        kind = fields[0] if fields else ""
        return {
            "ok": kind in ("OK", "EVAL_FAIL"),
            "referenced": False,
            "actual": kind or "NO_STATUS",
        }
    kind = fields[0] if fields else ""
    if "values" not in point_reference:
        return {
            "ok": not accepted(fields),
            "expected": "EVAL_FAIL",
            "actual": kind or "NO_STATUS",
            "referenced": True,
        }
    if kind != "OK":
        return {
            "ok": False,
            "expected": "OK",
            "actual": kind or "NO_STATUS",
            "referenced": True,
        }
    values = fields[1:]
    compared = compare_vectors(point_reference["values"], values, False)
    gate = gate_for(model, point_reference, max_rel)
    rel = compared["max_rel"]
    within = (compared["same_shape"] and isinstance(rel, (int, float))
              and rel < gate)
    return {
        "ok": within,
        "expected": "OK",
        "actual": kind,
        "gate": gate,
        "values": compared,
        "referenced": True,
    }


def wa_comparison(model, outcome, point_reference, max_rel):
    if point_reference is None:
        return {
            "ok": True,
            "referenced": False,
            "actual": outcome["category"],
        }
    expected = point_reference.get("wa")
    if expected is None:
        return {
            "ok": True,
            "referenced": False,
            "actual": outcome["category"],
        }
    if outcome["category"] != "success":
        return {
            "ok": False,
            "referenced": True,
            "actual": outcome["category"],
        }
    compared = compare_vectors(expected["values"], outcome["values"], False)
    gate = gate_for(model, point_reference, max_rel)
    rel = compared["max_rel"]
    names_match = outcome["names"] == expected["names"]
    within = (compared["same_shape"] and isinstance(rel, (int, float))
              and rel < gate)
    return {
        "ok": names_match and within,
        "referenced": True,
        "names_match": names_match,
        "gate": gate,
        "values": compared,
    }


def execution_matches_status(proc, status):
    if proc["timeout"] or not status:
        return False
    expected = {"OK": 0, "EVAL_FAIL": 1, "COMPILE_FAIL": 1}
    return proc["returncode"] == expected.get(status[0])


def ab_only_values_within_gate(compared, max_rel):
    if compared is None:
        return True
    relative = compared["max_rel"]
    return (compared["same_shape"] and compared["nonfinite_match"]
            and isinstance(relative, (int, float)) and relative < max_rel)


def semantic_point(check, source, data, mirs, model, point,
                   point_reference, timeout, max_rel):
    runs = {}
    for mode in ("off", "on"):
        command = [
            check, source, data, "--mir", mirs[mode], "--point", str(point),
            "--wa-values",
        ]
        # Vectorization is a source-pass comparison. Ambient runtime reroll
        # settings must not silently change its semantic baseline.
        proc = run_command(command, timeout, CLEAN_RUNTIME_ENV)
        status = parse_status(proc["stdout"])
        wa = parse_wa_outcome(proc["stdout"])
        runs[mode] = {
            "command": [str(part) for part in command],
            "returncode": proc["returncode"],
            "timeout": proc["timeout"],
            "execution_matches_status": execution_matches_status(proc,
                                                                   status),
            "status": status,
            "wa": wa,
            "stderr_tail": proc["stderr"].strip().splitlines()[-1:][0]
            if proc["stderr"].strip() else "",
            "reference": reference_comparison(model, status,
                                              point_reference, max_rel),
            "wa_reference": wa_comparison(model, wa, point_reference,
                                          max_rel),
        }

    left, right = runs["off"], runs["on"]
    off_status, on_status = left["status"], right["status"]
    off_kind = off_status[0] if off_status else ""
    on_kind = on_status[0] if on_status else ""
    category_match = off_kind == on_kind and bool(off_kind)
    error_match = True
    values = None
    if category_match and off_kind == "OK":
        values = compare_vectors(off_status[1:], on_status[1:])
    elif category_match:
        error_match = off_status[1:] == on_status[1:]

    wa_ab = compare_wa_outcomes(left["wa"], right["wa"])
    ab_only = point_reference is None
    value_gate = not ab_only or ab_only_values_within_gate(values, max_rel)
    wa_value_gate = (not ab_only
                     or ab_only_values_within_gate(wa_ab["values"], max_rel))

    semantic_ok = (
        left["execution_matches_status"]
        and right["execution_matches_status"]
        and category_match and error_match
        and left["reference"]["ok"] and right["reference"]["ok"]
        and left["wa_reference"]["ok"] and right["wa_reference"]["ok"]
        and (values is None or (values["same_shape"]
                                and values["nonfinite_match"]))
        and wa_ab["category_match"] and wa_ab["reason_match"]
        and wa_ab["names_match"]
        and (wa_ab["values"] is None
             or (wa_ab["values"]["same_shape"]
                 and wa_ab["values"]["nonfinite_match"]))
        and value_gate and wa_value_gate
    )
    return {
        "point": point,
        "ok": semantic_ok,
        "off": {key: value for key, value in left.items() if key != "wa"},
        "on": {key: value for key, value in right.items() if key != "wa"},
        "ab": {
            "category_match": category_match,
            "error_match": error_match,
            "values": values,
            "wa": wa_ab,
            "ab_only_finite_gate": {
                "applied": ab_only,
                "max_rel": max_rel if ab_only else None,
                "values_ok": value_gate,
                "wa_values_ok": wa_value_gate,
            },
        },
    }


def reroll_fields(rows, graph):
    row = prep_row(rows, graph, "reroll")
    return {field: row.get(field) for field in REROLL_FIELDS}


def summarize_reroll(records):
    evidence = []
    for record in records:
        samples = []
        for sample in record["samples"]:
            log_total = prep_row(sample["rows"], "log_prob", "total")
            wa_total = prep_row(sample["rows"], "write_array", "total")
            log_evidence = reroll_fields(sample["rows"], "log_prob")
            log_evidence["final_ops"] = log_total.get("ops")
            log_evidence["final_slots"] = log_total.get("slots")
            wa_evidence = reroll_fields(sample["rows"], "write_array")
            wa_evidence["final_ops"] = wa_total.get("ops")
            wa_evidence["final_slots"] = wa_total.get("slots")
            samples.append({
                "sample": sample["sample"],
                "log_prob": log_evidence,
                "write_array": wa_evidence,
            })
        evidence.append({
            "source_pass": record["source_pass"],
            "runtime_reroll": record["runtime_reroll"],
            "samples": samples,
        })
    return evidence


def graph_cell(dump, bench, mir, data, source_mode, reroll_enabled,
               prep_samples, timeout):
    env = dict(CLEAN_RUNTIME_ENV)
    if not reroll_enabled:
        env["STANLI_NO_REROLL"] = "1"
    dump_env = dict(env)
    dump_proc = run_command([dump, mir, data, "-1"], timeout, dump_env)
    parsed_dump = (parse_dump(dump_proc["stdout"])
                   if dump_proc["returncode"] == 0 else {})
    dump_issues = dump_problems(parsed_dump)
    samples = []
    for sample in range(prep_samples):
        sample_env = dict(env)
        sample_env["STANLI_PROFILE_PREP"] = "1"
        proc = run_command([bench, mir, data, "--prep"], timeout, sample_env)
        rows = parse_prep(proc["stderr"])
        profile_issues = prep_problems(rows)
        samples.append({
            "sample": sample + 1,
            "returncode": proc["returncode"],
            "timeout": proc["timeout"],
            "elapsed_ns": proc["elapsed_ns"],
            "maxrss_bytes": proc["maxrss_bytes"],
            "rows": rows,
            "profile_problems": profile_issues,
            "stdout": proc["stdout"].strip(),
            "stderr_tail": proc["stderr"].strip().splitlines()[-1:][0]
            if proc["stderr"].strip() else "",
        })
    return {
        "source_pass": source_mode,
        "runtime_reroll": "on" if reroll_enabled else "off",
        "dump_returncode": dump_proc["returncode"],
        "dump_timeout": dump_proc["timeout"],
        "dump_elapsed_ns": dump_proc["elapsed_ns"],
        "dump_problems": dump_issues,
        "graph": parsed_dump,
        "samples": samples,
    }


def lower_ops(cell):
    for sample in cell["samples"]:
        ops = prep_row(sample["rows"], "log_prob", "lower").get("ops")
        if isinstance(ops, int):
            return ops
    return None


def op_count_gate(model, records):
    cells = {
        (record["source_pass"], record["runtime_reroll"]): record
        for record in records
    }
    off, on = cells.get(("off", "on")), cells.get(("on", "on"))
    if off is None or on is None:
        return []
    failures = []
    off_lower, on_lower = lower_ops(off), lower_ops(on)
    if (off_lower is not None and on_lower is not None
            and on_lower > off_lower and model not in LOWERED_GROWTH_EXPECTED):
        failures.append(
            f"{model}: lowered log_prob ops grew {off_lower} -> {on_lower} "
            "(off/reroll-on vs on/reroll-on)")
    off_final = off.get("graph", {}).get("ops")
    on_final = on.get("graph", {}).get("ops")
    if (isinstance(off_final, int) and isinstance(on_final, int)
            and on_final * 100 > off_final * (100 + FINAL_OPS_GROWTH_PERCENT)):
        failures.append(
            f"{model}: final log_prob ops grew {off_final} -> {on_final}, "
            f"over {FINAL_OPS_GROWTH_PERCENT}% "
            "(off/reroll-on vs on/reroll-on)")
    return failures


BASELINE_FIELDS = ("final_ops", "lower_ops", "regions", "final_slots")


def cell_summary(record):
    rows = [row for sample in record["samples"] for row in sample["rows"]
            if row.get("graph") == "log_prob"]
    stages = {row["stage"]: row for row in rows}
    graph = record.get("graph") or {}
    return {
        "final_ops": graph.get("ops"),
        "final_slots": graph.get("slots"),
        "lower_ops": stages.get("lower", {}).get("ops"),
        "regions": stages.get("island", {}).get("regions"),
        "island_ns": stages.get("island", {}).get("ns"),
        "prep_ns": stages.get("total", {}).get("ns"),
    }


def cells_from_records(records):
    return {
        (record["model"], record["source_pass"], record["runtime_reroll"]):
            cell_summary(record)
        for record in records
    }


def load_cells(output_dir):
    path = pathlib.Path(output_dir) / "graphs.jsonl"
    if not path.is_file():
        return None
    records = []
    with path.open() as stream:
        for line in stream:
            line = line.strip()
            if line:
                records.append(json.loads(line))
    return cells_from_records(records)


def diff_baseline(before, after):
    changes = []
    for key in sorted(set(before) & set(after)):
        b, a = before[key], after[key]
        if all(b.get(field) == a.get(field) for field in BASELINE_FIELDS):
            continue
        model, source_pass, runtime_reroll = key
        changes.append({
            "model": model,
            "source_pass": source_pass,
            "runtime_reroll": runtime_reroll,
            "before": {field: b.get(field) for field in BASELINE_FIELDS},
            "after": {field: a.get(field) for field in BASELINE_FIELDS},
        })
    return changes


def baseline_comparison_report(baseline_dir, before_cells, after_cells):
    if before_cells is None:
        return {
            "baseline_dir": str(baseline_dir),
            "available": False,
            "reason": "no graphs.jsonl in the baseline directory",
        }
    changes = diff_baseline(before_cells, after_cells)
    grew, shrank = set(), set()
    for change in changes:
        before_ops = change["before"]["final_ops"]
        after_ops = change["after"]["final_ops"]
        if not (isinstance(before_ops, int) and isinstance(after_ops, int)):
            continue
        if after_ops > before_ops:
            grew.add(change["model"])
        elif after_ops < before_ops:
            shrank.add(change["model"])
    return {
        "baseline_dir": str(baseline_dir),
        "available": True,
        "cells_compared": len(set(before_cells) & set(after_cells)),
        "changed_cells": len(changes),
        "final_ops_grew_models": sorted(grew),
        "final_ops_shrank_models": sorted(shrank),
        "changes": changes,
    }


def render_baseline_comparison(comparison):
    if comparison is None:
        return []
    lines = ["## Baseline comparison", "",
             f"Baseline: `{comparison['baseline_dir']}`"]
    if not comparison["available"]:
        lines += [f"Not available: {comparison['reason']}", ""]
        return lines
    lines += [
        f"Cells compared: {comparison['cells_compared']}. "
        f"Cells changed: {comparison['changed_cells']}. "
        f"Models whose final log_prob ops grew: "
        f"{len(comparison['final_ops_grew_models'])}. "
        f"Shrank: {len(comparison['final_ops_shrank_models'])}.",
        "",
    ]
    if comparison["changes"]:
        lines += [
            "| model | cell | final ops | lower ops | regions | "
            "final slots |",
            "| --- | --- | --- | --- | --- | --- |",
        ]
        for change in comparison["changes"]:
            cell = f"{change['source_pass']}/{change['runtime_reroll']}"
            b, a = change["before"], change["after"]
            lines.append(
                f"| `{change['model']}` | {cell} | "
                f"{b['final_ops']} -> {a['final_ops']} | "
                f"{b['lower_ops']} -> {a['lower_ops']} | "
                f"{b['regions']} -> {a['regions']} | "
                f"{b['final_slots']} -> {a['final_slots']} |")
        lines.append("")
    return lines


def parse_bench_output(stdout):
    """Parse only bench_grad's final four-field numeric row."""
    lines = stdout.rstrip().splitlines()
    if not lines:
        return None
    fields = lines[-1].split()
    if len(fields) != 4:
        return None
    try:
        gradient_ns = float(fields[0])
        sink = float(fields[1])
        forward_ns = float(fields[2])
        n_params = int(fields[3])
    except ValueError:
        return None
    if (not math.isfinite(gradient_ns) or gradient_ns <= 0
            or not math.isfinite(forward_ns) or forward_ns < 0
            or n_params < 0):
        return None
    return {
        "gradient_ns": gradient_ns,
        "sink": metric(sink),
        "forward_ns": forward_ns,
        "n_params": n_params,
    }


def calibrated_iterations(latencies_ns, target_seconds, maximum):
    usable = [value for value in latencies_ns
              if isinstance(value, (int, float)) and value > 0
              and math.isfinite(value)]
    if not usable:
        return 1
    iterations = int(target_seconds * 1e9 / max(usable))
    return max(1, min(maximum, iterations))


def bench_run(bench, mir, data, iterations, timeout):
    command = [bench, mir, data, str(iterations)]
    proc = run_command(command, timeout, CLEAN_RUNTIME_ENV)
    parsed = parse_bench_output(proc["stdout"])
    return {
        "command": [str(part) for part in command],
        "returncode": proc["returncode"],
        "timeout": proc["timeout"],
        "elapsed_ns": proc["elapsed_ns"],
        "maxrss_bytes": proc["maxrss_bytes"],
        "result": parsed,
        "stderr_tail": proc["stderr"].strip().splitlines()[-1:][0]
        if proc["stderr"].strip() else "",
        "ok": proc["returncode"] == 0 and not proc["timeout"]
        and parsed is not None,
    }


def gradient_benchmark(bench, mirs, data, rounds, calibration_n,
                       target_seconds, max_n, timeout):
    calibrations = {
        mode: bench_run(bench, mirs[mode], data, calibration_n, timeout)
        for mode in ("off", "on")
    }
    latency = [
        run["result"]["gradient_ns"]
        for run in calibrations.values()
        if run["ok"]
    ]
    iterations = calibrated_iterations(latency, target_seconds, max_n)
    order = ("off", "on", "on", "off")
    runs = []
    for round_index in range(rounds):
        for order_index, mode in enumerate(order):
            run = bench_run(bench, mirs[mode], data, iterations, timeout)
            run.update({
                "round": round_index + 1,
                "order": order_index + 1,
                "source_pass": mode,
            })
            runs.append(run)
    medians = {}
    for mode in ("off", "on"):
        values = [
            run["result"]["gradient_ns"] for run in runs
            if run["source_pass"] == mode and run["ok"]
        ]
        medians[mode] = statistics.median(values) if values else None
    ratio = None
    if medians["off"] and medians["on"] is not None:
        ratio = medians["on"] / medians["off"]
    ok = all(run["ok"] for run in calibrations.values()) \
        and all(run["ok"] for run in runs)
    return {
        "ok": ok,
        "target_seconds": target_seconds,
        "iterations": iterations,
        "rounds": rounds,
        "process_order": list(order),
        "calibration_iterations": calibration_n,
        "calibrations": calibrations,
        "runs": runs,
        "median_gradient_ns": medians,
        "on_over_off": ratio,
    }


def gradient_ratio_exceeds(gradient):
    ratio = gradient.get("on_over_off") if gradient else None
    return isinstance(ratio, (int, float)) and ratio > GRADIENT_RATIO_THRESHOLD


def gradient_gate(model, gradient, confirmation):
    if not gradient_ratio_exceeds(gradient):
        return []
    if not confirmation or not confirmation.get("ok") \
            or not gradient_ratio_exceeds(confirmation):
        return []
    return [
        f"{model}: gradient on/off {gradient['on_over_off']:.4f} exceeds "
        f"{GRADIENT_RATIO_THRESHOLD}, confirmed by a fresh re-run at "
        f"{confirmation['on_over_off']:.4f}"
    ]


def tsv_value(row, key):
    return row.get(key, "") if row else ""


def blank_if_none(value):
    return value if value is not None else ""


def write_reports(output_dir, manifest, corpus_records, graph_records,
                  model_summaries, failures, infrastructure_failures,
                  op_count_diagnostics, gradient_failures,
                  baseline_comparison=None):
    output_dir.mkdir(parents=True, exist_ok=True)
    (output_dir / "manifest.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n")
    with (output_dir / "corpus.jsonl").open("w") as stream:
        for record in corpus_records:
            stream.write(json.dumps(record, sort_keys=True) + "\n")
    with (output_dir / "graphs.jsonl").open("w") as stream:
        for record in graph_records:
            stream.write(json.dumps(record, sort_keys=True) + "\n")

    columns = [
        "model", "measurement", "source_pass", "runtime_reroll", "sample",
        "round", "order", "compiler_elapsed_ns", "process_elapsed_ns",
        "driver_total_ns", "compile_ns", "log_prob_total_ns",
        "write_array_total_ns", "gradient_n", "gradient_ns", "forward_ns",
        "n_params", "log_prob_ops", "log_prob_scalar_out",
        "write_array_ops", "log_prob_slots", "write_array_slots",
        "prep_maxrss_bytes", "gradient_maxrss_bytes",
    ]
    for graph in ("log_prob", "write_array"):
        columns.extend(f"{graph}_reroll_{field}" for field in REROLL_FIELDS)
    with (output_dir / "bench.tsv").open("w") as stream:
        stream.write("\t".join(columns) + "\n")

        def emit(values):
            stream.write("\t".join(str(values.get(name, ""))
                                   for name in columns) + "\n")

        for model in model_summaries:
            for mode, compiled in model.get("compile", {}).items():
                emit({
                    "model": model["model"],
                    "measurement": "source_compile",
                    "source_pass": mode,
                    "compiler_elapsed_ns": compiled.get("elapsed_ns", ""),
                })
            gradient = model.get("gradient")
            if gradient:
                for mode, run in gradient["calibrations"].items():
                    result = run.get("result") or {}
                    emit({
                        "model": model["model"],
                        "measurement": "gradient_calibration",
                        "source_pass": mode,
                        "sample": 0,
                        "process_elapsed_ns": run["elapsed_ns"],
                        "gradient_n": gradient["calibration_iterations"],
                        "gradient_ns": result.get("gradient_ns", ""),
                        "forward_ns": result.get("forward_ns", ""),
                        "n_params": result.get("n_params", ""),
                        "gradient_maxrss_bytes": blank_if_none(
                            run.get("maxrss_bytes")),
                    })
                for run in gradient["runs"]:
                    result = run.get("result") or {}
                    emit({
                        "model": model["model"],
                        "measurement": "gradient",
                        "source_pass": run["source_pass"],
                        "sample": ((run["round"] - 1) * 4
                                   + run["order"]),
                        "round": run["round"],
                        "order": run["order"],
                        "process_elapsed_ns": run["elapsed_ns"],
                        "gradient_n": gradient["iterations"],
                        "gradient_ns": result.get("gradient_ns", ""),
                        "forward_ns": result.get("forward_ns", ""),
                        "n_params": result.get("n_params", ""),
                        "gradient_maxrss_bytes": blank_if_none(
                            run.get("maxrss_bytes")),
                    })
        for record in graph_records:
            summary = record.get("graph", {}).get("summary", {})
            for sample in record["samples"]:
                rows = sample["rows"]
                driver = prep_row(rows, "driver", "total")
                compile_total = prep_row(rows, "compile", "total")
                log_total = prep_row(rows, "log_prob", "total")
                wa_total = prep_row(rows, "write_array", "total")
                values = {
                    "model": record["model"],
                    "measurement": "preparation",
                    "source_pass": record["source_pass"],
                    "runtime_reroll": record["runtime_reroll"],
                    "sample": sample["sample"],
                    "process_elapsed_ns": sample["elapsed_ns"],
                    "driver_total_ns": tsv_value(driver, "ns"),
                    "compile_ns": tsv_value(compile_total, "ns"),
                    "log_prob_total_ns": tsv_value(log_total, "ns"),
                    "write_array_total_ns": tsv_value(wa_total, "ns"),
                    "log_prob_ops": summary.get("ops", ""),
                    "log_prob_scalar_out": summary.get("scalar_out", ""),
                    "write_array_ops": tsv_value(wa_total, "ops"),
                    "log_prob_slots": tsv_value(log_total, "slots"),
                    "write_array_slots": tsv_value(wa_total, "slots"),
                    "prep_maxrss_bytes": blank_if_none(
                        sample.get("maxrss_bytes")),
                }
                for graph in ("log_prob", "write_array"):
                    reroll = prep_row(rows, graph, "reroll")
                    for field in REROLL_FIELDS:
                        values[f"{graph}_reroll_{field}"] = tsv_value(
                            reroll, field)
                emit(values)

    changed_models = sum(summary["mir_changed"] for summary in model_summaries)
    changed_values = sum(summary["changed_values"]
                         for summary in model_summaries)
    summary = {
        "schema": 2,
        "ok": (not failures and not infrastructure_failures
               and not gradient_failures),
        "candidate_pass": manifest.get("corpus_scope", {}).get(
            "candidate_pass", VECTORIZE_LOOPS),
        "models": len(model_summaries),
        "referenced_models": sum(
            item.get("reference_kind") == "cmdstan-recorded"
            for item in model_summaries),
        "ab_only_models": sum(
            item.get("reference_kind") == "ab-only"
            for item in model_summaries),
        "points": len(corpus_records),
        "harness_elapsed_ns": manifest.get("harness_elapsed_ns"),
        "mir_changed_models": changed_models,
        "arithmetic_order_changed_values": changed_values,
        "semantic_failures": failures,
        "op_count_diagnostics": op_count_diagnostics,
        "gradient_failures": gradient_failures,
        "infrastructure_failures": infrastructure_failures,
        "baseline_comparison": baseline_comparison,
        "per_model": model_summaries,
    }
    (output_dir / "summary.json").write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n")

    lines = ["# MIR source-pass A/B measurement", ""]
    lines += render_baseline_comparison(baseline_comparison)
    lines += [
        f"Outcome: **{'PASS' if summary['ok'] else 'FAIL'}**", "",
        f"- Candidate pass: `{summary['candidate_pass']}`",
        f"- Models: {summary['models']}",
        f"- CmdStan-referenced models: {summary['referenced_models']}",
        f"- A/B-only models: {summary['ab_only_models']}",
        f"- Semantic points: {summary['points']}",
        f"- Harness elapsed seconds: "
        f"{summary['harness_elapsed_ns'] / 1e9:.3f}"
        if summary["harness_elapsed_ns"] is not None
        else "- Harness elapsed seconds: unavailable",
        f"- Models with different portable MIR: {changed_models}",
        f"- Finite values changed by arithmetic order: {changed_values}",
        f"- Semantic failures: {len(failures)}",
        f"- Op count diagnostics: {len(op_count_diagnostics)}",
        f"- Gradient time failures: {len(gradient_failures)}",
        f"- Measurement infrastructure failures: "
        f"{len(infrastructure_failures)}", "",
        "Preparation timings in `graphs.jsonl` and `bench.tsv`, and op "
        "counts for a model with different portable MIR, are diagnostics: "
        "they never fail the run on their own. The lowered log_prob graph "
        "growing, or the final log_prob graph growing by more than "
        f"{FINAL_OPS_GROWTH_PERCENT}% (both in the runtime-reroll-on "
        "cells), are listed below as `## Op count diagnostics` but do not "
        "decide pass/fail. Gradient time on `GRADIENT_MODELS` is the "
        f"execution gate: a model whose on/off ratio exceeds "
        f"{GRADIENT_RATIO_THRESHOLD} is re-measured fresh, and the run "
        "fails only if that re-run also exceeds the ratio.", "",
        "| model | comparison | MIR changed | changed values | semantic points | "
        "gradient on/off | confirmation on/off |",
        "| --- | --- | ---: | ---: | ---: | ---: | ---: |",
    ]
    for item in model_summaries:
        ratio = item.get("gradient", {}).get("on_over_off")
        ratio_text = f"{ratio:.4f}" if ratio is not None else ""
        confirmation_ratio = (item.get("gradient_confirmation") or {}).get(
            "on_over_off")
        confirmation_text = (f"{confirmation_ratio:.4f}"
                             if confirmation_ratio is not None else "")
        lines.append(
            f"| `{item['model']}` | {item.get('reference_kind', '')} | "
            f"{int(item['mir_changed'])} | "
            f"{item['changed_values']} | {item['points']} | {ratio_text} | "
            f"{confirmation_text} |")
    if failures:
        lines += ["", "## Semantic failures", ""]
        lines += [f"- {failure}" for failure in failures]
    if op_count_diagnostics:
        lines += ["", "## Op count diagnostics", ""]
        lines += [f"- {failure}" for failure in op_count_diagnostics]
    if gradient_failures:
        lines += ["", "## Gradient time failures", ""]
        lines += [f"- {failure}" for failure in gradient_failures]
    if infrastructure_failures:
        lines += ["", "## Measurement infrastructure failures", ""]
        lines += [f"- {failure}" for failure in infrastructure_failures]
    (output_dir / "summary.md").write_text("\n".join(lines) + "\n")
    return summary


def main():
    harness_started_ns = time.monotonic_ns()
    parser = argparse.ArgumentParser()
    parser.add_argument("pdb", type=pathlib.Path)
    parser.add_argument("models", nargs="*")
    parser.add_argument(
        "--compiler", type=pathlib.Path,
        default=REPO / "deps" / "stanc3" / "stanli-vectorize-probe")
    parser.add_argument(
        "--candidate-pass", choices=CANDIDATE_PASSES,
        default=VECTORIZE_LOOPS)
    parser.add_argument("--check", type=pathlib.Path,
                        default=REPO / "build-rel" / "stanli_check")
    parser.add_argument("--bench", type=pathlib.Path,
                        default=REPO / "build-rel" / "bench_grad")
    parser.add_argument("--dump", type=pathlib.Path,
                        default=REPO / "build-rel" / "dump_ops")
    parser.add_argument("--output-dir", type=pathlib.Path,
                        default=REPO / "build" / "vectorize-ab")
    parser.add_argument(
        "--baseline", type=pathlib.Path, default=None,
        help="a previous --output-dir to diff final ops, lowered ops, "
             "island regions, and slots against, report-only")
    parser.add_argument("--max-rel", type=float, default=1e-9)
    parser.add_argument("--timeout", type=float, default=300)
    parser.add_argument("--prep-samples", type=int, default=1)
    parser.add_argument("--gradient-rounds", type=int, default=2)
    parser.add_argument("--gradient-calibration-n", type=int, default=8)
    parser.add_argument("--gradient-target-seconds", type=float, default=0.75)
    parser.add_argument("--gradient-max-n", type=int, default=1_000_000)
    parser.add_argument("--gradient-timeout", type=float, default=20)
    args = parser.parse_intermixed_args()

    if args.prep_samples < 1:
        parser.error("--prep-samples must be positive")
    if args.timeout <= 0:
        parser.error("--timeout must be positive")
    if args.gradient_rounds < 2:
        parser.error("--gradient-rounds must be at least 2")
    if args.gradient_calibration_n < 1:
        parser.error("--gradient-calibration-n must be positive")
    if not 0 < args.gradient_target_seconds <= 5:
        parser.error("--gradient-target-seconds must be in (0, 5]")
    if args.gradient_max_n < 1:
        parser.error("--gradient-max-n must be positive")
    if not 0 < args.gradient_timeout <= args.timeout:
        parser.error("--gradient-timeout must be in (0, --timeout]")
    tools = {
        "compiler": args.compiler.resolve(),
        "check": args.check.resolve(),
        "bench": args.bench.resolve(),
        "dump": args.dump.resolve(),
    }
    missing_tools = [str(path) for path in tools.values() if not path.is_file()]
    if missing_tools:
        parser.error("missing tool(s): " + ", ".join(missing_tools))

    probe_stamp_path = pathlib.Path(str(tools["compiler"]) + ".stamp")
    if not probe_stamp_path.is_file():
        parser.error(f"missing compiler provenance: {probe_stamp_path}")
    probe_stamp = read_stamp(probe_stamp_path)
    stanc_pin = setup_value("STANC3_SRC_SHA")
    stanc_repo = setup_value("STANC3_SRC_REPO")
    opam_switch = setup_value("OPAM_SWITCH")
    if probe_stamp.get("stanc3_src_sha") != stanc_pin:
        parser.error("compiler provenance does not match STANC3_SRC_SHA")
    provenance_ok, provenance_error = probe_provenance_matches(
        tools["compiler"], stanc_pin, opam_switch)
    if not provenance_ok:
        detail = f": {provenance_error}" if provenance_error else ""
        parser.error(f"compiler provenance does not match producer inputs"
                     f"{detail}")

    pdb_checkout = args.pdb.resolve()
    pdb = pdb_checkout / "posterior_database"
    refs, recorded = load_refs()
    pdb_entries = dict(corpus_models(pdb))
    selected = args.models or default_selection(refs, pdb_entries)
    missing_models = [
        model for model in selected
        if model not in refs and model not in pdb_entries
    ]
    if missing_models:
        parser.error("no recorded or posteriordb inputs for: "
                     + ", ".join(missing_models))

    pdb_pin = setup_value("PDB_SHA")
    pdb_revision = git_revision(pdb_checkout)
    if recorded.get("posteriordb") != pdb_pin:
        parser.error("reference provenance does not match PDB_SHA")
    if pdb_revision is not None and pdb_revision != pdb_pin:
        parser.error("posteriordb checkout does not match PDB_SHA")
    language_models = [
        model for model in selected
        if (REPO / "tests" / "stanc3" / f"{model}.stan").is_file()
    ]
    selected_pdb_models = [
        model for model in selected if model not in language_models
    ]
    selected_referenced = [model for model in selected if model in refs]
    selected_ab_only = [model for model in selected if model not in refs]
    available_pdb_models = len(pdb_entries)
    if args.models:
        selection_kind = (
            "bounded-recorded-reference-selection"
            if not selected_ab_only else "bounded-mixed-ab-selection"
        )
    else:
        selection_kind = "complete-references-plus-posteriordb-model-census"
    gradient_models = [
        model for model in selected
        if model in GRADIENT_MODELS[args.candidate_pass]
    ]

    revision = git_revision(REPO) or "unknown"
    manifest = {
        "schema": 2,
        "generated_at": datetime.datetime.now(
            datetime.timezone.utc).isoformat(),
        "repository": str(REPO),
        "revision": revision,
        "repository_state": {
            "revision": revision,
            "tracked_dirty": git_tracked_dirty(REPO),
            "harness_sha256": sha256_file(pathlib.Path(__file__)),
        },
        "command": sys.argv,
        "corpus_scope": {
            "kind": selection_kind,
            "models": selected,
            "selected_models": len(selected),
            "selected_posteriordb_models": len(selected_pdb_models),
            "selected_language_models": len(language_models),
            "selected_referenced_models": len(selected_referenced),
            "selected_ab_only_models": len(selected_ab_only),
            "ab_only_models": selected_ab_only,
            "recorded_reference_models": len(refs),
            "posteriordb_models_available": available_pdb_models,
            "full_posteriordb_model_census": not bool(args.models),
            "full_model_data_pair_sweep": False,
            "gradient_models": gradient_models,
            "candidate_pass": args.candidate_pass,
            "expected_mir_change_sentinels": sorted(
                EXPECTED_MIR_CHANGES[args.candidate_pass]),
            "note": "Recorded models use CmdStan gates; posteriordb models "
                    "without a reference use off/on parity only. One data "
                    "set is selected per posteriordb model.",
        },
        "points": list(POINTS),
        "max_rel": args.max_rel,
        "prep_samples": args.prep_samples,
        "baseline_dir": str(args.baseline.resolve())
        if args.baseline is not None else None,
        "gradient": {
            "rounds": args.gradient_rounds,
            "process_order": ["off", "on", "on", "off"],
            "calibration_iterations": args.gradient_calibration_n,
            "target_seconds": args.gradient_target_seconds,
            "maximum_iterations": args.gradient_max_n,
            "process_timeout_seconds": args.gradient_timeout,
            "gating": True,
            "ratio_threshold": GRADIENT_RATIO_THRESHOLD,
        },
        "stanc3": {
            "repository": stanc_repo,
            "configured_pin": stanc_pin,
            "source_checkout_revision": git_revision(
                REPO / "deps" / "stanc3-src"),
            "source_checkout_tracked_dirty": git_tracked_dirty(
                REPO / "deps" / "stanc3-src"),
            "probe_stamp_path": str(probe_stamp_path),
            "probe_stamp": probe_stamp,
            "probe_provenance_validated": True,
        },
        "posteriordb": {
            "configured_pin": pdb_pin,
            "checkout_revision": pdb_revision,
            "checkout_tracked_dirty": git_tracked_dirty(pdb_checkout),
            "reference_pin": recorded.get("posteriordb"),
            "checkout": str(pdb_checkout),
        },
        "cmdstan_reference": recorded,
        "tools": {
            name: {"path": str(path), "sha256": sha256_file(path)}
            for name, path in tools.items()
        },
    }
    manifest.update(execution_metadata(tools["check"]))

    output_dir = args.output_dir.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    (output_dir / "manifest.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n")
    corpus_records = []
    graph_records = []
    model_summaries = []
    failures = []
    infrastructure_failures = []
    op_count_diagnostics = []
    gradient_failures = []
    with tempfile.TemporaryDirectory(prefix="stanli_vectorize_ab_") as temp:
        temp = pathlib.Path(temp)
        for model in selected:
            reference_kind = (
                "cmdstan-recorded" if model in refs else "ab-only"
            )
            if model in refs:
                inputs = model_files(model, refs[model], pdb, temp)
            else:
                inputs = corpus_input(
                    pdb, temp, model, pdb_entries[model])
            if inputs is None:
                failures.append(f"{model}: missing posteriordb input")
                model_summaries.append({
                    "model": model, "mir_changed": False,
                    "changed_values": 0, "points": 0,
                    "reference_kind": reference_kind,
                })
                continue
            source, data = inputs
            if not source.is_file() or not data.is_file():
                failures.append(f"{model}: missing input {source} or {data}")
                model_summaries.append({
                    "model": model, "mir_changed": False,
                    "changed_values": 0, "points": 0,
                    "reference_kind": reference_kind,
                })
                continue

            mirs = {
                mode: temp / f"{model}.{mode}.portable-mir"
                for mode in ("off", "on")
            }
            compile_info = {}
            compile_ok = True
            for mode in ("off", "on"):
                proc, info = compile_source(
                    tools["compiler"], source, mirs[mode],
                    args.candidate_pass, mode == "on", args.timeout)
                compile_info[mode] = info
                if proc["returncode"] != 0 or not info["produced"]:
                    compile_ok = False
                    failures.append(
                        f"{model}: {mode} source compilation failed: "
                        f"{info['stderr'][-160:]}")

            if not compile_ok:
                model_summaries.append({
                    "model": model, "mir_changed": False,
                    "changed_values": 0, "points": 0,
                    "compile": compile_info,
                    "reference_kind": reference_kind,
                })
                continue

            mir_changed = compile_info["off"]["sha256"] != \
                compile_info["on"]["sha256"]
            if (model in EXPECTED_MIR_CHANGES[args.candidate_pass]
                    and not mir_changed):
                infrastructure_failures.append(
                    f"{model}: {args.candidate_pass} sentinel MIR did not "
                    "change")
            changed_values = 0
            model_points = 0
            for point in POINTS:
                record = semantic_point(
                    tools["check"], source, data, mirs, model, point,
                    refs[model]["points"][str(point)]
                    if model in refs else None, args.timeout,
                    args.max_rel)
                record.update({
                    "model": model,
                    "source": str(source),
                    "data": str(data),
                    "source_sha256": sha256_file(source),
                    "data_sha256": sha256_file(data),
                    "compile": compile_info,
                    "reference_kind": reference_kind,
                })
                corpus_records.append(record)
                model_points += 1
                if not record["ok"]:
                    failures.append(f"{model} point {point}: semantic parity")
                comparisons = [
                    record["ab"].get("values"),
                    record["ab"].get("wa", {}).get("values"),
                ]
                for compared in comparisons:
                    if (compared
                            and compared.get("finite_changed") is not None):
                        changed_values += compared["finite_changed"]

            model_graph_records = []
            for source_mode in ("off", "on"):
                for reroll_enabled in (False, True):
                    cell = graph_cell(
                        tools["dump"], tools["bench"], mirs[source_mode], data,
                        source_mode, reroll_enabled, args.prep_samples,
                        args.timeout)
                    cell["model"] = model
                    graph_records.append(cell)
                    model_graph_records.append(cell)
                    if (cell["dump_returncode"] != 0 or cell["dump_timeout"]
                            or cell["dump_problems"]):
                        detail = ", ".join(cell["dump_problems"])
                        infrastructure_failures.append(
                            f"{model} {source_mode}/reroll-"
                            f"{'on' if reroll_enabled else 'off'}: dump_ops"
                            f"{': ' + detail if detail else ''}")
                    for sample in cell["samples"]:
                        if (sample["returncode"] != 0 or sample["timeout"]
                                or sample["profile_problems"]):
                            detail = ", ".join(sample["profile_problems"])
                            infrastructure_failures.append(
                                f"{model} {source_mode}/reroll-"
                                f"{'on' if reroll_enabled else 'off'} "
                                f"sample {sample['sample']}: bench_grad"
                                f"{': ' + detail if detail else ''}")
            if mir_changed:
                op_count_diagnostics.extend(
                    op_count_gate(model, model_graph_records))

            gradient = None
            gradient_confirmation = None
            if model in gradient_models:
                gradient = gradient_benchmark(
                    tools["bench"], mirs, data, args.gradient_rounds,
                    args.gradient_calibration_n,
                    args.gradient_target_seconds, args.gradient_max_n,
                    args.gradient_timeout)
                if not gradient["ok"]:
                    infrastructure_failures.append(
                        f"{model}: gradient benchmark")
                elif gradient_ratio_exceeds(gradient):
                    gradient_confirmation = gradient_benchmark(
                        tools["bench"], mirs, data, args.gradient_rounds,
                        args.gradient_calibration_n,
                        args.gradient_target_seconds, args.gradient_max_n,
                        args.gradient_timeout)
                    if not gradient_confirmation["ok"]:
                        infrastructure_failures.append(
                            f"{model}: gradient confirmation benchmark")
                    gradient_failures.extend(gradient_gate(
                        model, gradient, gradient_confirmation))

            model_summary = {
                "model": model,
                "mir_changed": mir_changed,
                "changed_values": changed_values,
                "points": model_points,
                "compile": compile_info,
                "reroll": summarize_reroll(model_graph_records),
                "reference_kind": reference_kind,
            }
            if gradient is not None:
                model_summary["gradient"] = gradient
            if gradient_confirmation is not None:
                model_summary["gradient_confirmation"] = gradient_confirmation
            model_summaries.append(model_summary)
            ratio = gradient.get("on_over_off") if gradient else None
            ratio_text = f", gradient on/off {ratio:.4f}" \
                if ratio is not None else ""
            print(
                f"{model}: MIR {'changed' if mir_changed else 'unchanged'}, "
                f"{changed_values} arithmetic-order value changes"
                f"{ratio_text}")

    baseline_comparison = None
    if args.baseline is not None:
        baseline_dir = args.baseline.resolve()
        baseline_comparison = baseline_comparison_report(
            baseline_dir, load_cells(baseline_dir),
            cells_from_records(graph_records))

    manifest["harness_elapsed_ns"] = time.monotonic_ns() - harness_started_ns
    summary = write_reports(
        output_dir, manifest, corpus_records, graph_records, model_summaries,
        failures, infrastructure_failures, op_count_diagnostics,
        gradient_failures, baseline_comparison=baseline_comparison)
    print(
        f"\n{summary['models']} models, {summary['points']} points, "
        f"{len(failures)} semantic failures, "
        f"{len(op_count_diagnostics)} op count diagnostics, "
        f"{len(gradient_failures)} gradient time failures, "
        f"{len(infrastructure_failures)} measurement failures")
    print(f"report: {output_dir}")
    return 0 if summary["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())

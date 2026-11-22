#!/usr/bin/env python3
"""Generate the MIR test corpus from its checked-in Stan sources."""

from __future__ import annotations

import argparse
import concurrent.futures
import os
import pathlib
import re
import subprocess


REPO = pathlib.Path(__file__).resolve().parents[1]

# These are source-level lit corpora. The lit runner compiles them when each
# test runs, so also materializing their MIR in tests/fixtures only makes
# every ordinary build process a large duplicate corpus.
SOURCE_ONLY_PATTERN = re.compile(
    r"(?:builtin|density)_signatures_[0-9]+\.stan\Z")

# The other fixtures run stanc3's O1 passes with function inlining and
# partial evaluation, which the shipping policy turns off, so that the tests
# keep seeing inlined function bodies and partially evaluated expressions.
# This is not stock stanc's O1: stanli's compiler keeps preserve_stability on
# and splits tuples, and block_fixing, allow_uninitialized_decls and
# optimize_soa stay off, as in the shipping policy.
O1_PASSES = ("function_inlining,list_collapsing,constant_propagation,"
             "copy_propagation,dead_code_elimination,partial_evaluation")

# These models exercise structured control that O1 rewrites away, so they
# get no passes. Keep the policy centralized here so CMake and the manual
# wrapper cannot generate different corpora.
O0_FIXTURES = {
    "call_argument_cache",
    "gq_udf_return_layout",
    "gq_rng_udf_effect",
    "gq_runtime_int_index",
    "gq_udf_branch_rng",
    "gq_udf_loop_return_o0",
    "gq_function_exits",
    "paramcond_effect_ternary",
    "paramcond_intarray",
    "paramcond_local_only",
    "region_map_cf_nested",
    "region_map_cf_variant_o0",
    "region_map_kept_call_inline_o0",
    "region_map_kept_call_o0",
    "reduce_sum_arguments",
    "region_constant_shadow",
    "region_local_shadow",
    "runtime_int_array_udf",
    "slice_between_o0",
    "slice_upfrom_o0",
    "structured_dead_after_break",
    "structured_matrix_ops",
    "structured_refused_branch",
    "structured_sequence_local",
    "structured_udf_local_capacity_o0",
    "structured_zero_matrix_udf",
    "trunc_param",
    "udf_branch_kernels",
    "udf_call_int_index",
    "udf_call_int_size",
    "udf_call_while_inline_o0",
    "udf_call_while_o0",
    "udf_conditional_return",
    "udf_constant_branch",
    "udf_data_actual_branch_inline_o0",
    "udf_data_actual_branch_o0",
    "udf_data_actual_index_inline_o0",
    "udf_data_actual_index_o0",
    "udf_guard_returns",
    "udf_lp_bound",
    "udf_lp_branch_propto",
    "udf_lp_condition",
    "udf_local_shape",
    "udf_lp_propto",
    "udf_lpdf_folded_normalized",
    "udf_truncate_forms",
    "udf_truncate_shapes",
    "udf_void_early_return",
    "udf_void_names",
    "udf_void_return_in_branch",
    "udf_void_statements",
    "viewc_effectful_int_local_o0",
    "viewc_effectful_int_udf_nested_o0",
    "viewc_effectful_int_udf_o0",
    "wa_target_lp",
    "wa_tp_lp_refused",
    "whileloop",
}


def fixture_sources(source_dir: pathlib.Path) -> list[pathlib.Path]:
    return [source for source in sorted(source_dir.glob("*.stan"))
            if SOURCE_ONLY_PATTERN.fullmatch(source.name) is None]


def generate(compiler: pathlib.Path, stan: pathlib.Path,
             output: pathlib.Path) -> None:
    passes = "none" if stan.stem in O0_FIXTURES else O1_PASSES
    completed = subprocess.run(
        [str(compiler), "--emit-mir", stan.relative_to(REPO).as_posix(),
         "--passes", passes],
        cwd=REPO,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    # The compiler's warnings are expected for many fixtures; show its
    # output only when it fails.
    if completed.returncode:
        raise RuntimeError(f"{stan}: {completed.stderr.decode(errors='replace')}")

    text = completed.stdout
    if output.exists() and output.read_bytes() == text:
        # The build rule also depends on this script and the compiler. Refresh
        # an unchanged output so make/ninja does not rerun the whole corpus on
        # every build after either input changes.
        output.touch()
        return
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_name(output.name + ".tmp")
    temporary.write_bytes(text)
    temporary.replace(output)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--stanli-check", type=pathlib.Path,
                        help="the stanli_check executable that compiles "
                        "each fixture (stanli_check --emit-mir)")
    parser.add_argument("--source-dir", default="tests/fixtures",
                        type=pathlib.Path)
    parser.add_argument("--output-dir", default="tests/fixtures",
                        type=pathlib.Path)
    parser.add_argument("--list-sources", action="store_true",
                        help="print the fixture sources and exit")
    parser.add_argument("--jobs", type=int, default=min(8, os.cpu_count() or 1))
    args = parser.parse_args()

    source_dir = (REPO / args.source_dir).resolve()
    output_dir = (REPO / args.output_dir).resolve()
    sources = fixture_sources(source_dir)
    if not sources:
        parser.error(f"no .stan fixtures found in {source_dir}")
    if args.list_sources:
        for source in sources:
            print(source)
        return 0

    if args.stanli_check is None or not args.stanli_check.is_file():
        parser.error(f"stanli_check not found: {args.stanli_check}")
    compiler = args.stanli_check.expanduser().resolve()

    # *.tmir.sexp is the stock stanc corpus that this one replaced.
    expected = {f"{stan.stem}.mir" for stan in sources}
    for stale in [*output_dir.glob("*.mir"), *output_dir.glob("*.tmir.sexp")]:
        if stale.name not in expected:
            stale.unlink()

    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        for done in [pool.submit(generate, compiler, stan,
                                 output_dir / f"{stan.stem}.mir")
                     for stan in sources]:
            done.result()
    print(f"generated {len(sources)} MIR fixtures in {output_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

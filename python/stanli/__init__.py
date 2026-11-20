"""stanli: the Stan Language Interpreter.

Compiles a .stan model with stanc3 (linked into the bundled shared library,
or a bundled compiler executable as a subprocess where it is not), lowers it
to an op graph in-process, and samples with NUTS. No C++ toolchain, no model
compilation on this machine.
"""
import base64
import ctypes
from collections.abc import Mapping
import json
import math
import numbers
import operator
import os
import pathlib
import re
import signal
import subprocess
import sys
import threading
import warnings

import numpy as np

__all__ = ["Model", "Function", "Fit", "Summary", "OptimizeResult",
           "laplace_whiten", "LaplaceWhitenReport",
           "exact_lp", "thread_safe", "stan_to_mir", "build_id",
           "bridgestan_model", "bridgestan_data", "runtime_path",
           "SAMPLER_COLUMNS", "O1FallbackWarning", "ExperimentalWarning",
           "__version__"]
# The one place the version lives. setup.py and the release workflow both
# read it from here.
__version__ = "0.30.1"

_BIN = pathlib.Path(__file__).parent / "_bin"

# The C ABI this module's ctypes declarations are written against: must
# equal STANLI_ABI_VERSION in runtime/include/stanli/capi.h.
_ABI_VERSION = 6

# stanli_compiler_message_kind in capi.h.
_COMPILER_WARNING = 0
_COMPILER_O1_FALLBACK = 1
# The column sets of stanli_n_variables.
_CONSTRAINED_COLUMNS = 0
_WRITE_ARRAY_COLUMNS = 1


class O1FallbackWarning(RuntimeWarning):
    """The compiler skipped O1 for this model and it runs from O0 MIR.

    Issued when the model's structure exceeds stanli's O1 budget. The model
    is correct but may be slower; ``STANLI_NO_O1_FALLBACK=1`` forces O1.
    """


class ExperimentalWarning(UserWarning):
    """An experimental feature was used. Issued once per process per feature."""


_experimental_notified = set()


def _experimental(feature, advice):
    if feature in _experimental_notified:
        return
    _experimental_notified.add(feature)
    warnings.warn(f"{feature} is experimental. {advice}",
                  ExperimentalWarning, stacklevel=3)


_LAPLACE_WHITENING_ADVICE = (
    "A whitened fit can be wrong without any sampler warning, so compare it "
    "against one fit with the default sampler. See docs/laplace-whitening.md.")


class _OptimizeOpts(ctypes.Structure):
    """Mirrors stanli_optimize_opts in runtime/include/stanli/capi.h."""
    _fields_ = [
        ("seed", ctypes.c_uint32),
        ("chain_id", ctypes.c_int),
        ("iter", ctypes.c_int),
        ("jacobian", ctypes.c_int),
        ("init_alpha", ctypes.c_double),
        ("tol_obj", ctypes.c_double),
        ("tol_rel_obj", ctypes.c_double),
        ("tol_grad", ctypes.c_double),
        ("tol_rel_grad", ctypes.c_double),
        ("tol_param", ctypes.c_double),
        ("history_size", ctypes.c_int),
        ("init_radius", ctypes.c_double),
        ("init", ctypes.POINTER(ctypes.c_double)),
    ]


class _SampleOpts(ctypes.Structure):
    """Mirrors stanli_sample_opts in runtime/include/stanli/capi.h.

    Field order and types must match the header exactly. A mismatch is a
    silent misread rather than an error -- the run samples successfully
    from the wrong configuration -- so tests/test_python.py checks the
    defaults this struct comes back with against the documented ones.
    """
    _fields_ = [
        ("seed", ctypes.c_uint32),
        ("chains", ctypes.c_int),
        ("chain_id", ctypes.c_int),
        ("warmup", ctypes.c_int),
        ("samples", ctypes.c_int),
        ("thin", ctypes.c_int),
        ("delta", ctypes.c_double),
        ("max_depth", ctypes.c_int),
        ("save_warmup", ctypes.c_int),
        ("init_radius", ctypes.c_double),
        ("inits", ctypes.POINTER(ctypes.c_double)),
        ("num_threads", ctypes.c_int),
        ("resume_in", ctypes.POINTER(ctypes.c_void_p)),
        ("resume_in_len", ctypes.POINTER(ctypes.c_size_t)),
        ("resume_out", ctypes.c_void_p),
    ]


class _SampleReport(ctypes.Structure):
    """Mirrors stanli_sample_report in runtime/include/stanli/capi.h."""
    _fields_ = [
        ("warmup_seconds", ctypes.c_double),
        ("sampling_seconds", ctypes.c_double),
        ("n_divergent", ctypes.c_int64),
        ("n_max_treedepth", ctypes.c_int64),
    ]


_SampleProgressCallback = ctypes.CFUNCTYPE(
    None, ctypes.c_int32, ctypes.c_int64, ctypes.c_int64, ctypes.c_int32,
    ctypes.c_int64, ctypes.c_int64, ctypes.c_void_p)
_SamplePollCallback = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p)


class _MetricOpts(ctypes.Structure):
    """Mirrors stanli_metric_opts in runtime/include/stanli/capi.h."""
    _fields_ = [
        ("metric", ctypes.c_int),
        ("inv_metric", ctypes.POINTER(ctypes.c_double)),
        ("step_size", ctypes.c_double),
        ("adapt_engaged", ctypes.c_int),
        ("adapt_metric", ctypes.c_int),
        ("adapted_step_size", ctypes.POINTER(ctypes.c_double)),
        ("adapted_inv_metric", ctypes.POINTER(ctypes.c_double)),
    ]


_METRICS = ("unit_e", "diag_e", "dense_e")


class _ModelOpts(ctypes.Structure):
    """Mirrors stanli_model_opts in runtime/include/stanli/capi.h."""
    _fields_ = [("seed", ctypes.c_uint32),
                ("threads_per_chain", ctypes.c_int),
                ("fast_math", ctypes.c_int),
                ("allow_interpreter", ctypes.c_int),
                ("execution_report", ctypes.c_int),
                ("parallel_write_array", ctypes.c_int)]


# stanli_parallel_wa: where write_array is lowered while a model is built.
_PARALLEL_WRITE_ARRAY = {"auto": 0, "off": 1, "force": 2}


class _FieldLayout(ctypes.Structure):
    _fields_ = [("type", ctypes.c_char_p), ("field", ctypes.c_char_p),
                ("offset", ctypes.c_size_t), ("size", ctypes.c_size_t),
                ("type_size", ctypes.c_size_t)]


_MIRRORS = {"stanli_optimize_opts": _OptimizeOpts,
            "stanli_sample_opts": _SampleOpts,
            "stanli_sample_report": _SampleReport,
            "stanli_metric_opts": _MetricOpts,
            "stanli_model_opts": _ModelOpts}


def _check_layouts(lib, mirrors=None):
    """Refuse a runtime whose structs differ from the ctypes mirrors here.

    A mirror with a field at the wrong offset does not fail: the call reads
    the wrong bytes and runs with the wrong settings.
    """
    mirrors = _MIRRORS if mirrors is None else mirrors
    lib.stanli_n_field_layouts.restype = ctypes.c_int64
    lib.stanli_field_layouts.restype = ctypes.POINTER(_FieldLayout)
    table = lib.stanli_field_layouts()
    runtime = {}
    for i in range(lib.stanli_n_field_layouts()):
        e = table[i]
        runtime.setdefault(e.type.decode(), (e.type_size, {}))[1][
            e.field.decode()] = (e.offset, e.size)
    for name, mirror in mirrors.items():
        type_size, fields = runtime.get(name, (None, {}))
        ours = {f: (getattr(mirror, f).offset, getattr(mirror, f).size)
                for f, _ in mirror._fields_}
        if type_size != ctypes.sizeof(mirror) or ours != fields:
            raise ImportError(
                f"the stanli runtime's {name} differs from this package's "
                f"copy (runtime {type_size} bytes {fields}, package "
                f"{ctypes.sizeof(mirror)} bytes {ours}); reinstall stanli")


def _load_lib():
    names = {"darwin": "libstanli.dylib", "linux": "libstanli.so"}
    lib = ctypes.CDLL(str(_BIN / names.get(sys.platform, "stanli.dll")))
    lib.stanli_abi_version.restype = ctypes.c_int
    if lib.stanli_abi_version() != _ABI_VERSION:
        raise ImportError(
            f"the stanli runtime speaks C ABI {lib.stanli_abi_version()}, "
            f"this package {_ABI_VERSION}; reinstall stanli")
    _check_layouts(lib)
    lib.stanli_model_free.argtypes = [ctypes.c_void_p]
    lib.stanli_n_unconstrained.restype = ctypes.c_int64
    lib.stanli_n_unconstrained.argtypes = [ctypes.c_void_p]
    lib.stanli_grad.restype = ctypes.c_int
    lib.stanli_grad.argtypes = [ctypes.c_void_p,
                                ctypes.POINTER(ctypes.c_double),
                                ctypes.POINTER(ctypes.c_double),
                                ctypes.POINTER(ctypes.c_double)]
    lib.stanli_hessian.restype = ctypes.c_int
    lib.stanli_hessian.argtypes = [ctypes.c_void_p,
                                   ctypes.POINTER(ctypes.c_double),
                                   ctypes.POINTER(ctypes.c_double),
                                   ctypes.POINTER(ctypes.c_double),
                                   ctypes.POINTER(ctypes.c_double),
                                   ctypes.c_char_p, ctypes.c_size_t]
    lib.stanli_model_whiten.restype = ctypes.c_void_p
    lib.stanli_model_whiten.argtypes = [
        ctypes.c_void_p, ctypes.POINTER(ctypes.c_double), ctypes.c_int64,
        ctypes.POINTER(ctypes.c_int64), ctypes.POINTER(ctypes.c_int64),
        ctypes.POINTER(ctypes.c_int64), ctypes.POINTER(ctypes.c_double),
        ctypes.c_int64, ctypes.c_char_p, ctypes.c_size_t]
    lib.stanli_model_whiten_lowrank.restype = ctypes.c_void_p
    lib.stanli_model_whiten_lowrank.argtypes = [
        ctypes.c_void_p, ctypes.POINTER(ctypes.c_double), ctypes.c_int64,
        ctypes.POINTER(ctypes.c_int64), ctypes.POINTER(ctypes.c_int64),
        ctypes.POINTER(ctypes.c_int64), ctypes.POINTER(ctypes.c_double),
        ctypes.c_int64, ctypes.c_int64, ctypes.POINTER(ctypes.c_double),
        ctypes.POINTER(ctypes.c_double), ctypes.POINTER(ctypes.c_double),
        ctypes.c_char_p, ctypes.c_size_t]
    lib.stanli_unconstrain_inits.restype = ctypes.c_int
    lib.stanli_unconstrain_inits.argtypes = [ctypes.c_void_p, ctypes.c_char_p,
                                             ctypes.POINTER(ctypes.c_double),
                                             ctypes.c_char_p, ctypes.c_size_t]
    lib.stanli_n_constrained.restype = ctypes.c_int64
    lib.stanli_n_constrained.argtypes = [ctypes.c_void_p]
    lib.stanli_constrained_name.restype = ctypes.c_char_p
    lib.stanli_constrained_name.argtypes = [ctypes.c_void_p, ctypes.c_int64]
    lib.stanli_n_param_blocks.restype = ctypes.c_int64
    lib.stanli_n_param_blocks.argtypes = [ctypes.c_void_p]
    lib.stanli_param_block_name.restype = ctypes.c_char_p
    lib.stanli_param_block_name.argtypes = [ctypes.c_void_p, ctypes.c_int64]
    lib.stanli_param_block_size.restype = ctypes.c_int64
    lib.stanli_param_block_size.argtypes = [ctypes.c_void_p, ctypes.c_int64]
    lib.stanli_has_embedded_stanc.restype = ctypes.c_int
    lib.stanli_exact_lp.restype = ctypes.c_int
    lib.stanli_reduce_sum_count.restype = ctypes.c_int
    lib.stanli_reduce_sum_count.argtypes = [ctypes.c_void_p]
    lib.stanli_n_reduce_sum_fallbacks.restype = ctypes.c_int64
    lib.stanli_n_reduce_sum_fallbacks.argtypes = [ctypes.c_void_p]
    lib.stanli_reduce_sum_fallback.restype = ctypes.c_char_p
    lib.stanli_reduce_sum_fallback.argtypes = [ctypes.c_void_p, ctypes.c_int64]
    lib.stanli_model_new_with_opts.restype = ctypes.c_void_p
    lib.stanli_model_new_with_opts.argtypes = [
        ctypes.c_char_p, ctypes.c_char_p, ctypes.POINTER(_ModelOpts),
        ctypes.c_char_p, ctypes.c_size_t]
    lib.stanli_compile_stan.restype = ctypes.c_void_p
    lib.stanli_compile_stan.argtypes = [
        ctypes.c_char_p, ctypes.c_char_p, ctypes.POINTER(ctypes.c_char_p),
        ctypes.c_size_t, ctypes.c_int]
    lib.stanli_compilation_mir.restype = ctypes.c_char_p
    lib.stanli_compilation_mir.argtypes = [ctypes.c_void_p]
    lib.stanli_compilation_error.restype = ctypes.c_char_p
    lib.stanli_compilation_error.argtypes = [ctypes.c_void_p]
    lib.stanli_compilation_n_messages.restype = ctypes.c_int64
    lib.stanli_compilation_n_messages.argtypes = [ctypes.c_void_p]
    lib.stanli_compilation_message_kind.restype = ctypes.c_int
    lib.stanli_compilation_message_kind.argtypes = [ctypes.c_void_p,
                                                    ctypes.c_int64]
    lib.stanli_compilation_message.restype = ctypes.c_char_p
    lib.stanli_compilation_message.argtypes = [ctypes.c_void_p,
                                               ctypes.c_int64]
    lib.stanli_compilation_free.argtypes = [ctypes.c_void_p]
    lib.stanli_model_n_compiler_messages.restype = ctypes.c_int64
    lib.stanli_model_n_compiler_messages.argtypes = [ctypes.c_void_p]
    lib.stanli_model_compiler_message_kind.restype = ctypes.c_int
    lib.stanli_model_compiler_message_kind.argtypes = [ctypes.c_void_p,
                                                       ctypes.c_int64]
    lib.stanli_model_compiler_message.restype = ctypes.c_char_p
    lib.stanli_model_compiler_message.argtypes = [ctypes.c_void_p,
                                                  ctypes.c_int64]
    lib.stanli_n_variables.restype = ctypes.c_int64
    lib.stanli_n_variables.argtypes = [ctypes.c_void_p, ctypes.c_int]
    lib.stanli_variable_name.restype = ctypes.c_char_p
    lib.stanli_variable_name.argtypes = [ctypes.c_void_p, ctypes.c_int,
                                         ctypes.c_int64]
    lib.stanli_variable_n_dims.restype = ctypes.c_int64
    lib.stanli_variable_n_dims.argtypes = [ctypes.c_void_p, ctypes.c_int,
                                           ctypes.c_int64]
    lib.stanli_variable_dim.restype = ctypes.c_int64
    lib.stanli_variable_dim.argtypes = [ctypes.c_void_p, ctypes.c_int,
                                        ctypes.c_int64, ctypes.c_int64]
    lib.stanli_variable_n_annotations.restype = ctypes.c_int64
    lib.stanli_variable_n_annotations.argtypes = [ctypes.c_void_p,
                                                  ctypes.c_int, ctypes.c_int64]
    lib.stanli_variable_annotation.restype = ctypes.c_char_p
    lib.stanli_variable_annotation.argtypes = [ctypes.c_void_p, ctypes.c_int,
                                               ctypes.c_int64, ctypes.c_int64]
    lib.stanli_model_new_from_stan_named.restype = ctypes.c_void_p
    lib.stanli_model_new_from_stan_named.argtypes = [
        ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p,
        ctypes.POINTER(_ModelOpts), ctypes.c_char_p, ctypes.c_size_t]
    lib.stanli_string_free.argtypes = [ctypes.c_void_p]
    lib.stanli_capture_messages.argtypes = [ctypes.c_int]
    lib.stanli_take_messages.restype = ctypes.c_void_p
    lib.stanli_take_messages.argtypes = []
    lib.stanli_build_id.restype = ctypes.c_char_p
    lib.stanli_build_id.argtypes = []
    lib.stanli_wa_n_columns.restype = ctypes.c_int64
    lib.stanli_wa_n_columns.argtypes = [ctypes.c_void_p]
    lib.stanli_wa_column_name.restype = ctypes.c_char_p
    lib.stanli_wa_column_name.argtypes = [ctypes.c_void_p, ctypes.c_int64]
    lib.stanli_warnings.restype = ctypes.c_char_p
    lib.stanli_warnings.argtypes = [ctypes.c_void_p]
    lib.stanli_transformed_data_rng.restype = ctypes.c_int
    lib.stanli_transformed_data_rng.argtypes = [ctypes.c_void_p]
    lib.stanli_wa_row.restype = ctypes.c_int
    lib.stanli_wa_row.argtypes = [ctypes.c_void_p,
                                  ctypes.POINTER(ctypes.c_double),
                                  ctypes.POINTER(ctypes.c_double)]
    # Multi-chain sampling, summaries, diagnostics.
    lib.stanli_sample_opts_init.argtypes = [ctypes.POINTER(_SampleOpts)]
    lib.stanli_sample_opts_check.restype = ctypes.c_int
    lib.stanli_sample_opts_check.argtypes = [
        ctypes.POINTER(_SampleOpts), ctypes.c_char_p, ctypes.c_size_t]
    lib.stanli_n_stored_draws.restype = ctypes.c_int64
    lib.stanli_n_stored_draws.argtypes = [ctypes.POINTER(_SampleOpts)]
    lib.stanli_thread_safe.restype = ctypes.c_int
    lib.stanli_resume_size.restype = ctypes.c_size_t
    lib.stanli_resume_size.argtypes = [ctypes.c_void_p]
    lib.stanli_metric_opts_init.argtypes = [ctypes.POINTER(_MetricOpts)]
    lib.stanli_sample_multi_metric.restype = ctypes.c_int
    lib.stanli_sample_multi_metric.argtypes = [
        ctypes.c_void_p, ctypes.POINTER(_SampleOpts),
        ctypes.POINTER(_MetricOpts), ctypes.c_int,
        ctypes.POINTER(ctypes.c_double), ctypes.POINTER(ctypes.c_double),
        ctypes.POINTER(ctypes.c_double),
        _SampleProgressCallback, ctypes.c_void_p, _SamplePollCallback,
        ctypes.c_void_p, ctypes.POINTER(ctypes.c_int),
        ctypes.POINTER(_SampleReport), ctypes.c_char_p, ctypes.c_size_t]
    lib.stanli_pathfinder_inits.restype = ctypes.c_int
    lib.stanli_pathfinder_inits.argtypes = [
        ctypes.c_void_p, ctypes.c_uint32, ctypes.c_int, ctypes.c_int,
        ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_double,
        ctypes.POINTER(ctypes.c_double), ctypes.c_void_p, ctypes.c_void_p,
        ctypes.c_char_p, ctypes.c_size_t]
    lib.stanli_summary_stats.restype = ctypes.c_int
    lib.stanli_summary_stats.argtypes = [ctypes.POINTER(ctypes.c_double),
                                         ctypes.c_int64, ctypes.c_int64,
                                         ctypes.c_int64,
                                         ctypes.POINTER(ctypes.c_double)]
    lib.stanli_optimize_opts_init.argtypes = [ctypes.POINTER(_OptimizeOpts)]
    lib.stanli_optimize.restype = ctypes.c_int
    lib.stanli_optimize.argtypes = [ctypes.c_void_p,
                                    ctypes.POINTER(_OptimizeOpts),
                                    ctypes.POINTER(ctypes.c_double),
                                    ctypes.POINTER(ctypes.c_double),
                                    ctypes.POINTER(ctypes.c_double),
                                    ctypes.c_char_p, ctypes.c_size_t]
    lib.stanli_diagnose_text.restype = ctypes.c_int64
    lib.stanli_diagnose_text.argtypes = [ctypes.POINTER(ctypes.c_double),
                                         ctypes.c_int64, ctypes.c_int64,
                                         ctypes.c_int64,
                                         ctypes.POINTER(ctypes.c_char_p),
                                         ctypes.POINTER(ctypes.c_double),
                                         ctypes.c_int, ctypes.c_char_p,
                                         ctypes.c_size_t]
    lib.stanli_sampling_warnings.restype = ctypes.c_int64
    lib.stanli_sampling_warnings.argtypes = [
        ctypes.POINTER(_SampleOpts), ctypes.POINTER(_SampleReport),
        ctypes.POINTER(ctypes.c_double), ctypes.c_char_p, ctypes.c_size_t]
    lib.stanli_sampler_column_name.restype = ctypes.c_char_p
    lib.stanli_sampler_column_name.argtypes = [ctypes.c_int]
    lib.stanli_summary_stat_name.restype = ctypes.c_char_p
    lib.stanli_summary_stat_name.argtypes = [ctypes.c_int]
    return lib


_lib = _load_lib()
_lib.stanli_capture_messages(1)


def _flush_messages():
    """Write the runtime's queued messages to the current sys.stdout."""
    p = _lib.stanli_take_messages()
    if not p:
        return
    try:
        text = ctypes.cast(p, ctypes.c_char_p).value.decode("utf-8", "replace")
    finally:
        _lib.stanli_string_free(p)
    out = sys.stdout
    if out is not None:
        out.write(text)
        out.flush()


class _native_output:
    """Delivers the runtime's printed messages when a native call ends."""

    def __enter__(self):
        return self

    def __exit__(self, exc_type, *_rest):
        try:
            _flush_messages()
        except Exception:
            if exc_type is None:
                raise
        return False

# The column names are the ABI's, not ours; the R bridge reads the same
# accessor.
_N_SAMPLER_COLS = _lib.stanli_n_sampler_columns()
SAMPLER_COLUMNS = tuple(_lib.stanli_sampler_column_name(i).decode()
                        for i in range(_N_SAMPLER_COLS))
_SUMMARY_STATS = tuple(_lib.stanli_summary_stat_name(i).decode()
                       for i in range(_lib.stanli_n_summary_stats()))
_N_SUMMARY_STATS = len(_SUMMARY_STATS)


def _dptr(a):
    return a.ctypes.data_as(ctypes.POINTER(ctypes.c_double))


def _bracket(name):
    """CmdStan's CSV name in CmdStanPy's form: theta.1.2 -> theta[1,2],
    g.1:2.3 -> g[1].2[3]."""
    if "[" in name:
        return name
    parts = name.split(":")
    for i, part in enumerate(parts):
        head, *index = part.split(".")
        if index:
            parts[i] = head + "[" + ",".join(index) + "]"
    return ".".join(parts)


def _scalar_variables(names):
    """Every column a scalar variable under its own name."""
    return {n: ((), np.array(i, dtype=np.int64)) for i, n in enumerate(names)}


def _variable_columns(shapes):
    """Column names in Stan's order, ``theta[1,2]`` for an element."""
    out = []
    for name, dims in shapes:
        name = _bracket(name)
        if not dims:
            out.append(name)
            continue
        for col in np.ndindex(*reversed(dims)):
            out.append(name + "[" + ",".join(str(i + 1) for i in reversed(col))
                       + "]")
    return out


class _TupleArray:
    """An array of tuples: its dims and its elements, first index fastest."""

    def __init__(self, dims, elements):
        self.dims = dims
        self.elements = elements


def _tuple_path(name):
    """A CmdStan tuple column name as its base and its steps: an int for a
    slot, a tuple of ints for an array index. h.1.2:2 -> h, [(1, 2), 2]."""
    head, *rest = name.split(":")
    base, *index = head.split(".")
    path = [tuple(int(i) for i in index)] if index else []
    for part in rest:
        slot, *index = part.split(".")
        path.append(int(slot))
        if index:
            path.append(tuple(int(i) for i in index))
    return base, path


def _tuple_node(entries):
    """A tuple (a Python tuple of nodes) or a _TupleArray from nested
    {step: entry}; a leaf is its array of column indices."""
    if not isinstance(entries, dict):
        return entries
    keys = list(entries)
    if isinstance(keys[0], int):
        return tuple(_tuple_node(entries[k]) for k in sorted(keys))
    dims = tuple(max(k[d] for k in keys) for d in range(len(keys[0])))
    order = sorted(keys, key=lambda k: tuple(reversed(k)))
    return _TupleArray(dims, [_tuple_node(entries[k]) for k in order])


def _tuple_values(node, draws):
    """CmdStanPy's stan_variable values for a tuple node: an object array
    of (rows, *dims) from draws of (rows, columns)."""
    if isinstance(node, np.ndarray):
        return draws[:, node]
    rows = draws.shape[0]
    if isinstance(node, _TupleArray):
        out = np.empty((rows,) + node.dims, dtype=object)
        for pos, element in zip(np.ndindex(*reversed(node.dims)),
                                node.elements):
            out[(slice(None),) + tuple(reversed(pos))] = \
                _tuple_values(element, draws)
        return out
    parts = [_tuple_values(slot, draws) for slot in node]
    out = np.empty(rows, dtype=object)
    for k in range(rows):
        out[k] = tuple(part[k] for part in parts)
    return out


def _tuple_leaves(name, node):
    """[(column-style name, column indices)] for each leaf of a tuple."""
    if isinstance(node, np.ndarray):
        return [(name, node)]
    if isinstance(node, _TupleArray):
        return [leaf for pos, element in zip(np.ndindex(*reversed(node.dims)),
                                             node.elements)
                for leaf in _tuple_leaves(
                    name + "[" + ",".join(str(i + 1) for i in reversed(pos))
                    + "]", element)]
    return [leaf for i, slot in enumerate(node, 1)
            for leaf in _tuple_leaves(f"{name}.{i}", slot)]


def _variable_indices(idx):
    """The columns of a variable, in column order."""
    if isinstance(idx, np.ndarray):
        return [int(c) for c in np.atleast_1d(idx).flatten("F")]
    return [int(c) for _, leaf in _tuple_leaves("", idx)
            for c in np.atleast_1d(leaf).flatten("F")]


def _variables_from_shapes(shapes):
    """{variable: (dims, index_array)} from the runtime's (name, dims) list.

    Variables cover consecutive columns in order, first index fastest. A
    tuple is one variable under its base name, with the dims of its array
    (none for a plain tuple) and its slots as a tree of index arrays.
    """
    out, tuples, start = {}, {}, 0
    for name, dims in shapes:
        size = int(np.prod(dims, dtype=np.int64)) if dims else 1
        cols = np.arange(start, start + size, dtype=np.int64)
        idx = (cols.reshape(dims, order="F") if dims
               else np.array(start, dtype=np.int64))
        start += size
        if ":" not in name:
            out[name] = (dims, idx)
            continue
        base, path = _tuple_path(name)
        out.setdefault(base, None)
        node = tuples.setdefault(base, {})
        for step in path[:-1]:
            node = node.setdefault(step, {})
        node[path[-1]] = idx
    for base, entries in tuples.items():
        node = _tuple_node(entries)
        out[base] = (node.dims if isinstance(node, _TupleArray) else (), node)
    return out


def _resolve(name, variables, columns):
    """(dims, index_array) for a variable, flat bracket, or dot name."""
    if name in variables:
        return variables[name]
    if name in columns:
        return (), np.array(columns[name], dtype=np.int64)
    bracketed = _bracket(name)
    if bracketed in variables:
        return variables[bracketed]
    if bracketed in columns:
        return (), np.array(columns[bracketed], dtype=np.int64)
    raise ValueError(f"{name!r} is not a column or variable of this fit")


class _InterruptWatch:
    """Turns Ctrl-C during a native call into a stop the sampler polls.

    Python only runs its SIGINT handler when the main thread executes
    bytecode, which during a foreign call is exactly the poll callback. A
    handler that raised there would be swallowed by ctypes, so this one
    records the signal and the poll answers it; the caller raises once the
    sampler has returned. Off the main thread no handler can be installed
    and the poll simply never fires.
    """

    def __init__(self):
        self.tripped = False
        self.previous = None
        self.flush_error = None
        self.poll = _SamplePollCallback(self._poll)

    def _poll(self, _user):
        if self.flush_error is None:
            try:
                _flush_messages()
            except BaseException as exc:  # ctypes cannot propagate it
                self.flush_error = exc
        return 1 if self.tripped else 0

    def _handle(self, _signum, _frame):
        self.tripped = True

    def __enter__(self):
        if threading.current_thread() is threading.main_thread():
            self.previous = signal.signal(signal.SIGINT, self._handle)
        return self

    def __exit__(self, *_exc):
        if self.previous is not None:
            signal.signal(signal.SIGINT, self.previous)
        return False

    def raise_interrupt(self):
        previous = self.previous
        if callable(previous) and previous is not signal.default_int_handler:
            previous(signal.SIGINT, None)
        raise KeyboardInterrupt


_PATHFINDER_INIT_DEFAULTS = {
    "num_iterations": 1000,
    "num_elbo_draws": 25,
    "history_size": 5,
    "init_radius": 2.0,
}


def _pathfinder_init_options(value):
    """Validate the small, deliberately separate Pathfinder-init surface."""
    if not isinstance(value, Mapping):
        raise TypeError("pathfinder_init must be a mapping of options")
    unknown = set(value) - set(_PATHFINDER_INIT_DEFAULTS)
    if unknown:
        raise ValueError(
            "unknown pathfinder_init option" + ("s" if len(unknown) > 1 else "")
            + ": " + ", ".join(sorted(map(str, unknown))))
    out = dict(_PATHFINDER_INIT_DEFAULTS)
    out.update(value)
    int_max = 2 ** (8 * ctypes.sizeof(ctypes.c_int) - 1) - 1
    for name in ("num_iterations", "num_elbo_draws", "history_size"):
        raw = out[name]
        if isinstance(raw, (bool, np.bool_)):
            raise TypeError(f"pathfinder_init {name} must be a positive integer")
        try:
            parsed = operator.index(raw)
        except TypeError:
            raise TypeError(
                f"pathfinder_init {name} must be a positive integer") from None
        if parsed <= 0:
            raise ValueError(f"pathfinder_init {name} must be positive")
        if parsed > int_max:
            raise OverflowError(f"pathfinder_init {name} does not fit in a C int")
        out[name] = parsed
    radius = out["init_radius"]
    if isinstance(radius, (bool, np.bool_)):
        raise TypeError("pathfinder_init init_radius must be nonnegative")
    try:
        radius = float(radius)
    except (TypeError, ValueError):
        raise TypeError(
            "pathfinder_init init_radius must be nonnegative") from None
    if not math.isfinite(radius) or radius < 0:
        raise ValueError(
            "pathfinder_init init_radius must be finite and nonnegative")
    out["init_radius"] = radius
    return out


def _compiler_command(model_path: pathlib.Path, include_paths=(),
                      fast_math=False, filename=None, messages_json=None):
    """The argv for the packaged stanli-compile."""
    suffix = ".exe" if sys.platform == "win32" else ""
    portable = _BIN / ("stanli-compile" + suffix)
    if not portable.is_file():
        raise RuntimeError("the bundled Stan compiler is missing "
                           f"(expected {portable.name} in {_BIN})")
    flags = [arg for path in include_paths for arg in ("--include-path", path)]
    if fast_math:
        flags.insert(0, "--fast-math")
    if filename is not None:
        flags.extend(["--filename", filename])
    if messages_json is not None:
        flags.extend(["--messages-json", str(messages_json)])
    return [str(portable), *flags, str(model_path)]


def _warn_compile(text, category=RuntimeWarning):
    """Report compiler warnings once, attributed to the first caller outside
    this package."""
    if not text:
        return
    level, frame = 1, sys._getframe(1)
    while frame is not None and frame.f_globals.get("__package__") == __package__:
        level, frame = level + 1, frame.f_back
    warnings.warn(text, category, stacklevel=level + 1)


def _warn_compiler_messages(kinds_and_texts):
    for kind, text in kinds_and_texts:
        _warn_compile(text, O1FallbackWarning if kind == _COMPILER_O1_FALLBACK
                      else RuntimeWarning)


def _read_compiler_messages(path):
    """stanli-compile's --messages-json file as (kind, text) pairs."""
    import json
    messages = json.loads(pathlib.Path(path).read_text(encoding="utf-8"))
    out = [(_COMPILER_WARNING, text) for text in messages["warnings"]]
    if messages["o1_fallback"] is not None:
        out.append((_COMPILER_O1_FALLBACK, messages["o1_fallback"]))
    return out


def _stanc_mir(model_path: pathlib.Path, include_paths=(), fast_math=False,
               filename=None) -> str:
    import tempfile
    with tempfile.TemporaryDirectory(prefix="stanli-messages-") as tmpdir:
        messages = pathlib.Path(tmpdir) / "messages.json"
        argv = _compiler_command(model_path, include_paths, fast_math,
                                 filename, messages)
        try:
            r = subprocess.run(argv, capture_output=True, text=True,
                               encoding="utf-8")
        except OSError as exc:
            raise RuntimeError(
                f"could not run the bundled Stan compiler: {exc}") from exc
        if r.returncode != 0 or not r.stdout:
            raise RuntimeError(f"stanc failed:\n{r.stderr}")
        _warn_compiler_messages(_read_compiler_messages(messages))
    return r.stdout


def _subprocess_mir(stan_code: str, include_paths=(), fast_math=False,
                    filename=None) -> str:
    """Compile source in an isolated directory and remove every side effect."""
    import tempfile
    with tempfile.TemporaryDirectory(prefix="stanli-compile-") as tmpdir:
        source = pathlib.Path(tmpdir) / "model.stan"
        # Binary UTF-8 avoids the locale-dependent encoding and CRLF rewriting
        # of text-mode files on Windows.
        source.write_bytes(stan_code.encode("utf-8"))
        extra = {} if filename is None else {"filename": filename}
        if fast_math:
            return _stanc_mir(source, include_paths, True, **extra)
        return (_stanc_mir(source, include_paths, **extra) if include_paths
                else _stanc_mir(source, **extra))


def _read_utf8_file(path) -> str:
    """Read source bytes as UTF-8 without universal-newline rewriting."""
    return pathlib.Path(path).read_bytes().decode("utf-8")


def _include_paths(include_paths, stan_file=None):
    """Snapshot an ordered search path without changing the process directory."""
    if include_paths is None:
        include_paths = []
    elif isinstance(include_paths, (str, os.PathLike)):
        include_paths = [include_paths]
    paths = []
    for path in include_paths:
        if not isinstance(path, (str, os.PathLike)):
            raise TypeError("include_paths must contain directory paths")
        raw = os.fspath(path)
        if not isinstance(raw, str) or not raw or "\0" in raw:
            raise ValueError("include_paths must contain nonempty paths without NUL")
        directory = pathlib.Path(raw).expanduser().resolve()
        if not directory.is_dir():
            raise ValueError(f"include_paths directory does not exist: {directory}")
        paths.append(str(directory))
    paths.append(str(pathlib.Path(stan_file).resolve().parent
                     if stan_file is not None else pathlib.Path.cwd()))
    return list(dict.fromkeys(paths))


def _read_stan_source(stan_file, stan_code, include_paths):
    if stan_code is None:
        if stan_file is None:
            raise ValueError("provide stan_file, stan_code, or mir")
        stan_code = _read_utf8_file(stan_file)
    else:
        stan_file = None
    paths = (_include_paths(include_paths, stan_file)
             if include_paths is not None or "#include" in stan_code else [])
    return stan_code, paths


def stan_to_mir(stan_code: str, include_paths=None, fast_math=False) -> str:
    """Stan source to optimized-MIR text, without building a model.

    The first half of compiling a model, on its own. Useful when the MIR
    is what you want to keep -- to cache it, ship it, or hand it to
    ``Model(mir=...)`` in another process. The result is stanli's versioned
    portable format.

    ``include_paths`` is a directory or an iterable of directories searched
    in order for ``#include`` files, followed by the current directory.
    Nested includes use the same search path.

    ``fast_math=True`` compiles with the opt-in fast mode: the compiler
    rewrites expressions into fused multiply-adds, and the results can
    differ from the default in the last bits.
    """
    fast_math = _check_fast_math(fast_math)
    stan_code, paths = _read_stan_source(None, stan_code, include_paths)
    return _compile_stan(stan_code, paths, fast_math)


def _check_fast_math(value):
    if not isinstance(value, bool):
        raise TypeError("fast_math must be True or False")
    return value


def _compile_stan(stan_code, include_paths, fast_math=False, filename=None):
    include_paths = include_paths if "#include" in stan_code else []
    if not _lib.stanli_has_embedded_stanc():
        extra = {} if filename is None else {"filename": filename}
        if fast_math:
            return _subprocess_mir(stan_code, include_paths, True, **extra)
        return (_subprocess_mir(stan_code, include_paths, **extra)
                if include_paths else _subprocess_mir(stan_code, **extra))
    paths = (ctypes.c_char_p * len(include_paths))(
        *(path.encode("utf-8") for path in include_paths))
    with _native_output():
        c = _lib.stanli_compile_stan(
            stan_code.encode(), None if filename is None else filename.encode(),
            paths, len(paths), int(fast_math))
    if not c:
        raise MemoryError("stanli could not allocate a compilation")
    try:
        _warn_compiler_messages(
            (_lib.stanli_compilation_message_kind(c, i),
             _lib.stanli_compilation_message(c, i).decode())
            for i in range(_lib.stanli_compilation_n_messages(c)))
        mir = _lib.stanli_compilation_mir(c)
        if mir is None:
            raise RuntimeError(_lib.stanli_compilation_error(c).decode())
        return mir.decode()
    finally:
        _lib.stanli_compilation_free(c)


def _lib_suffix() -> str:
    return {"darwin": ".dylib", "win32": ".dll"}.get(sys.platform, ".so")


def runtime_path() -> pathlib.Path:
    """Path of the runtime library this package loaded.

    The library a BridgeStan client loads as the model library. The model
    itself travels in the data argument; see ``bridgestan_data``.
    """
    return _BIN / ("libstanli" + _lib_suffix() if sys.platform != "win32"
                   else "stanli.dll")


_runtime_lib_path = runtime_path


def _source_label(stan_file, stan_code):
    return None if stan_code is not None or stan_file is None else os.fspath(stan_file)


def _resolve_program(stan_file, stan_code, mir, name, include_paths=None):
    """(mir, name) from whichever form the caller has, compiling if needed."""
    if mir is None:
        label = _source_label(stan_file, stan_code)
        stan_code, paths = _read_stan_source(stan_file, stan_code, include_paths)
        mir = _compile_stan(stan_code, paths, filename=label)
    if name is None:
        name = pathlib.Path(stan_file).stem if stan_file else "stanli_model"
    return mir, name


def bridgestan_data(stan_file=None, stan_code=None, mir=None, data=None,
                    name=None, include_paths=None) -> str:
    """The data argument that makes the runtime serve this program to BridgeStan.

    The model travels inside the data argument: the manifest rides under
    the reserved key ``"__stanli"`` (never a data variable; Stan
    identifiers begin with a letter), and ``bs_model_construct`` reads it
    out before binding the rest as the model's data. Every model shares
    the one runtime library already inside this package, so nothing is
    copied and nothing touches disk. Path-form ``data`` is read and
    inlined to make room for the splice.

    Any BridgeStan client can sample the program by loading
    ``runtime_path()`` as the model library and passing the returned JSON
    text as the data. ``include_paths`` has the same search order as
    ``Model``.
    """
    mir, name = _resolve_program(stan_file, stan_code, mir, name, include_paths)
    return _bridgestan_payload(mir, name, _data_to_json(data), None)


def _bridgestan_payload(mir, name, data_json, whitening):
    payload = json.loads(data_json)
    if not isinstance(payload, dict):
        raise ValueError("data must be a JSON object")
    manifest = {"build_id": build_id(), "mir": mir, "name": name}
    if whitening is not None:
        manifest["whiten"] = _whitening_manifest(whitening)
    payload["__stanli"] = manifest
    return json.dumps(payload)


def _whitening_manifest(whitening):
    q_hat, perm, col_ptr, row_idx, values, lowrank = whitening

    def pack(a, dtype):
        raw = np.ascontiguousarray(a, dtype=dtype).tobytes()
        return base64.b64encode(raw).decode("ascii")

    out = {"q_hat": pack(q_hat, "<f8"), "perm": pack(perm, "<i8"),
           "col_ptr": pack(col_ptr, "<i8"), "row_idx": pack(row_idx, "<i8"),
           "values": pack(values, "<f8")}
    if lowrank is not None:
        c, b, w = lowrank
        out.update(k=int(b.shape[1]), c=pack(c, "<f8"), b=pack(b, "<f8"),
                   w=pack(w, "<f8"))
    return out


def bridgestan_model(stan_file=None, stan_code=None, mir=None, data=None,
                     name=None, include_paths=None, **kw):
    """A ``bridgestan.StanModel`` for this program, with nothing written.

    The payload is ``bridgestan_data(...)`` and the library is
    ``runtime_path()``.

    ``bridgestan`` is imported lazily and is not a stanli dependency.
    Extra keyword arguments pass through to ``bridgestan.StanModel``.
    ``include_paths`` has the same search order as ``Model``.
    """
    import bridgestan

    text = bridgestan_data(stan_file, stan_code, mir, data, name,
                           include_paths)
    # bridgestan warns that the library is "already loaded" -- it is, by
    # stanli's own import, and sharing it is this function's design, so
    # the warning is off unless the caller asks for it.
    kw.setdefault("warn", False)
    return bridgestan.StanModel(runtime_path(), text, **kw)


def build_id() -> str:
    """Identifies the runtime binary this package loaded.

    Source revision plus the build choices that change what that source
    produces. Key cached artifacts on it and refuse a mismatch.
    """
    return _lib.stanli_build_id().decode()


def _data_to_json(data) -> str:
    """Accept what callers naturally have: a dict, a path, or JSON text.

    numpy arrays are converted, since data almost always arrives as one.
    """
    if data is None:
        return "{}"
    if isinstance(data, pathlib.Path):
        return data.read_text()
    if isinstance(data, str):
        stripped = data.lstrip()
        if stripped.startswith("{"):
            return data
        return pathlib.Path(data).read_text()

    def encode(value):
        if hasattr(value, "tolist"):
            return value.tolist()
        raise TypeError(f"cannot serialise {type(value).__name__} as Stan data")

    return json.dumps(_stan_json_value(data), default=encode)


def _stan_json_value(value):
    """Tuples as CmdStan's JSON objects keyed "1", "2", ..., as CmdStanPy
    writes them, including inside lists and object arrays."""
    if isinstance(value, tuple):
        return {str(i): _stan_json_value(v) for i, v in enumerate(value, 1)}
    if isinstance(value, Mapping):
        return {k: _stan_json_value(v) for k, v in value.items()}
    if isinstance(value, list):
        return [_stan_json_value(v) for v in value]
    if isinstance(value, np.ndarray) and value.dtype == object:
        return _stan_json_value(value.tolist())
    return value


def exact_lp() -> bool:
    """Always True: every build reproduces CmdStan's lp__ exactly."""
    return bool(_lib.stanli_exact_lp())


def thread_safe() -> bool:
    """True if this build can run chains in parallel threads.

    stan-math's autodiff stack is a plain static unless STAN_THREADS is
    defined at build time, and the legacy kernels build nested var tapes
    on it. Where this is False, ``parallel_chains`` is clamped to 1 rather
    than honoured -- a slow answer instead of a wrong one.
    """
    return bool(_lib.stanli_thread_safe())


def _fmt(v, prec=4):
    if v is None or (isinstance(v, float) and math.isnan(v)):
        return "nan"
    if abs(v) >= 1e5 or (v != 0 and abs(v) < 1e-3):
        return f"{v:.{prec}g}"
    return f"{v:.{prec}f}"


def _print_sample_progress(chain_id, iteration, total, warmup,
                           n_divergent=0, n_max_treedepth=0, max_depth=10):
    """One CmdStan-shaped progress line, through Python's live stdout."""
    width = max(1, len(str(total)))
    percent = 100 if total == 0 else 100 * iteration // total
    phase = "Warmup" if warmup else "Sampling"
    problems = []
    if n_divergent:
        problems.append(f"{n_divergent} divergent")
    if n_max_treedepth:
        problems.append(f"{n_max_treedepth} at max treedepth ({max_depth})")
    suffix = "  " + ", ".join(problems) if problems else ""
    print(f"Chain [{chain_id}] Iteration: {iteration:{width}d} / {total} "
          f"[{percent:3d}%]  ({phase}){suffix}", flush=True)


def _sampling_warnings(opts, reports, stats):
    """The runtime's warnings for a finished run, "" when there are none."""
    need = _lib.stanli_sampling_warnings(ctypes.byref(opts), reports,
                                         _dptr(stats), None, 0)
    buf = ctypes.create_string_buffer(max(int(need), 1))
    _lib.stanli_sampling_warnings(ctypes.byref(opts), reports, _dptr(stats),
                                  buf, len(buf))
    return buf.value.decode()


def _print_sample_reports(reports, first_chain, warnings):
    """Per-chain timing, then the sampler warnings the runtime wrote."""
    print()
    for c, report in enumerate(reports):
        label = f"Chain [{first_chain + c}] Elapsed Time: "
        indent = " " * len(label)
        total = report.warmup_seconds + report.sampling_seconds
        print(f"{label}{report.warmup_seconds:.3f} seconds (Warm-up)")
        print(f"{indent}{report.sampling_seconds:.3f} seconds (Sampling)")
        print(f"{indent}{total:.3f} seconds (Total)")
        print()
    if warnings:
        print(warnings)


class Summary:
    """Per-parameter posterior summary: the columns stansummary prints.

    R-hat is rank-normalized split-R-hat and ESS is the bulk/tail pair,
    both Vehtari et al. 2021, computed by stan's own estimators. A
    constant column (a fixed transformed parameter) reports NaN for both
    rather than a passing number.

    Each statistic is also an attribute holding the whole column, so
    ``summary.r_hat.max()`` works without indexing by name.
    """

    def __init__(self, names, stats):
        self.names = list(names)
        self.stats = stats  # (n_cols, len(_SUMMARY_STATS))
        for i, key in enumerate(_SUMMARY_STATS):
            setattr(self, key, stats[:, i])

    def __len__(self):
        return len(self.names)

    def __getitem__(self, name):
        """The row for one parameter, as a dict. Dot names are also accepted."""
        if name not in self.names:
            name = _bracket(name)
        i = self.names.index(name)
        return {k: self.stats[i, j] for j, k in enumerate(_SUMMARY_STATS)}

    def to_pandas(self):
        """A DataFrame, for callers that have pandas. Not a dependency."""
        import pandas as pd
        return pd.DataFrame(self.stats, index=self.names,
                            columns=list(_SUMMARY_STATS))

    def __str__(self):
        w = max([len("name")] + [len(n) for n in self.names])
        head = ["Mean", "MCSE", "StdDev", "5%", "50%", "95%", "ESS_bulk",
                "ESS_tail", "R_hat"]
        out = ["name".ljust(w) + "".join(h.rjust(11) for h in head)]
        for i, n in enumerate(self.names):
            r = self.stats[i]
            cells = [_fmt(r[0]), _fmt(r[1]), _fmt(r[2]), _fmt(r[4]),
                     _fmt(r[5]), _fmt(r[6]), _fmt(r[7], 0), _fmt(r[8], 0),
                     _fmt(r[9], 3)]
            out.append(n.ljust(w) + "".join(c.rjust(11) for c in cells))
        return "\n".join(out)

    __repr__ = __str__


class Fit:
    """Draws from one sampling run, plus the sampler's own diagnostics.

    Indexing by name gives every draw with the chains concatenated, which
    is what an estimate wants and what ``sample()`` returned before it grew
    chains::

        fit["mu"].mean()
        fit["theta"]        # (chains*draws, 8), the declared shape
        fit["theta[1]"]     # (chains*draws,), one column

    ``fit.draws("mu")`` keeps the chain axis -- shape (chains, draws) --
    which is what a trace plot or a per-chain check wants.

    ``warmup_seconds`` and ``sampling_seconds`` hold each chain's times, and
    ``threads_per_chain`` the setting the run used.
    """

    def __init__(self, names, draws, sampler_stats, max_depth, seed,
                 reports=None, metric="diag_e", step_size=None,
                 inv_metric=None, thin=1, delta=0.8, resume=None,
                 threads_per_chain=1, variables=None):
        self.names = list(names)
        self._columns = {n: i for i, n in enumerate(self.names)}
        self._variables = (_scalar_variables(self.names) if variables is None
                           else variables)
        self._draws = draws               # (chains, draws, cols)
        self.sampler_stats = sampler_stats  # (chains, draws, 7)
        self.max_depth = max_depth
        self.seed = seed
        self.metric = metric
        self.step_size = (np.full(draws.shape[0], np.nan) if step_size is None
                          else step_size)
        self.inv_metric = ([None] * draws.shape[0] if inv_metric is None
                           else inv_metric)
        self.thin = thin
        self.delta = delta
        self.threads_per_chain = threads_per_chain
        # Per-chain resume records (bytes) for sample(continue_from=).
        self._resume = resume
        self._divergence_counts = None
        self._max_treedepth_counts = None
        self.warmup_seconds = np.full(draws.shape[0], np.nan)
        self.sampling_seconds = np.full(draws.shape[0], np.nan)
        if reports is not None:
            self.warmup_seconds = np.array([r.warmup_seconds for r in reports])
            self.sampling_seconds = np.array(
                [r.sampling_seconds for r in reports])
            self._divergence_counts = np.array(
                [r.n_divergent for r in reports], dtype=np.int64)
            self._max_treedepth_counts = np.array(
                [r.n_max_treedepth for r in reports], dtype=np.int64)

    @property
    def n_chains(self):
        return self._draws.shape[0]

    @property
    def n_draws(self):
        return self._draws.shape[1]

    @property
    def variables(self):
        """{name: dims} for every declared variable, in declaration order."""
        return {name: dims for name, (dims, idx) in self._variables.items()}

    def draws(self, name=None):
        """(chains, draws, *dims) for one variable, or all columns if None.

        A scalar variable, or a flat or dot column name, keeps the plain
        (chains, draws) shape.
        """
        if name is None:
            return self._draws
        dims, idx = _resolve(name, self._variables, self._columns)
        if not isinstance(idx, np.ndarray):
            flat = self._draws.reshape(-1, self._draws.shape[2])
            return _tuple_values(idx, flat).reshape(
                self._draws.shape[:2] + dims)
        return self._draws[:, :, idx]

    def __getitem__(self, name):
        """Every draw of one variable or column, chains concatenated.

        A variable name keeps its declared shape; a flat or dot name gives
        one column. Sampler columns (lp__, divergent__, ...) are reachable
        by name here too, so `fit["lp__"]` works the way a CSV reader would
        expect even though they are not posterior columns.
        """
        if name in SAMPLER_COLUMNS:
            col = SAMPLER_COLUMNS.index(name)
            return self.sampler_stats[:, :, col].reshape(-1)
        dims, idx = _resolve(name, self._variables, self._columns)
        if not isinstance(idx, np.ndarray):
            return _tuple_values(
                idx, self._draws.reshape(-1, self._draws.shape[2]))
        return self._draws[:, :, idx].reshape(-1, *dims)

    def stan_variable(self, name):
        """One declared variable's draws, chains concatenated, as
        CmdStanPy's ``stan_variable`` returns them: a tuple variable is an
        object array of Python tuples."""
        if name not in self._variables:
            raise ValueError(f"{name!r} is not a variable of this fit")
        return self[name]

    def stan_variables(self):
        """{name: stan_variable(name)} for every declared variable."""
        return {name: self[name] for name in self._variables}

    # The dict protocol the pre-chains API returned, so `fit["mu"]` and
    # `for k, v in fit.items()` keep working unchanged.
    def keys(self):
        return list(self.names)

    def items(self):
        return [(n, self[n]) for n in self.names]

    def values(self):
        return [self[n] for n in self.names]

    def __iter__(self):
        return iter(self.names)

    def __len__(self):
        return len(self.names)

    def __contains__(self, name):
        try:
            _resolve(name, self._variables, self._columns)
        except ValueError:
            return False
        return True

    @property
    def divergences(self):
        """Divergent transitions per chain."""
        if self._divergence_counts is not None:
            return self._divergence_counts.copy()
        col = SAMPLER_COLUMNS.index("divergent__")
        return self.sampler_stats[:, :, col].sum(axis=1).astype(np.int64)

    @property
    def max_treedepth_hits(self):
        """Transitions that saturated max_depth, per chain."""
        if self._max_treedepth_counts is not None:
            return self._max_treedepth_counts.copy()
        col = SAMPLER_COLUMNS.index("treedepth__")
        return (self.sampler_stats[:, :, col] >= self.max_depth) \
            .sum(axis=1).astype(np.int64)

    @property
    def stepsize(self):
        """Adapted stepsize per chain (constant after warmup)."""
        col = SAMPLER_COLUMNS.index("stepsize__")
        return self.sampler_stats[:, -1, col].copy()

    def ebfmi(self):
        """E-BFMI per chain (Betancourt); below 0.3 is a problem.

        The ratio of the mean square of successive energy differences to
        the marginal energy variance. Low values mean momentum resampling
        is not moving the chain across the energy distribution, so the
        tails go unexplored -- a failure R-hat and ESS are both blind to.
        """
        col = SAMPLER_COLUMNS.index("energy__")
        e = self.sampler_stats[:, :, col]
        num = np.square(np.diff(e, axis=1)).sum(axis=1)
        den = np.square(e - e.mean(axis=1, keepdims=True)).sum(axis=1)
        with np.errstate(invalid="ignore", divide="ignore"):
            return np.where(den > 0, num / den, np.nan)

    def summary(self, params=None):
        """Per-parameter summary. `params` selects columns by name.

        Each entry in `params` may be a variable, flat, or dot name; a
        variable name expands to all of its columns in column order.
        """
        if params is None:
            cols = list(range(len(self.names)))
        else:
            cols = []
            for p in params:
                _, idx = _resolve(p, self._variables, self._columns)
                cols.extend(_variable_indices(idx))
        sub = np.ascontiguousarray(self._draws[:, :, cols], dtype=np.float64)
        n_chains, n_draws, n_cols = sub.shape
        out = np.empty((n_cols, _N_SUMMARY_STATS))
        if _lib.stanli_summary_stats(_dptr(sub), n_chains, n_draws, n_cols,
                                     _dptr(out)) != 0:
            raise RuntimeError("summary failed")
        return Summary([self.names[i] for i in cols], out)

    def diagnose(self):
        """Convergence checks as text: what passed, what failed, what to do.

        Covers divergences, treedepth saturation, E-BFMI, R-hat and
        bulk/tail ESS -- the set Betancourt's workflow checks, at the
        thresholds the rank-normalized estimators call for.
        """
        d = np.ascontiguousarray(self._draws, dtype=np.float64)
        st = np.ascontiguousarray(self.sampler_stats, dtype=np.float64)
        n_chains, n_draws, n_cols = d.shape
        arr = (ctypes.c_char_p * n_cols)(*[n.encode() for n in self.names])
        # Two calls: the first sizes the text, the second fills it. The C
        # side returns the length it needs either way.
        need = _lib.stanli_diagnose_text(_dptr(d), n_chains, n_draws, n_cols,
                                         arr, _dptr(st), self.max_depth,
                                         None, 0)
        buf = ctypes.create_string_buffer(max(int(need), 1))
        _lib.stanli_diagnose_text(_dptr(d), n_chains, n_draws, n_cols, arr,
                                  _dptr(st), self.max_depth, buf, len(buf))
        return buf.value.decode()

    def to_arviz(self):
        """An arviz InferenceData with the sampler stats attached.

        arviz is not a dependency; this raises ImportError without it.
        """
        import arviz as az
        posterior = {}
        for var, (dims, idx) in self._variables.items():
            if isinstance(idx, np.ndarray):
                posterior[var] = self._draws[:, :, idx]
            else:
                for name, leaf in _tuple_leaves(var, idx):
                    posterior[name] = self._draws[:, :, leaf]
        stats = {n.rstrip("_"): self.sampler_stats[:, :, i]
                 for i, n in enumerate(SAMPLER_COLUMNS)}
        return az.from_dict(posterior=posterior, sample_stats=stats)

    def __repr__(self):
        return (f"<stanli.Fit {self.n_chains} chains x {self.n_draws} draws, "
                f"{len(self.names)} columns>")


class OptimizeResult(dict):
    """The mode, by column name, plus where it is on the sampler's scale.

    A dict so `result["mu"]` reads the way a draw does, with the
    unconstrained point and lp attached for handing to `sample(inits=)`.
    A variable name instead returns an array with its declared shape, e.g.
    `result["theta"]` for a `vector[8]`; flat and dot names still give a
    single float.
    """
    unconstrained = None
    lp = None

    def __init__(self, data, variables=None):
        super().__init__(data)
        self._names = list(data)
        self._columns = {n: i for i, n in enumerate(self._names)}
        self._variables = (_scalar_variables(self._names) if variables is None
                           else variables)

    @property
    def variables(self):
        """{name: dims} for every declared variable, in declaration order."""
        return {name: dims for name, (dims, idx) in self._variables.items()}

    def __missing__(self, key):
        try:
            dims, idx = _resolve(key, self._variables, self._columns)
        except ValueError:
            raise KeyError(key) from None
        if not isinstance(idx, np.ndarray):
            values = np.array([dict.__getitem__(self, n) for n in self._names])
            return _tuple_values(idx, values[None, :])[0]
        if dims == ():
            return self[self._names[int(idx)]]
        cols = idx.flatten("F")
        return np.array([self[self._names[int(c)]] for c in cols]) \
            .reshape(dims)

    def __contains__(self, key):
        if super().__contains__(key):
            return True
        try:
            _resolve(key, self._variables, self._columns)
        except ValueError:
            return False
        return True


_C_INT_MAX = 2 ** (8 * ctypes.sizeof(ctypes.c_int) - 1) - 1


def _whole_number(value):
    if isinstance(value, (bool, np.bool_)):
        return None
    try:
        return operator.index(value)
    except TypeError:
        pass
    if (isinstance(value, numbers.Real) and math.isfinite(value)
            and value == int(value)):
        return int(value)
    return None


def _int_arg(name, value):
    """A C int for the sampler; the runtime judges the range of values."""
    n = _whole_number(value)
    if n is None:
        raise ValueError(f"{name} must be an integer")
    if n > _C_INT_MAX:
        raise ValueError(f"{name} must be an integer no larger than {_C_INT_MAX}")
    return max(n, -_C_INT_MAX)


def _seed_arg(seed):
    n = _whole_number(seed)
    if n is None or n < 0:
        raise ValueError("seed must be an integer >= 0")
    if n > 2 ** 32 - 1:
        raise ValueError(f"seed must be at most {2 ** 32 - 1}")
    return n


def _real_arg(name, value):
    if (isinstance(value, (bool, np.bool_))
            or not isinstance(value, numbers.Real)):
        raise ValueError(f"{name} must be a number")
    return float(value)


def _reject_named_start(name, value):
    rows = value if isinstance(value, (list, tuple)) else [value]
    if any(isinstance(v, (Mapping, str)) for v in rows):
        raise TypeError(
            f"{name} must be the start as a vector on the unconstrained "
            f"scale (or one row per chain), not named values. To start from "
            f"named constrained values, convert them first: "
            f"{name}=model.unconstrain({{'theta': ...}})")


def _check_step_size(step_size):
    if step_size is None:
        return 1.0
    ok = (isinstance(step_size, numbers.Real)
          and not isinstance(step_size, (bool, np.bool_)))
    if not ok or not math.isfinite(step_size) or step_size <= 0:
        raise ValueError(
            f"step_size must be a single positive finite number, "
            f"got {step_size!r}")
    return float(step_size)


def _inv_metric_buffer(inv_metric, metric, chains, n):
    if metric == "unit_e":
        raise ValueError("inv_metric cannot be used with metric='unit_e'")
    one = (n,) if metric == "diag_e" else (n, n)
    try:
        arr = np.asarray(inv_metric)
        if arr.dtype == object:
            raise TypeError
        arr = arr.astype(np.float64)
    except (TypeError, ValueError):
        raise ValueError(
            f"inv_metric for {metric} must be numeric with shape {one} or "
            f"{(chains,) + one}") from None
    if arr.shape == one:
        arr = np.broadcast_to(arr, (chains,) + one)
    elif arr.shape != (chains,) + one:
        raise ValueError(
            f"inv_metric for {metric} must have shape {one} or "
            f"{(chains,) + one}, got {arr.shape}")
    return np.ascontiguousarray(arr)


# What sample(continue_from=) takes from the fit and refuses from the caller,
# with the defaults that mean "not passed".
_CONTINUE_FIXED = {
    "chains": 4, "seed": 1, "warmup": 1000, "delta": 0.8, "max_depth": 10,
    "thin": 1, "save_warmup": False, "inits": None, "init_radius": 2.0,
    "pathfinder_init": None, "metric": "diag_e", "inv_metric": None,
    "step_size": None, "adapt_engaged": True, "adapt_metric": True,
}


def _continue_settings(fit, given):
    if not isinstance(fit, Fit):
        raise TypeError(
            f"continue_from must be a Fit from Model.sample(), got "
            f"{type(fit).__name__}")
    passed = [name for name, default in _CONTINUE_FIXED.items()
              if (given[name] is not None if default is None
                  else given[name] is not default and given[name] != default)]
    if passed:
        raise ValueError(
            "continue_from continues the fit's chains with the fit's "
            "settings, so only samples, refresh, parallel_chains and "
            "threads_per_chain can be given; got " + ", ".join(passed))
    records = getattr(fit, "_resume", None)
    if not records or len(records) != fit.n_chains:
        raise ValueError(
            "this fit has no resume record to continue from: continue_from "
            "needs a NUTS fit from Model.sample(), and fits made by earlier "
            "stanli versions carry none")
    return records


def _continued_fit(old, new):
    """`old` with `new`'s draws appended per chain."""
    fit = Fit(old.names, np.concatenate([old._draws, new._draws], axis=1),
              np.concatenate([old.sampler_stats, new.sampler_stats], axis=1),
              old.max_depth, old.seed, metric=old.metric,
              step_size=old.step_size.copy(),
              inv_metric=[None if a is None else a.copy()
                          for a in old.inv_metric],
              thin=old.thin, delta=old.delta, resume=new._resume,
              threads_per_chain=new.threads_per_chain,
              variables=old._variables)
    fit._divergence_counts = old.divergences + new.divergences
    fit._max_treedepth_counts = (old.max_treedepth_hits
                                 + new.max_treedepth_hits)
    fit.warmup_seconds = old.warmup_seconds.copy()
    fit.sampling_seconds = old.sampling_seconds + new.sampling_seconds
    return fit


def _threads_per_chain(value):
    if isinstance(value, (bool, np.bool_)):
        raise TypeError("threads_per_chain must be a positive integer")
    try:
        value = operator.index(value)
    except TypeError:
        raise TypeError("threads_per_chain must be a positive integer") from None
    if value < 1:
        raise ValueError("threads_per_chain must be positive")
    if value > 2 ** (8 * ctypes.sizeof(ctypes.c_int) - 1) - 1:
        raise OverflowError("threads_per_chain does not fit in a C int")
    return value


class Model:
    """A compiled (model, data) pair.

    ``seed`` is the model-construction seed: RNG calls in transformed data
    draw from it once, here, the way CmdStan's generated constructor does,
    so the same seed reproduces the same transformed data. ``sample`` and
    ``optimize`` forward their own seed here when transformed data drew
    from it, rebuilding the model for that run, so one seed governs a whole
    run as it does in CmdStan. The rebuild replaces this object's model:
    afterwards ``log_prob_grad``, ``n_unconstrained`` and
    ``constrained_names`` all describe the model as built under the run
    seed.

    ``threads_per_chain`` is the number of threads each chain can use for
    eligible ``reduce_sum`` calls. The model is compiled for this count, and
    sampling, optimization and direct gradient calls all use it. To use
    another count, build a new Model. Small or unsupported ``reduce_sum``
    calls stay serial. ``reduce_sum_count`` is the number that run in
    parallel, and ``reduce_sum_fallbacks`` gives the reasons that lowering
    refused others. A different count
    splits the sum differently, which can change floating-point rounding and
    so the NUTS draws.

    ``include_paths`` accepts a directory or an iterable of directories for
    Stan ``#include`` files. These are searched in order, then the directory
    containing ``stan_file`` (or the current directory for ``stan_code``).
    Nested includes use the same search path. Included source is compiled
    once and retained for later sampling, even if its files change or move.

    ``fast_math=True`` opts this model into fast mode: source is compiled
    with fused multiply-add rewriting and the runtime may use faster,
    less exactly rounded evaluation. Results can differ from the default in
    the last bits. Pass the same ``fast_math`` with ``mir=`` that was used
    to produce it with ``stan_to_mir``.

    ``allow_interpreter=False`` makes construction raise, naming the parts,
    when parts of the model have no compiled path and would run through the
    much slower MIR interpreter.
    """

    def __init__(self, stan_file=None, data=None, stan_code=None, mir=None,
                 seed=1, threads_per_chain=1, include_paths=None,
                 fast_math=False, allow_interpreter=True,
                 _parallel_write_array="auto"):
        threads_per_chain = _threads_per_chain(threads_per_chain)
        if _parallel_write_array not in _PARALLEL_WRITE_ARRAY:
            raise ValueError("_parallel_write_array must be one of "
                             + ", ".join(_PARALLEL_WRITE_ARRAY))
        self._parallel_write_array = _PARALLEL_WRITE_ARRAY[_parallel_write_array]
        seed = _seed_arg(seed)
        self._fast_math = _check_fast_math(fast_math)
        if not isinstance(allow_interpreter, bool):
            raise TypeError("allow_interpreter must be True or False")
        self._allow_interpreter = allow_interpreter
        self._label = None
        if mir is None:
            self._label = _source_label(stan_file, stan_code)
            stan_code, paths = _read_stan_source(stan_file, stan_code, include_paths)
            if "#include" in stan_code or not _lib.stanli_has_embedded_stanc():
                mir = _compile_stan(stan_code, paths, self._fast_math,
                                    self._label)
        self._source = (mir, stan_code, _data_to_json(data))
        self._whitening = None
        self._m = None
        self._adopt(self._construct(seed, threads_per_chain), seed, threads_per_chain)
        _warn_compiler_messages(
            (_lib.stanli_model_compiler_message_kind(self._m, i),
             _lib.stanli_model_compiler_message(self._m, i).decode())
            for i in range(_lib.stanli_model_n_compiler_messages(self._m)))
        note = _lib.stanli_warnings(self._m)
        if note:
            warnings.warn(note.decode(), RuntimeWarning, stacklevel=2)

    def _adopt(self, m, seed, threads_per_chain):
        # Everything this object caches about its model is derived from the
        # handle, so a rebuild under another seed must refresh all of it:
        # transformed data can size a parameter (`int k = poisson_rng(3);`
        # then `vector[k] mu;`), which changes the free vector length and
        # the column names along with the draws.
        if self._m:
            _lib.stanli_model_free(self._m)
        self._m, self._seed = m, seed
        self._threads_per_chain = threads_per_chain
        self.n_unconstrained = _lib.stanli_n_unconstrained(m)
        self.constrained_names = _variable_columns(
            self._column_shapes(False))

    def _construct(self, seed, threads_per_chain):
        mir, stan_code, data_json = self._source
        err = ctypes.create_string_buffer(8192)
        opts = _ModelOpts(seed, threads_per_chain, int(self._fast_math),
                          int(self._allow_interpreter), 0,
                          self._parallel_write_array)
        with _native_output():
            if mir is not None:
                m = _lib.stanli_model_new_with_opts(
                    mir.encode(), data_json.encode(), ctypes.byref(opts), err,
                    len(err))
            else:
                m = _lib.stanli_model_new_from_stan_named(
                    stan_code.encode(),
                    None if self._label is None else self._label.encode(),
                    data_json.encode(), ctypes.byref(opts), err, len(err))
        if not m:
            raise RuntimeError(err.value.decode())
        if self._whitening is not None:
            whitened = self._apply_whitening(m)
            _lib.stanli_model_free(m)
            m = whitened
        return m

    def _apply_whitening(self, inner):
        q_hat, perm, col_ptr, row_idx, values, lowrank = self._whitening
        err = ctypes.create_string_buffer(4096)
        if lowrank is None:
            c = b = w = None
            k = 0
        else:
            c, b, w = lowrank
            k = b.shape[1]
            c, b, w = (_dptr(c), _dptr(b), _dptr(w))
        m = _lib.stanli_model_whiten_lowrank(
            inner,
            q_hat.ctypes.data_as(ctypes.POINTER(ctypes.c_double)),
            q_hat.size,
            perm.ctypes.data_as(ctypes.POINTER(ctypes.c_int64)),
            col_ptr.ctypes.data_as(ctypes.POINTER(ctypes.c_int64)),
            row_idx.ctypes.data_as(ctypes.POINTER(ctypes.c_int64)),
            values.ctypes.data_as(ctypes.POINTER(ctypes.c_double)),
            values.size, k, c, b, w, err, len(err))
        if not m:
            raise ValueError(err.value.decode())
        return m

    def bridgestan_data(self, name=None):
        """The data argument that makes the runtime serve this model to BridgeStan.

        Like the module-level ``bridgestan_data``, for the program and data
        this model was built from. On a model returned by ``whiten`` the
        manifest also carries the whitening (``q_hat``, the permutation, L
        and any low-rank factors, as exact little-endian bytes), so a
        BridgeStan client sees the same free vector z and the same log
        density, gradient and constrain/unconstrain maps as this object.
        Unconstrained parameter names are ``z.1``, ``z.2``, ... for a
        whitened model.

        The log density is the one the runtime computes by default;
        ``fast_math`` is a runtime option BridgeStan has no way to pass, so
        a model built with it is refused.
        """
        if self._fast_math:
            raise ValueError("a fast_math model has no BridgeStan route: "
                             "the option is not part of the manifest")
        mir, stan_code, data_json = self._source
        if mir is None:
            mir = _compile_stan(stan_code, [], False, self._label)
        if name is None:
            name = (pathlib.Path(self._label).stem if self._label
                    else "stanli_model")
        return _bridgestan_payload(mir, name, data_json, self._whitening)

    def bridgestan_model(self, name=None, **kw):
        """A ``bridgestan.StanModel`` over ``bridgestan_data(name)``."""
        import bridgestan

        kw.setdefault("warn", False)
        return bridgestan.StanModel(runtime_path(), self.bridgestan_data(name),
                                    **kw)

    def whiten(self, q_hat, perm, L, lowrank=None):
        """EXPERIMENTAL. A new Model over whitened coordinates z.

        With q this model's unconstrained vector, the new model's free
        vector z satisfies ``q = q_hat + P.T @ inv(L.T) @ z`` where
        ``(P v)[i] = v[perm[i]]`` and ``L`` is a lower triangular Cholesky
        factor with ``P Q P.T = L L.T``; Q is typically the negative Hessian
        of the log density at ``q_hat``, so z is close to standard normal.
        The log density is this model's at q(z) with the constant
        ``log|det L|`` dropped. Every algorithm (sample, optimize, pathfinder,
        laplace, log_prob_grad, log_prob_hessian) runs on z unchanged, and
        sampler settings such as ``metric="unit_e"`` apply to z.

        Columns, constrained names and output rows are this model's own, at
        q(z). ``unconstrain`` returns z for given constrained values.
        Unconstrained inits, modes and Hessians passed in or read back are in
        z. The result does not depend on this model afterwards.

        ``L`` is a scipy.sparse matrix (any format; converted to CSC) or a
        ``(indptr, indices, data)`` tuple of CSC arrays. It must be lower
        triangular with a nonzero diagonal. scipy is not imported by this
        package; pass the tuple form to avoid needing it.

        ``lowrank=(c, B, W)`` inserts the factor ``C = diag(c) +
        tril(W B.T, -1)`` (``B`` and ``W`` are n by k): ``q = q_hat + P.T @
        inv(L.T) @ inv(C.T) @ z``. With ``L`` unit lower triangular and ``C
        C.T = D + W W.T`` the whitened precision is ``P.T L (D + W W.T) L.T
        P``. The per-gradient cost is O(nnz(L) + n k).
        """
        _experimental("Laplace whitening", _LAPLACE_WHITENING_ADVICE)
        n = self.n_unconstrained
        q_hat = np.ascontiguousarray(q_hat, dtype=np.float64)
        if q_hat.shape != (n,):
            raise ValueError(f"q_hat has shape {q_hat.shape}, the model has "
                             f"{n} unconstrained parameters")
        perm = np.ascontiguousarray(perm, dtype=np.int64)
        if perm.shape != (n,):
            raise ValueError(f"perm has shape {perm.shape}, expected ({n},)")
        if hasattr(L, "tocsc"):
            if tuple(L.shape) != (n, n):
                raise ValueError(f"L has shape {tuple(L.shape)}, expected "
                                 f"({n}, {n})")
            L = L.tocsc().copy()
            L.sum_duplicates()
            L.sort_indices()
            parts = (L.indptr, L.indices, L.data)
        else:
            parts = tuple(L)
            if len(parts) != 3:
                raise ValueError("L must be a scipy.sparse matrix or an "
                                 "(indptr, indices, data) tuple")
        col_ptr = np.ascontiguousarray(parts[0], dtype=np.int64)
        row_idx = np.ascontiguousarray(parts[1], dtype=np.int64)
        values = np.ascontiguousarray(parts[2], dtype=np.float64)
        if col_ptr.shape != (n + 1,) or row_idx.shape != values.shape:
            raise ValueError(f"L is not an {n} by {n} CSC matrix")
        if lowrank is not None:
            c, B, W = (np.ascontiguousarray(a, dtype=np.float64)
                       for a in lowrank)
            if (c.shape != (n,) or B.ndim != 2 or B.shape[0] != n
                    or B.shape != W.shape or B.shape[1] < 1):
                raise ValueError(f"lowrank must be (c, B, W) with c of "
                                 f"shape ({n},) and B, W of shape ({n}, k)")
            lowrank = (c.copy(), B.copy(), W.copy())
        out = Model.__new__(Model)
        out._fast_math = self._fast_math
        out._label = self._label
        out._source = self._source
        out._whitening = (q_hat.copy(), perm.copy(), col_ptr.copy(),
                          row_idx.copy(), values.copy(), lowrank)
        out._m = None
        out._adopt(out._apply_whitening(self._handle), self._seed,
                   self._threads_per_chain)
        return out

    @property
    def _handle(self):
        m = getattr(self, "_m", None)
        if not m:
            raise RuntimeError("stanli model handle is no longer valid")
        return m

    def __reduce__(self):
        raise TypeError(
            "stanli models cannot be copied or pickled; build another with "
            "Model(...)")

    @property
    def fast_math(self):
        """Whether this model was built in fast mode."""
        return self._fast_math

    @property
    def threads_per_chain(self):
        """Number of threads per chain that the model was built for."""
        return self._threads_per_chain

    @property
    def reduce_sum_count(self):
        """Number of retained native parallel reductions in the prepared graph."""
        return _lib.stanli_reduce_sum_count(self._handle)

    @property
    def reduce_sum_fallbacks(self):
        """Graph-lowering refusal reasons; opaque runtime regions are not inventoried."""
        m = self._handle
        return [_lib.stanli_reduce_sum_fallback(m, i).decode()
                for i in range(_lib.stanli_n_reduce_sum_fallbacks(m))]

    def _prepare_run(self, seed, threads_per_chain=None):
        # threads_per_chain is given only by sample()'s deprecated argument.
        if threads_per_chain is None:
            threads_per_chain = self.threads_per_chain
        construction_seed = (seed if _lib.stanli_transformed_data_rng(self._handle)
                             else self._seed)
        if (construction_seed != self._seed or
                threads_per_chain != self.threads_per_chain):
            m = self._construct(construction_seed, threads_per_chain)
            self._adopt(m, construction_seed, threads_per_chain)

    def __del__(self):
        if getattr(self, "_m", None):
            _lib.stanli_model_free(self._m)
            self._m = None

    def log_prob_grad(self, q):
        """log density (jacobian included) and gradient at unconstrained q."""
        q = np.ascontiguousarray(q, dtype=np.float64)
        if q.size != self.n_unconstrained:
            raise ValueError(f"q has {q.size} elements, model has "
                             f"{self.n_unconstrained} unconstrained parameters")
        lp = ctypes.c_double()
        grad = np.empty(self.n_unconstrained)
        with _native_output():
            rc = _lib.stanli_grad(self._handle, _dptr(q), ctypes.byref(lp),
                                  _dptr(grad))
        if rc != 0:
            # The gradient buffer is uninitialized on failure; never let a
            # caller see it.
            raise RuntimeError("log density evaluation failed at this point "
                               "(domain error in a distribution or function)")
        return lp.value, grad

    def log_prob_hessian(self, q):
        """log density, gradient and Hessian at unconstrained q.

        The density is that of `log_prob_grad`. The Hessian has shape (n, n)
        and is symmetric: Stan's finite-difference Hessian of the gradient,
        which costs 1 + 2n gradient evaluations. It is the routine Stan's Laplace
        approximation uses, so the error is that of a finite difference.
        """
        q = np.ascontiguousarray(q, dtype=np.float64)
        n = self.n_unconstrained
        if q.size != n:
            raise ValueError(f"q has {q.size} elements, model has "
                             f"{n} unconstrained parameters")
        lp = ctypes.c_double()
        grad = np.empty(n)
        hess = np.empty((n, n))
        err = ctypes.create_string_buffer(4096)
        with _native_output():
            rc = _lib.stanli_hessian(self._handle, _dptr(q), ctypes.byref(lp),
                                     _dptr(grad), _dptr(hess), err, len(err))
        if rc != 0:
            raise RuntimeError("Hessian evaluation failed at this point: "
                               + err.value.decode(errors="replace"))
        return lp.value, grad, hess

    def unconstrain(self, values):
        """Starting values on the CONSTRAINED scale, as the free vector.

        `values` is a dict of parameter name to value, or a JSON string in
        CmdStan's data format. Every declared parameter must appear, at its
        declared size; a missing, unknown, wrong-length, or out-of-support
        value raises naming the parameter.

        Each value has its declared shape, as CmdStan requires of a starting
        value: a numpy array of that shape, or lists nested first index
        outermost. A flat list for a matrix, or a transposed one, raises.
        A zero-size parameter may be left out.

        The result is what `sample(inits=...)`, `optimize(init=...)`, and
        `log_prob_grad` all take, so unconstraining is a step you do once per
        starting point rather than a second kind of argument on every call.
        """
        # The same shapes data accepts: a dict, a path, or JSON text.
        document = _data_to_json(values).encode()
        q = np.empty(self.n_unconstrained)
        err = ctypes.create_string_buffer(4096)
        with _native_output():
            rc = _lib.stanli_unconstrain_inits(self._handle, document,
                                               _dptr(q), err, len(err))
        if rc != 0:
            raise ValueError(err.value.decode())
        return q

    def _unconstrained_layout(self):
        """[(parameter name, unconstrained entries)] in vector order.

        Empty for a whitened model, whose vector is not in the declared
        parameters' terms.
        """
        return [(_lib.stanli_param_block_name(self._handle, i).decode(),
                 _lib.stanli_param_block_size(self._handle, i))
                for i in range(_lib.stanli_n_param_blocks(self._handle))]

    def _column_names(self):
        """The CSV columns for a draw, and whether write_array supplies them.

        Every column CmdStan would write when the model has a generated
        quantities section; the constrained parameters otherwise.
        """
        if _lib.stanli_wa_n_columns(self._handle) > 0:
            return _variable_columns(self._column_shapes(True)), True
        return list(self.constrained_names), False

    def _column_variables(self, write_array):
        """The declared variables behind _column_names, from the runtime."""
        return _variables_from_shapes(self._column_shapes(write_array))

    def _column_shapes(self, write_array):
        """The runtime's (CmdStan name, dims) behind _column_names."""
        m = self._m
        which = _WRITE_ARRAY_COLUMNS if write_array else _CONSTRAINED_COLUMNS
        shapes = []
        for i in range(_lib.stanli_n_variables(m, which)):
            dims = tuple(_lib.stanli_variable_dim(m, which, i, k) for k in
                         range(_lib.stanli_variable_n_dims(m, which, i)))
            shapes.append((_lib.stanli_variable_name(m, which, i).decode(),
                           dims))
        return shapes

    def parameter_annotations(self):
        """The declaration annotations on each parameter.

        A {name: [annotation, ...]} of the parameters that carry at least
        one, in source order and without the `@`: `@latent vector[J] a;`
        gives {"a": ["latent"]}. Transformed parameters and generated
        quantities are not listed; an annotation on one has no effect.
        """
        m = self._m
        found = {}
        for i in range(_lib.stanli_n_variables(m, _CONSTRAINED_COLUMNS)):
            count = _lib.stanli_variable_n_annotations(
                m, _CONSTRAINED_COLUMNS, i)
            if count:
                name = _lib.stanli_variable_name(
                    m, _CONSTRAINED_COLUMNS, i).decode()
                found[name] = [
                    _lib.stanli_variable_annotation(
                        m, _CONSTRAINED_COLUMNS, i, k).decode()
                    for k in range(count)]
        return found

    def optimize(self, *, seed=1, iter=2000, jacobian=True, init=None,
                 init_radius=2.0):
        """The mode by L-BFGS -- the same optimizer CmdStan's `optimize` runs.

        Returns an OptimizeResult: a {name: float} of every CSV column at
        the mode, matching one draw of what `sample()` returns, with the
        unconstrained point and lp attached. `.unconstrained` is what
        `sample(inits=...)` takes.

        Returns the posterior MODE. CmdStan's `optimize` defaults to
        `jacobian=0`, the penalized maximum likelihood, which stanli
        cannot offer: the Jacobian terms are folded into the graph at
        lowering time. `jacobian=False` raises rather than quietly
        returning the other quantity -- the two differ for any
        constrained parameter, which is most models.
        """
        seed = _seed_arg(seed)
        iter = _int_arg("iter", iter)
        init_radius = _real_arg("init_radius", init_radius)
        if iter < 1:
            raise ValueError("iter must be an integer >= 1")
        if not (math.isfinite(init_radius) and init_radius >= 0):
            raise ValueError("init_radius must be a finite number >= 0")
        if init is not None:
            _reject_named_start("init", init)
        self._prepare_run(seed)
        if not jacobian:
            raise NotImplementedError(
                "stanli folds the Jacobian into the graph at lowering "
                "time, so the penalized maximum likelihood (CmdStan's "
                "jacobian=0) is not available; optimize() returns the "
                "posterior mode")
        opts = _OptimizeOpts()
        _lib.stanli_optimize_opts_init(ctypes.byref(opts))
        opts.seed = seed
        opts.iter = iter
        opts.jacobian = 1
        opts.init_radius = init_radius
        init_arr = None
        if init is not None:
            init_arr = np.ascontiguousarray(init, dtype=np.float64)
            if init_arr.shape != (self.n_unconstrained,):
                raise ValueError(
                    f"init must be ({self.n_unconstrained},) on the "
                    f"unconstrained scale, got {init_arr.shape}")
            opts.init = _dptr(init_arr)

        names, write_array = self._column_names()
        variables = self._column_variables(write_array)
        q = np.empty(self.n_unconstrained)
        vals = np.empty(len(names))
        lp = ctypes.c_double()
        err = ctypes.create_string_buffer(4096)
        with _native_output():
            rc = _lib.stanli_optimize(self._handle, ctypes.byref(opts),
                                      _dptr(q), _dptr(vals), ctypes.byref(lp),
                                      err, len(err))
        del init_arr
        if rc != 0:
            raise RuntimeError(f"optimize failed: {err.value.decode()}")
        out = OptimizeResult({n: vals[i] for i, n in enumerate(names)},
                             variables)
        out.unconstrained = q
        out.lp = lp.value
        return out

    def sample(self, *, chains=4, seed=1, warmup=1000, samples=1000,
               delta=0.8, max_depth=10, thin=1, save_warmup=False,
               inits=None, init_radius=2.0, pathfinder_init=None,
               parallel_chains=None, refresh=100, threads_per_chain=None,
               metric="diag_e", inv_metric=None, step_size=None,
               adapt_engaged=True, adapt_metric=True, continue_from=None):
        """NUTS draws as a Fit.

        Four chains by default, because R-hat needs more than one and a
        single-chain run cannot be checked for convergence at all. Chain c
        uses CmdStan's stream for (seed, chain id c+1), so a matched seed
        means a matched stream per chain.

        Generated quantities consume that live stream between saved draws,
        including saved warmup; thinned-away transitions do not run them.
        Numerical differences between model implementations can still change
        trajectories. A generated-quantity domain error fills its output row
        with NaN and sampling continues with the already advanced RNG.

        `inits` is on the UNCONSTRAINED scale: one vector shared by every
        chain, or one row per chain. Start from constrained values by
        passing them through `unconstrain()` first -- one scale here means
        one contract for what a start is. `init_radius=0` starts every
        chain at the origin, which is CmdStan's `init=0`.

        `pathfinder_init={}` instead fits one single-path Pathfinder
        approximation and draws one unconstrained start per chain from it.
        The mapping can override `num_iterations` (1000), `num_elbo_draws`
        (25), `history_size` (5), and Pathfinder's own `init_radius` (2).
        It uses this call's seed and is mutually exclusive with `inits`.
        Single-path Pathfinder does not perform PSIS resampling.

        `parallel_chains` defaults to running every chain at once, capped
        at the machine's cores. Threading changes nothing about the
        answer -- the draws are byte-identical to a sequential run,
        because each chain has its own executor and its own RNG stream --
        so it is on by default. A build without thread support clamps it
        to 1 (see ``thread_safe()``).

        Sampling uses the `threads_per_chain` that the model was built with.
        Each active chain can use this many threads, including its sampling
        thread, so a run uses up to `parallel_chains * threads_per_chain`
        threads. The `threads_per_chain` argument here is deprecated. When it
        differs from the model's count, sample() warns and rebuilds this model
        for the new count, and the model keeps that count afterwards. Build
        the model with `Model(..., threads_per_chain=N)` instead.

        `metric` is "diag_e" (the default), "dense_e" or "unit_e", as in
        CmdStan. `inv_metric` is the starting inverse metric: shape (n,) for
        "diag_e" or (n, n) for "dense_e" with n the number of unconstrained
        parameters, shared by every chain, or with a leading chain axis, or a
        list of one array per chain such as `fit.inv_metric`. None starts from
        the identity. "unit_e" takes none. `step_size` is the initial step
        size (default 1). `adapt_engaged=False` runs no adaptation, so the
        metric and step size stay as given. With warmup under 150 Stan does
        not adapt the metric and a supplied one is kept, as in CmdStan. With
        `adapt_engaged=True` and `warmup=0` Stan still runs its initial step
        size search, so use `adapt_engaged=False` to run at exactly `step_size`.
        `adapt_metric=False` with `adapt_engaged=True` adapts only the step
        size and keeps `inv_metric` (the identity if None) unchanged; it has
        no effect with "unit_e" or `adapt_engaged=False`.

        The Fit reports `metric`, `step_size` (one per chain) and
        `inv_metric` (a list with one array per chain, None for "unit_e"):
        what each chain holds after warmup. A pilot fit's `inv_metric` and
        mean `step_size` can start a later fit with `warmup=0` and
        `adapt_engaged=False`.

        `refresh` is the number of transitions between CmdStan-shaped
        progress updates. The default is 100; zero disables all automatic
        progress, timing, and sampler-problem output. Reporting only observes
        the run and does not change draws, sampler statistics, or RNG streams.

        `continue_from=fit` runs `samples` more transitions of each of the
        fit's chains, starting where they stopped with their own step size,
        inverse metric and random stream, and no warmup or adaptation. It
        returns a new Fit with the new draws and sampler statistics appended
        to the old ones per chain; `fit` itself is unchanged. The result is
        the run a single call with the summed `samples` would have made,
        bitwise, generated quantities included, when the model has the same
        `threads_per_chain` as the run that made the fit. Only `samples`,
        `refresh`, `parallel_chains` and the deprecated `threads_per_chain`
        can be given with it; the other settings come from the fit. The model
        and data must be the ones that made the fit.

        Columns are every column CmdStan's CSV would carry -- constrained
        parameters, transformed parameters, generated quantities, with RNG
        draws streamed per chain -- for models with a generate_quantities
        section, and the constrained parameters otherwise.
        """
        resume_in = None
        if continue_from is not None:
            resume_in = _continue_settings(continue_from, {
                "chains": chains, "seed": seed, "warmup": warmup,
                "delta": delta, "max_depth": max_depth, "thin": thin,
                "save_warmup": save_warmup, "inits": inits,
                "init_radius": init_radius, "pathfinder_init": pathfinder_init,
                "metric": metric, "inv_metric": inv_metric,
                "step_size": step_size, "adapt_engaged": adapt_engaged,
                "adapt_metric": adapt_metric})
            chains, seed = continue_from.n_chains, continue_from.seed
            warmup = 0
            thin, delta = continue_from.thin, continue_from.delta
            max_depth, metric = continue_from.max_depth, continue_from.metric
            if metric != "unit_e":
                inv_metric = continue_from.inv_metric
            adapt_engaged = False
        if threads_per_chain is None:
            threads_per_chain = self.threads_per_chain
        threads_per_chain = _threads_per_chain(threads_per_chain)
        if threads_per_chain != self.threads_per_chain:
            warnings.warn(
                f"passing threads_per_chain to sample() is deprecated. This "
                f"call rebuilds the model for {threads_per_chain} threads, "
                f"and the model keeps that count. Build the model with "
                f"Model(..., threads_per_chain={threads_per_chain}) instead.",
                DeprecationWarning, stacklevel=2)
        seed = _seed_arg(seed)
        if inits is not None:
            _reject_named_start("inits", inits)
        if metric not in _METRICS:
            raise ValueError(
                f"metric must be one of {', '.join(_METRICS)}, got {metric!r}")
        step_size_value = _check_step_size(step_size)
        if not isinstance(adapt_engaged, (bool, np.bool_)):
            raise TypeError("adapt_engaged must be a bool")
        if not isinstance(adapt_metric, (bool, np.bool_)):
            raise TypeError("adapt_metric must be a bool")
        if isinstance(refresh, (bool, np.bool_)):
            raise TypeError("refresh must be a nonnegative integer")
        try:
            refresh = operator.index(refresh)
        except TypeError:
            raise TypeError("refresh must be a nonnegative integer") from None
        if refresh < 0:
            raise ValueError("refresh must be nonnegative")
        int_max = 2 ** (8 * ctypes.sizeof(ctypes.c_int) - 1) - 1
        if refresh > int_max:
            raise OverflowError("refresh does not fit in a C int")

        opts = _SampleOpts()
        _lib.stanli_sample_opts_init(ctypes.byref(opts))
        opts.seed = seed
        opts.chains = _int_arg("chains", chains)
        opts.warmup = _int_arg("warmup", warmup)
        opts.samples = _int_arg("samples", samples)
        opts.thin = _int_arg("thin", thin)
        opts.delta = _real_arg("delta", delta)
        opts.max_depth = _int_arg("max_depth", max_depth)
        opts.save_warmup = 1 if save_warmup else 0
        opts.init_radius = _real_arg("init_radius", init_radius)
        if parallel_chains is None:
            parallel_chains = min(max(opts.chains, 1), os.cpu_count() or 1)
        parallel_chains = _int_arg("parallel_chains", parallel_chains)
        if parallel_chains < 1:
            raise ValueError("parallel_chains must be an integer >= 1")
        opts.num_threads = parallel_chains
        err = ctypes.create_string_buffer(4096)
        if _lib.stanli_sample_opts_check(ctypes.byref(opts), err, len(err)):
            raise ValueError(err.value.decode())
        self._prepare_run(seed, threads_per_chain)

        # Held in a local until sample_multi returns: opts.inits borrows
        # this buffer, so letting it fall out of scope would hand the
        # sampler freed memory.
        init_arr = None
        if inits is not None and pathfinder_init is not None:
            raise ValueError("inits and pathfinder_init are mutually exclusive")
        if pathfinder_init is not None:
            pf = _pathfinder_init_options(pathfinder_init)
            init_arr = np.empty((opts.chains, self.n_unconstrained))
            err = ctypes.create_string_buffer(4096)
            with _native_output():
                rc = _lib.stanli_pathfinder_inits(
                    self._handle, opts.seed, opts.chain_id, opts.chains,
                    pf["num_iterations"], pf["num_elbo_draws"],
                    pf["history_size"], pf["init_radius"], _dptr(init_arr),
                    None, None, err, len(err))
            if rc != 0:
                raise RuntimeError(
                    f"Pathfinder initialization failed: {err.value.decode()}")
            opts.inits = _dptr(init_arr)
        elif inits is not None:
            init_arr = np.ascontiguousarray(inits, dtype=np.float64)
            if init_arr.ndim == 1:
                init_arr = np.ascontiguousarray(
                    np.tile(init_arr, (opts.chains, 1)))
            if init_arr.shape != (opts.chains, self.n_unconstrained):
                raise ValueError(
                    f"inits must be ({opts.chains}, {self.n_unconstrained}) "
                    f"or ({self.n_unconstrained},) on the unconstrained "
                    f"scale, got {init_arr.shape}")
            opts.inits = _dptr(init_arr)

        metric_opts = _MetricOpts()
        _lib.stanli_metric_opts_init(ctypes.byref(metric_opts))
        metric_opts.metric = _METRICS.index(metric)
        metric_opts.step_size = step_size_value
        metric_opts.adapt_engaged = 1 if adapt_engaged else 0
        metric_opts.adapt_metric = 1 if adapt_metric else 0
        inv_metric_arr = None
        if inv_metric is not None:
            inv_metric_arr = _inv_metric_buffer(
                inv_metric, metric, opts.chains, self.n_unconstrained)
            metric_opts.inv_metric = _dptr(inv_metric_arr)
        adapted_step = np.full(opts.chains, np.nan)
        metric_opts.adapted_step_size = _dptr(adapted_step)
        adapted_inv = None
        if metric != "unit_e":
            width = self.n_unconstrained * (
                self.n_unconstrained if metric == "dense_e" else 1)
            adapted_inv = np.full((opts.chains, width), np.nan)
            metric_opts.adapted_inv_metric = _dptr(adapted_inv)

        # Locals until the end of the call: opts borrows them.
        resume_bufs = resume_ptrs = resume_lens = None
        if resume_in is not None:
            resume_bufs = [(ctypes.c_char * len(r)).from_buffer_copy(r)
                           for r in resume_in]
            resume_ptrs = (ctypes.c_void_p * opts.chains)(
                *[ctypes.addressof(b) for b in resume_bufs])
            resume_lens = (ctypes.c_size_t * opts.chains)(
                *[len(r) for r in resume_in])
            opts.resume_in = resume_ptrs
            opts.resume_in_len = resume_lens
        record_size = _lib.stanli_resume_size(self._handle)
        resume_out = (ctypes.c_char * (opts.chains * record_size))()
        opts.resume_out = ctypes.addressof(resume_out)

        n_stored = _lib.stanli_n_stored_draws(ctypes.byref(opts))
        raw = np.empty((opts.chains, n_stored, self.n_unconstrained))
        stats = np.empty((opts.chains, n_stored, _N_SAMPLER_COLS))
        names, write_array = self._column_names()
        variables = self._column_variables(write_array)
        out = np.empty((opts.chains, n_stored, len(names)))
        reports = (_SampleReport * opts.chains)()
        err = ctypes.create_string_buffer(4096)
        callback_errors = []
        # A typed null function pointer: ctypes does not accept Python None for
        # a CFUNCTYPE argtype, even though both represent a null C callback.
        progress_cb = _SampleProgressCallback()
        if refresh:
            def progress(chain_id, iteration, total, is_warmup, n_divergent,
                         n_max_treedepth, _user):
                if callback_errors:
                    return
                try:
                    _flush_messages()
                    _print_sample_progress(chain_id, iteration, total,
                                           is_warmup, n_divergent,
                                           n_max_treedepth, opts.max_depth)
                except BaseException as exc:  # ctypes cannot propagate it
                    callback_errors.append(exc)

            # Keep this object in a local until the native call returns. ctypes
            # callbacks are raw function pointers; collecting it early would
            # leave the sampler calling freed memory.
            progress_cb = _SampleProgressCallback(progress)

        interrupted = ctypes.c_int(0)
        watch = _InterruptWatch()
        with watch, _native_output():
            failed = _lib.stanli_sample_multi_metric(
                self._handle, ctypes.byref(opts), ctypes.byref(metric_opts),
                refresh, _dptr(raw), _dptr(stats), _dptr(out), progress_cb,
                None, watch.poll, None, ctypes.byref(interrupted), reports,
                err, len(err))
        del init_arr, inv_metric_arr
        if interrupted.value:
            watch.raise_interrupt()
        if failed < 0:
            raise RuntimeError(err.value.decode())
        if failed:
            raise RuntimeError(
                f"{failed} of {opts.chains} chains failed; first: "
                f"{err.value.decode()}")
        if callback_errors:
            raise callback_errors[0]
        if watch.flush_error is not None:
            raise watch.flush_error

        first_chain = opts.chain_id if opts.chain_id > 0 else 1
        n = self.n_unconstrained
        fit_inv_metric = None
        if adapted_inv is not None:
            shape = (n, n) if metric == "dense_e" else (n,)
            fit_inv_metric = [row.reshape(shape).copy() for row in adapted_inv]
        records = [resume_out.raw[c * record_size:(c + 1) * record_size]
                   for c in range(opts.chains)]
        fit = Fit(names, out, stats, opts.max_depth, seed, reports,
                  metric=metric, step_size=adapted_step,
                  inv_metric=fit_inv_metric, thin=opts.thin,
                  delta=opts.delta, resume=records,
                  threads_per_chain=threads_per_chain, variables=variables)
        if refresh:
            _print_sample_reports(reports, first_chain,
                                  _sampling_warnings(opts, reports, stats))
        del resume_bufs, resume_ptrs, resume_lens
        if continue_from is not None:
            return _continued_fit(continue_from, fit)
        return fit


# Kept separate from Model's JSON data path: functions bind typed buffers.
from ._function import Function  # noqa: E402
from .laplace import LaplaceWhitenReport, laplace_whiten  # noqa: E402

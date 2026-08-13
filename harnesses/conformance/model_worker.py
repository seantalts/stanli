#!/usr/bin/env python3
"""JSON-lines evaluator for reference and stanli BridgeStan models."""

from __future__ import annotations

import argparse
import contextlib
import json
import math
import pathlib
import sys
from typing import Optional


class TransportUnavailable(RuntimeError):
    pass


def _stan_call(function, *args, **kwargs):
    """Keep Stan `print` output off the JSON-lines stdout channel."""
    with contextlib.redirect_stdout(sys.stderr):
        return function(*args, **kwargs)


def _infrastructure_error(error: Exception) -> bool:
    return isinstance(error, (ImportError, ModuleNotFoundError,
                              TransportUnavailable))


def _number(value):
    value = float(value)
    if math.isnan(value):
        return "NaN"
    if value == math.inf:
        return "Infinity"
    if value == -math.inf:
        return "-Infinity"
    return value


def _phase(error: Exception, default: str) -> str:
    message = str(error).lower()
    if "syntax error" in message or "semantic error" in message \
            or "stanc" in message and "failed" in message:
        return "compile"
    return default


def _category(error: Exception) -> str:
    if isinstance(error, ValueError):
        return "invalid_argument"
    if isinstance(error, IndexError):
        return "out_of_range"
    message = str(error).lower()
    if "out_of_range" in message or "out of range" in message:
        return "out_of_range"
    if ("domain_error" in message or "constraint" in message
            or "must be greater" in message or "must be less" in message
            or "not a valid" in message or "does not sum to zero" in message):
        return "domain_error"
    if ("invalid_argument" in message or "mismatch" in message
            or "dimension" in message or "incorrect size" in message):
        return "invalid_argument"
    return "runtime_error"


class Worker:
    def __init__(self, model_path: pathlib.Path, backend: str,
                 data: Optional[pathlib.Path] = None):
        self.model_path = model_path
        self.backend = backend
        self.fixed_data = (json.loads(data.read_text(encoding="utf-8"))
                           if data is not None else None)
        self.models = {}
        self.failures = {}
        self.mir = None
        self.mir_error = None

    def stanli_mir(self, stanli):
        if self.mir_error is not None:
            raise self.mir_error
        if self.mir is None:
            try:
                self.mir = stanli.stan_to_mir(
                    self.model_path.read_text(encoding="utf-8"))
            except Exception as error:
                self.mir_error = error
                raise
        return self.mir

    def model(self, active_case: int):
        key = "fixed" if self.fixed_data is not None else active_case
        if key in self.models:
            return self.models[key]
        if key in self.failures:
            raise self.failures[key]
        data = (self.fixed_data if self.fixed_data is not None
                else {"active_case": active_case})
        try:
            if self.backend == "reference":
                import bridgestan
                model = _stan_call(
                    bridgestan.StanModel,
                    str(self.model_path), data, capture_stan_prints=True,
                    warn=False)
            else:
                import stanli
                missing = [name for name in ("bridgestan_model", "stan_to_mir")
                           if not hasattr(stanli, name)]
                if missing:
                    raise TransportUnavailable(
                        "this stanli build does not expose "
                        + ", ".join(missing))
                model = _stan_call(
                    stanli.bridgestan_model,
                    mir=self.stanli_mir(stanli),
                    name=self.model_path.stem, data=data,
                    capture_stan_prints=True)
        except Exception as error:
            self.failures[key] = error
            raise
        self.models[key] = model
        return model

    def _rejection(self, response, error: Exception, phase: str):
        if _infrastructure_error(error):
            response.update(protocol_error=True, message=str(error))
        else:
            response.update(accepted=False, phase=_phase(error, phase),
                            exception_category=_category(error),
                            message=str(error))
        return response

    def describe(self, request):
        response = {"request_id": request.get("request_id")}
        try:
            model = self.model(int(request.get("active_case", 1)))
            response.update(accepted=True,
                            parameter_count=int(model.param_unc_num()),
                            parameter_names=list(model.param_unc_names()))
        except Exception as error:
            self._rejection(response, error, "construction")
        return response

    def evaluate(self, request):
        response = {"request_id": request.get("request_id")}
        try:
            model = self.model(int(request.get("active_case", 1)))
        except Exception as error:
            return self._rejection(response, error, "construction")
        try:
            import numpy as np
            theta = np.asarray(request["point"], dtype=float)
            value, gradient = _stan_call(
                model.log_density_gradient, theta, propto=True, jacobian=True)
            response.update(accepted=True, value=_number(value),
                            gradient=[_number(item) for item in gradient])
        except Exception as error:
            return self._rejection(response, error, "evaluation")

        if request.get("include_outputs"):
            try:
                outputs = _stan_call(
                    model.param_constrain, theta, include_tp=True,
                    include_gq=True, rng=model.new_rng(1234))
                response.update(
                    outputs=[_number(item) for item in outputs],
                    output_names=list(model.param_names(
                        include_tp=True, include_gq=True)))
            except Exception as error:
                return self._rejection(response, error, "write_array")
        return response

    def request(self, request):
        return (self.describe(request) if request.get("action") == "describe"
                else self.evaluate(request))


def main(argv=None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("model", type=pathlib.Path)
    parser.add_argument("--backend", choices=("reference", "stanli"),
                        required=True)
    parser.add_argument("--data", type=pathlib.Path)
    args = parser.parse_args(argv)
    worker = Worker(args.model, args.backend, args.data)
    for line in sys.stdin:
        try:
            request = json.loads(line)
            response = worker.request(request)
        except Exception as error:
            response = {"request_id": None, "protocol_error": True,
                        "message": str(error)}
        print(json.dumps(response, separators=(",", ":")), flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())

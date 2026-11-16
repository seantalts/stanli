"""The status file stanli_check --status-json PATH writes.

One JSON record per line, flushed as written, so a run that crashed keeps
the records it finished:

  {"record": "compile", "ok": false, "stage": "stanc" | "data" | "model",
   "error": KIND, "message": str}
  {"record": "compile", "ok": true, "interpreter_fallbacks": [str, ...],
   "write_array": {"section": "none"}
                | {"section": "graph" | "interpreted", "columns": int,
                   "truncated": null | str}}
  {"record": "point", "point": int, "ok": bool,
   "error": KIND, "message": str,            only when ok is false
   "write_array": {"ok": true, "columns": int, "values": int,
                   "nonfinite": int}
                | {"ok": false, "error": KIND, "message": str}}

A point record has no write_array entry when the model has no section or
no columns. KIND is the C++ type of the exception: compile_error (stanli
cannot lower the model), mir_error (stanli cannot read the MIR), or the
Stan Math classes domain_error, out_of_range, invalid_argument and
runtime_error. A size or index error the compiler finds before evaluation
reports the class Stan Math would throw. "truncated" names what the
write_array graph could not express, or is null when it expresses the
whole section.
"""
from __future__ import annotations

import dataclasses
import json
import pathlib


@dataclasses.dataclass
class CheckStatus:
    compile: dict | None = None
    points: dict = dataclasses.field(default_factory=dict)

    @property
    def interpreted(self):
        """Does any part of the model run through the MIR interpreter?"""
        c = self.compile or {}
        return bool(c.get("interpreter_fallbacks")) or (
            c.get("write_array", {}).get("section") == "interpreted")

    def write_array(self):
        return (self.compile or {}).get("write_array", {"section": "none"})


def read(path):
    """The records in `path`; empty when the run wrote none."""
    status = CheckStatus()
    path = pathlib.Path(path)
    if not path.exists():
        return status
    for line in path.read_text(encoding="utf-8").splitlines():
        record = json.loads(line)
        if record["record"] == "compile":
            status.compile = record
        else:
            status.points[record["point"]] = record
    return status


# Failures that mean stanli lacks something, as opposed to Stan Math
# rejecting the model's values.
UNSUPPORTED = frozenset({"compile_error", "mir_error"})

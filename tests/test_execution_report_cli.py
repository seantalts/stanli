"""Exercise diagnostic JSON through the tool and the shipped C API."""
import ctypes as ct
import json
import os
from pathlib import Path
import subprocess
import sys


def child(library, fixture):
    lib = ct.CDLL(library)
    lib.stanli_model_new.argtypes = [ct.c_char_p, ct.c_char_p, ct.c_char_p, ct.c_size_t]
    lib.stanli_model_new.restype = ct.c_void_p
    lib.stanli_model_free.argtypes = [ct.c_void_p]
    lib.stanli_wa_n_columns.argtypes = [ct.c_void_p]
    lib.stanli_wa_n_columns.restype = ct.c_int64
    error = ct.create_string_buffer(8192)
    model = lib.stanli_model_new(Path(fixture).read_bytes(), b"{}", error, len(error))
    if not model:
        raise RuntimeError(error.value.decode())
    print(json.dumps({"columns": lib.stanli_wa_n_columns(model)}))
    lib.stanli_model_free(model)


def main(tool, library):
    fixtures = Path("tests/fixtures")
    fixture = fixtures / "execution_rng.tmir.sexp"
    # Use an existing empty JSON fixture; avoid temporary output beside sources.
    import tempfile
    with tempfile.TemporaryDirectory() as directory:
        data = Path(directory) / "empty.json"
        data.write_text("{}")
        command = [tool, str(fixture), str(data), "--execution-json"]
        result = subprocess.run(command, text=True, capture_output=True, check=True)
        report = json.loads(result.stdout)
        assert report["kind"] == "execution_manifest"
        assert report["host_probe"] == "not_run"
        assert report["write_array"]["status"] == "attached_unprobed"
        assert report["interpreter_events"]
        # The existing policy allows mandatory preparation, while the explicit
        # strict diagnostic rejects even constructing its interpreter.
        strict = subprocess.run(command + ["--forbid-mir"], text=True, capture_output=True)
        assert strict.returncode == 1
        assert json.loads(strict.stdout)["strict_violation"]
        assert "strict execution diagnostic" in strict.stderr
        nested = subprocess.run(command + ["--forbid-mir"], text=True, capture_output=True,
                                env={**os.environ, "STANLI_EXECUTION_REPORT": "1"})
        assert nested.returncode == 1
        assert json.loads(nested.stdout)["strict_violation"]

    for name, engine, probe in [
        ("execution_rng", "mir_interpreter", "passed"),
        ("execution_rng_reject", "unavailable_or_empty", "failed"),
        ("execution_legacy_ode", "bound_graph", "not_required"),
    ]:
        command = [sys.executable, __file__, "--child", library,
                   str(fixtures / (name + ".tmir.sexp"))]
        ordinary = subprocess.run(command, text=True, capture_output=True, check=True,
                                  env={**os.environ, "STANLI_EXECUTION_REPORT": "0"})
        observed = subprocess.run(command, text=True, capture_output=True, check=True,
                                  env={**os.environ, "STANLI_EXECUTION_REPORT": "1"})
        assert ordinary.stdout == observed.stdout
        assert '"kind":"execution_' not in ordinary.stderr
        events = [json.loads(line) for line in observed.stderr.splitlines()
                  if line.startswith('{"schema_version":')]
        manifests = [e for e in events if e["kind"] == "execution_manifest"]
        selections = [e for e in events if e["kind"] == "execution_host_selection"]
        assert len(manifests) == len(selections) == 1
        assert selections[0]["value_engine"] == engine, selections
        assert selections[0]["probe_status"] == probe, selections
        if probe == "failed":
            assert selections[0]["reason"]
    print("execution report CLI/C API checks passed")


if __name__ == "__main__":
    if sys.argv[1] == "--child":
        child(*sys.argv[2:])
    else:
        main(*sys.argv[1:])

"""Exercise diagnostic JSON through the tool and the shipped C API."""
import json
import os
from pathlib import Path
import subprocess
import sys


def run_checked(command, **kwargs):
    result = subprocess.run(command, text=True, capture_output=True, **kwargs)
    assert result.returncode == 0, (command, result.returncode, result.stdout, result.stderr)
    return result


def main(tool, host):
    fixtures = Path("tests/fixtures")
    fixture = fixtures / "execution_rng.tmir.sexp"
    # Use an existing empty JSON fixture; avoid temporary output beside sources.
    import tempfile
    with tempfile.TemporaryDirectory() as directory:
        data = Path(directory) / "empty.json"
        data.write_text("{}")
        command = [tool, str(fixture), str(data), "--execution-json"]
        result = run_checked(command)
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
        command = [host, str(fixtures / (name + ".tmir.sexp"))]
        ordinary = run_checked(command,
                               env={**os.environ, "STANLI_EXECUTION_REPORT": "0"})
        observed = run_checked(command,
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
    main(*sys.argv[1:])

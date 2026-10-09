import json
import pathlib
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
TOOLS = HERE.parent
REPO = TOOLS.parent
sys.path.insert(0, str(TOOLS))
sys.path.insert(0, str(HERE))
import verify_refs as v  # noqa: E402
from sexp import parse  # noqa: E402

PDB = pathlib.Path("/Users/xitrium/claud/stanrt/deps/posteriordb/posterior_database")
STANC = pathlib.Path("/Users/xitrium/claud/stanrt/deps/stanc3/stanc")


def load_case(model, tmp, refs=None):
    if refs is None:
        refs, _ = v.replay_refs(v.native_platform(), "clang")
    stan, dj = v.model_files(model, refs[model], PDB, pathlib.Path(tmp))
    return stan, dj, refs[model]


def compile_mir(stan):
    d = pathlib.Path(tempfile.mkdtemp())
    src = d / "model.stan"
    src.write_bytes(pathlib.Path(stan).read_bytes())
    r = subprocess.run([str(STANC), "--O0", "--debug-optimized-mir", "model.stan"],
                       cwd=d, capture_output=True, text=True, timeout=300)
    if r.returncode:
        raise RuntimeError("stanc failed: " + r.stderr[:300])
    return parse(r.stdout)

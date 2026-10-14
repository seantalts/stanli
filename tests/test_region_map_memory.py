"""Peak memory of compiling and evaluating a region map with a very large body."""
import resource
import subprocess
import sys

LIMIT_MB = 320


def main(stanli_check):
    subprocess.run(
        [stanli_check, "tests/cogmod/cm_invgaussian.stan",
         "tests/cogmod/cm_invgaussian.json"],
        check=True, capture_output=True, text=True)
    peak = resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss
    peak_mb = peak / 1e6 if sys.platform == "darwin" else peak / 1e3
    assert peak_mb < LIMIT_MB, f"peak {peak_mb:.0f} MB exceeds {LIMIT_MB} MB"


if __name__ == "__main__":
    main(sys.argv[1])

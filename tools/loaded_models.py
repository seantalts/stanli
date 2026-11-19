#!/usr/bin/env python3
"""List the models of a corpus run that were measured under heavy machine load.

python3 tools/loaded_models.py RUN.tsv [--max-load 6]

Prints a comma-separated list for `corpus_bench.py --models`. A model counts as
loaded when the 1-minute load average, recorded at the start of each of its
measured commands (Stanli compile, gradient pairs and preparation), went above
the threshold. Builds are left out: they run in parallel before any timing.
"""
import argparse
import copy
import json
from pathlib import Path
import re
import sys

DEFAULT_MAX_LOAD = 6.0
MEASURED = re.compile(r"(?P<model>.+)/(stanli-mir|gradient/\d+/(stanli|cmdstan)|prepare/\d+)")


def measurement_loads(events, models):
    """The highest 1-minute load average over each model's measured commands."""
    loads = {}
    for line in events.splitlines():
        event = json.loads(line)
        match = MEASURED.fullmatch(event.get("phase", ""))
        if not match or match["model"] not in models:
            continue
        load = event.get("load_average")
        if not isinstance(load, list) or not load or type(load[0]) not in (int, float):
            raise ValueError(f"No load average recorded for event {event.get('id')}")
        model = match["model"]
        loads[model] = max(loads.get(model, load[0]), load[0])
    return loads


def above(loads, max_load):
    return {model for model, load in loads.items() if load > max_load}


def check_rerun_identity(primary, rerun):
    """A rerun differs from its primary run only in which models it selected."""
    first, second = copy.deepcopy(primary["identity"]), copy.deepcopy(rerun["identity"])
    inputs, rerun_inputs = first.pop("inputs"), second.pop("inputs")
    for identity in (first, second):
        identity["config"].pop("models", None)
    if first != second or any(inputs.get(model) != wanted for model, wanted in rerun_inputs.items()):
        raise ValueError(f"Rerun {rerun['run_id']} identity differs from primary run {primary['run_id']}")


def plan_substitutions(primary, primary_events, reruns, max_load):
    """Which run supplies each model, decided by recorded load alone.

    The first rerun must contain exactly the loaded models, so the choice of what
    to rerun cannot depend on results. A later rerun may contain only models that
    are still loaded in the run that currently supplies them.
    """
    inputs = primary["identity"]["inputs"]
    loads = measurement_loads(primary_events, inputs)
    suppliers = {model: primary["run_id"] for model in inputs}
    for index, (manifest, events) in enumerate(reruns):
        check_rerun_identity(primary, manifest)
        wanted, loaded = set(manifest["identity"]["inputs"]), above(loads, max_load)
        if (index == 0 and wanted != loaded) or not wanted <= loaded:
            raise ValueError(
                f"Rerun {manifest['run_id']} must contain exactly the models above the load "
                f"threshold {max_load:g}" + (" in the primary run" if index == 0 else
                                            " in the run that supplies them"))
        rerun_loads = measurement_loads(events, wanted)
        for model in wanted:
            suppliers[model] = manifest["run_id"]
            loads[model] = rerun_loads.get(model, 0.0)
    return suppliers, sorted(model for model in above(loads, max_load)
                             if suppliers[model] != primary["run_id"])


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("summary", type=Path)
    parser.add_argument("--max-load", type=float, default=DEFAULT_MAX_LOAD)
    args = parser.parse_args(argv)
    run = Path(str(args.summary) + ".run")
    inputs = json.loads((run / "manifest.json").read_text())["identity"]["inputs"]
    loaded = sorted(above(measurement_loads((run / "events.jsonl").read_text(), inputs), args.max_load))
    print(",".join(loaded))
    print(f"{len(loaded)} of {len(inputs)} models measured above load {args.max_load:g}",
          file=sys.stderr)


if __name__ == "__main__":
    main()

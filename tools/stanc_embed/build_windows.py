"""Build a static OCaml embed object without partially linking Windows DLL imports."""

import json
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile


def output(*args, **kwargs):
    return subprocess.check_output(args, text=True, **kwargs).strip()


def main():
    source = Path(sys.argv[1]).resolve()
    target = "src/stanc_embed/stanc_embed.exe.o"
    rules = json.loads(output("dune", "describe", "rules", "--root", str(source),
                              "--format=json", "--profile=release", target))
    rule, = rules
    dependencies = [dep["File"] for dep in rule["deps"] if "File" in dep]
    subprocess.run(["dune", "build", "--root", source, "--profile=release",
                    "-j", sys.argv[2],
                    *(path for kind, path in dependencies if kind == "In_build_dir")],
                   check=True)

    _, directory, action = rule["action"]
    command = action[1:]
    workdir = source / directory
    ocamlopt = command[0]
    stdlib = Path(output(ocamlopt, "-where"))
    libraries = [workdir / arg for arg in command if arg.endswith(".cmxa")]
    library_dirs = [lib.parent for lib in libraries] + [stdlib]
    archives = []
    for library in libraries:
        info = output(str(Path(ocamlopt).with_name("ocamlobjinfo.exe")), str(library))
        c_objects = next(line.removeprefix("Extra C object files:")
                         for line in info.splitlines() if line.startswith("Extra C object files:"))
        for arg in shlex.split(c_objects):
            name = "lib" + arg[2:] + ".a" if arg.startswith("-l") else arg
            archive = next((path / name for path in library_dirs if (path / name).is_file()), None)
            # System import libraries stay unresolved until CMake's final link.
            if archive is not None and archive not in archives:
                archives.append(archive)
    archives.append(stdlib / "libasmrun.a")
    # The stock runtime references this support object; no flexlink invocation
    # is needed when its symbols and the empty static symbol table are linked in.
    runtime_support = stdlib / "flexdll" / (
        "flexdll_" + output(ocamlopt, "-config-var", "system") + ".o")
    compiler = os.environ.get("CC", "clang")
    c_runtime = Path(output(compiler, "-print-file-name=libmsvcrt.a"))

    destination = source / "_build/stanc_embed.static.o"
    with tempfile.TemporaryDirectory(prefix="stanc-static-") as tmp:
        tmp = Path(tmp)
        command = ["-output-obj" if arg == "-output-complete-obj" else arg
                   for arg in command]
        command[command.index("-o") + 1] = str(tmp / "ocaml.o")
        subprocess.run(command, cwd=workdir, check=True)
        combined = tmp / "combined.o"
        subprocess.run(["ld", "-r", "-u", "caml_startup", "-o", combined,
                        tmp / "ocaml.o", *archives, runtime_support], check=True)

        defined = {line.split()[0] for line in output(
            "nm", "--no-sort", "--format=posix", "--defined-only",
            str(combined), str(c_runtime)).splitlines() if line.split()}
        undefined = [line.split()[0] for line in output(
            "nm", "--no-sort", "--format=posix", "--undefined-only", str(combined)).splitlines()]
        # Prebuilt OCaml C stubs use DLL-import declarations. Bind their import
        # pointers to definitions in this static object, for both code and data.
        imports = sorted(name for name in undefined
                         if name.startswith("__imp_") and name[6:] in defined
                         and name not in defined)
        assembly = ('.section .rdata,"dr"\n.balign 8\n'
                    '.globl static_symtable\nstatic_symtable:\n.quad 0\n') + "".join(
            f".globl {name}\n{name}:\n.quad {name[6:]}\n" for name in imports)
        (tmp / "imports.s").write_text(assembly)
        subprocess.run([compiler, "-c", tmp / "imports.s", "-o", tmp / "imports.o"], check=True)
        subprocess.run(["ld", "-r", "--disable-auto-import", "-o", destination,
                        combined, tmp / "imports.o"], check=True)


if __name__ == "__main__":
    main()

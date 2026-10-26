"""Bundle native OCaml objects without partially linking Windows unwind tables."""

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
        inputs = [tmp / "ocaml.o", *archives, runtime_support]
        defined = {line.split()[0] for line in output(
            "nm", "--no-sort", "--format=posix", "--defined-only",
            *map(str, inputs), str(c_runtime)).splitlines() if line.split()}
        undefined = [line.split()[0] for line in output(
            "nm", "--no-sort", "--format=posix", "--undefined-only",
            *map(str, inputs)).splitlines() if line.split()]
        # Prebuilt OCaml C stubs use DLL-import declarations. Bind their import
        # pointers to static definitions, for both code and data. Give each
        # pointer its own archive member so an unused stub cannot pull in more
        # runtime code just because another stub needs an import pointer.
        imports = sorted({name for name in undefined
                          if name.startswith("__imp_") and name[6:] in defined
                          and name not in defined})
        assembly = ['.globl static_symtable\nstatic_symtable:\n.quad 0\n']
        assembly += [f".globl {name}\n{name}:\n.quad {name[6:]}\n" for name in imports]
        import_sources = []
        for index, text in enumerate(assembly):
            path = tmp / f"import_{index}.s"
            path.write_text('.section .rdata,"dr"\n.balign 8\n' + text)
            import_sources.append(path)
        subprocess.run([compiler, "-c", *import_sources], cwd=tmp, check=True)

        # GNU ld -r corrupts PE/COFF .pdata relocations (binutils PR 34704).
        # Those records can overlap unrelated C++ functions in the final
        # executable, breaking exception propagation even before OCaml starts.
        # Preserve every original C object and let the final linker relocate
        # it once. The historical .o filename is retained for setup/cache
        # compatibility; both GNU ld and LLD recognize the archive by magic.
        # https://sourceware.org/bugzilla/show_bug.cgi?id=34704
        objects = [tmp / "ocaml.o", runtime_support,
                   *(path.with_suffix(".o") for path in import_sources)]
        mri = [f'CREATE "{destination.as_posix()}"']
        mri += [f'ADDMOD "{path.as_posix()}"' for path in objects]
        mri += [f'ADDLIB "{path.as_posix()}"' for path in archives]
        mri += ["SAVE", "END"]
        subprocess.run([os.environ.get("AR", "ar"), "-M"],
                       input="\n".join(mri) + "\n", text=True, check=True)


if __name__ == "__main__":
    main()

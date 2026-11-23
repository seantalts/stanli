"""Check that the webR side module shares only the C API with other modules.

webR loads every shared object into one symbol table, and the first
definition of a name wins. If libstanli.so exports or imports Stan Math,
Eigen or Boost symbols, a compiled rstan model in another package binds to
stanli's copies (or stanli binds to the model's), and the two were built
against different Stan Math versions (issue #652).

Usage: python3 tests/check_wasm_exports.py build-wasm-side/libstanli.so
"""

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]

# Emitted by wasm-ld for every side module; the dynamic loader calls them.
LINKER_EXPORTS = {"__wasm_call_ctors", "__wasm_apply_data_relocs"}

# Type info and vtables of libc++ and Boost types that those libraries mark
# visibility("default") themselves, so that modules agree on exception types.
SHARED_RTTI = re.compile(r"_ZT[ISV](N?St|N5boost)")

# Namespaces that other packages' models also define (mangled prefixes).
PRIVATE_PREFIXES = ("_ZN4stan", "_ZNK4stan", "_ZN5Eigen", "_ZNK5Eigen",
                    "_ZN5boost", "_ZNK5boost", "_ZN3tbb", "_ZN6stanli",
                    "_ZNK6stanli")


def capi_names():
    source = (ROOT / "runtime/include/stanli/capi.h").read_text(
        encoding="utf-8")
    return set(re.findall(r"\b((?:stanli|bs)_\w+)\s*\(", source))


def _leb(data, i):
    result = shift = 0
    while True:
        byte = data[i]
        i += 1
        result |= (byte & 0x7F) << shift
        shift += 7
        if byte < 0x80:
            return result, i


def _name(data, i):
    n, i = _leb(data, i)
    return data[i:i + n].decode("utf-8", "replace"), i + n


def _skip_limits(data, i):
    flags = data[i]
    _, i = _leb(data, i + 1)
    if flags & 1:
        _, i = _leb(data, i)
    return i


def read_module(path):
    """Return (imports, exports) as lists of (module, name) and names."""
    data = pathlib.Path(path).read_bytes()
    if data[:4] != b"\0asm":
        raise ValueError(f"{path} is not a wasm module")
    imports, exports = [], []
    i = 8
    while i < len(data):
        section = data[i]
        size, i = _leb(data, i + 1)
        end = i + size
        if section == 2:
            count, j = _leb(data, i)
            for _ in range(count):
                module, j = _name(data, j)
                name, j = _name(data, j)
                kind = data[j]
                j += 1
                if kind == 0:
                    _, j = _leb(data, j)
                elif kind == 1:
                    j = _skip_limits(data, j + 1)
                elif kind == 2:
                    j = _skip_limits(data, j)
                elif kind == 3:
                    j += 2
                elif kind == 4:
                    _, j = _leb(data, j + 1)
                imports.append((module, name))
        elif section == 7:
            count, j = _leb(data, i)
            for _ in range(count):
                name, j = _name(data, j)
                _, j = _leb(data, j + 1)
                exports.append(name)
        i = end
    return imports, exports


def main(path):
    imports, exports = read_module(path)
    allowed = capi_names() | LINKER_EXPORTS
    extra_exports = sorted(name for name in set(exports) - allowed
                           if not SHARED_RTTI.match(name))
    private_imports = sorted({name for _, name in imports
                              if name.startswith(PRIVATE_PREFIXES)})
    print(f"{path}: {len(exports)} exports, {len(imports)} imports")
    for label, names in (("exports outside the C API", extra_exports),
                         ("imports of private C++ symbols", private_imports)):
        if names:
            print(f"{len(names)} {label}, for example:")
            for name in names[:10]:
                print(f"  {name}")
    return 1 if extra_exports or private_imports else 0


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    sys.exit(main(sys.argv[1]))

"""Keep the explicit PE exports in sync with the public C ABI headers."""

import pathlib
import re
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
HEADERS = (
    "runtime/include/stanli/capi.h",
    "runtime/include/stanli/function.hpp",
    "runtime/ext/bridgestan.h",
)


def declared_symbols(source):
    # Comments mention API calls too; callback typedefs are types, not exports.
    source = re.sub(r"/\*.*?\*/|//[^\n]*", "", source, flags=re.DOTALL)
    functions = set(re.findall(r"\b((?:stanli|bs)_\w+)\s*\(", source))
    data = set(re.findall(r"\bextern\s+const\s+int\s+(bs_\w+)\s*;", source))
    return functions, data


def exported_symbols(source):
    functions, data = set(), set()
    for line in source.splitlines():
        fields = line.split(";", 1)[0].split()
        if not fields or fields == ["EXPORTS"]:
            continue
        (data if "DATA" in fields[1:] else functions).add(fields[0])
    return functions, data


class WindowsExportsTest(unittest.TestCase):
    def test_public_abi_matches_windows_exports(self):
        declared_functions, declared_data = set(), set()
        for header in HEADERS:
            functions, data = declared_symbols(
                (ROOT / header).read_text(encoding="utf-8")
            )
            self.assertTrue(functions, f"no functions found in {header}")
            declared_functions.update(functions)
            declared_data.update(data)
        exported_functions, exported_data = exported_symbols(
            (ROOT / "tools/exported_symbols.def").read_text(encoding="utf-8")
        )
        self.assertEqual(declared_functions, exported_functions)
        self.assertEqual(declared_data, exported_data)

    def test_declarations_ignore_comments_and_callback_types(self):
        self.assertEqual(
            declared_symbols("""
                // stanli_comment();
                /* bs_comment(); */
                typedef void (*stanli_callback)(int value);
                int64_t stanli_example
                    (const stanli_model* model);
                BS_PUBLIC extern const int bs_major_version;
                BS_PUBLIC void bs_example(void);
            """),
            ({"stanli_example", "bs_example"}, {"bs_major_version"}),
        )

    def test_data_exports_are_distinct_from_functions(self):
        self.assertEqual(
            exported_symbols("""
                ; stanli_comment
                EXPORTS
                  stanli_example ; a function
                  bs_major_version DATA
            """),
            ({"stanli_example"}, {"bs_major_version"}),
        )


if __name__ == "__main__":
    unittest.main()

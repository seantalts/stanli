"""Exercise the PE table gate with valid and corrupt binary records."""

from pathlib import Path
import struct
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from check_pe_unwind import check


def image(records):
    data = bytearray(0x600)
    data[:2] = b"MZ"
    struct.pack_into("<I", data, 0x3c, 0x80)
    data[0x80:0x84] = b"PE\0\0"
    struct.pack_into("<HH", data, 0x84, 0x8664, 2)
    struct.pack_into("<H", data, 0x94, 0xf0)
    struct.pack_into("<H", data, 0x98, 0x20b)
    struct.pack_into("<I", data, 0x98 + 108, 16)
    struct.pack_into("<II", data, 0x98 + 136, 0x2000, len(records) * 12)
    for index, (rva, raw, flags) in enumerate(((0x1000, 0x200, 0x60000020),
                                             (0x2000, 0x400, 0x40000040))):
        section = 0x188 + index * 40
        struct.pack_into("<IIII", data, section + 8, 0x200, rva, 0x200, raw)
        struct.pack_into("<I", data, section + 36, flags)
    for index, record in enumerate(records):
        struct.pack_into("<III", data, 0x400 + index * 12, *record)
    data[0x500] = 1  # UNWIND_INFO version 1, no frame or unwind operations.
    return data


class PeUnwindTest(unittest.TestCase):
    def test_valid_adjacent_functions(self):
        self.assertEqual(check(image([(0x1000, 0x1020, 0x2100),
                                      (0x1020, 0x1080, 0x2100)])), 2)

    def test_partial_link_overlap(self):
        with self.assertRaisesRegex(ValueError, "overlapping"):
            check(image([(0x1000, 0x1080, 0x2100), (0x1040, 0x1090, 0x2100)]))

    def test_duplicate_start(self):
        with self.assertRaisesRegex(ValueError, "overlapping"):
            check(image([(0x1000, 0x1080, 0x2100), (0x1000, 0x1020, 0x2100)]))

    def test_range_outside_code(self):
        with self.assertRaisesRegex(ValueError, "not executable"):
            check(image([(0x2000, 0x2080, 0x2100)]))

    def test_bad_unwind_pointer(self):
        with self.assertRaisesRegex(ValueError, "outside file-backed"):
            check(image([(0x1000, 0x1080, 0x3000)]))

    def test_bad_unwind_version(self):
        data = image([(0x1000, 0x1080, 0x2100)])
        data[0x500] = 0
        with self.assertRaisesRegex(ValueError, "invalid unwind-info version"):
            check(data)

    def test_truncated_image(self):
        with self.assertRaisesRegex(ValueError, "truncated PE image"):
            check(b"MZ")


if __name__ == "__main__":
    unittest.main()

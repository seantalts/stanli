#!/usr/bin/env python3
"""Mutation tests for the function-model generator's inventory gate."""
import copy
import pathlib
import sys
import unittest
from unittest import mock

REPO = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "tools"))
import gen_function_models as generator


class InventoryGateTest(unittest.TestCase):
    def test_removing_a_runtime_function_probe_fails_the_inventory_gate(self):
        groups = copy.deepcopy(generator.GROUPS)
        del groups["containers"]["reverse"]
        with mock.patch.object(generator, "GROUPS", groups):
            with self.assertRaisesRegex(ValueError, "recipes: reverse"):
                generator.render()


if __name__ == "__main__":
    unittest.main()

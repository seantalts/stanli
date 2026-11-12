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
    """A runtime function needs a probe in a function or signature model."""

    def render(self, recipe=None, signature=None):
        groups = copy.deepcopy(generator.GROUPS)
        if recipe:
            del groups["containers"][recipe]
        names = generator.signature_names() - {signature}
        with mock.patch.object(generator, "GROUPS", groups), \
                mock.patch.object(generator, "signature_names", lambda: names):
            return generator.render()

    def test_a_function_in_neither_inventory_fails_the_gate(self):
        with self.assertRaisesRegex(ValueError, "recipes: reverse"):
            self.render(recipe="reverse", signature="reverse")

    def test_either_inventory_covers_a_function(self):
        self.render(recipe="reverse")
        self.render(signature="reverse")


if __name__ == "__main__":
    unittest.main()

"""Every mutating tool must have decided whether it can be undone.

AGENTS.md states that a mutating tool must have an entry in mutation.py. The
contract was unenforced and mostly unmet: 22 IDA and 26 Binary Ninja mutating
tools shared 9 builders, three of which named tools that exist in neither host.

This test makes the claim true by requiring every mutating tool to be in one of
two places — a reverse builder, or the explicit irreversible set — so adding a
mutating tool without deciding fails here instead of silently producing an undo
that does nothing.
"""

from __future__ import annotations

import importlib
import os
import pkgutil
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(__file__)))
from tests.mocks.ida_mock import install_ida_mocks

install_ida_mocks()

for _mod_name in ("rikugan.core.types", "rikugan.core.config"):
    sys.modules.pop(_mod_name, None)

from rikugan.agent.mutation import _KNOWN_IRREVERSIBLE, _REVERSE_BUILDERS  # noqa: E402
from rikugan.tools.base import ToolDefinition  # noqa: E402


def _mutating_tool_names(package_path: str) -> set[str]:
    """Names of every mutating tool a host package declares."""
    package = importlib.import_module(package_path)
    names: set[str] = set()
    for module_info in pkgutil.iter_modules(package.__path__):
        try:
            module = importlib.import_module(f"{package_path}.{module_info.name}")
        except Exception:  # a host API missing in the test environment
            continue
        for value in vars(module).values():
            defn = getattr(value, "_tool_definition", None)
            if isinstance(defn, ToolDefinition) and defn.mutating:
                names.add(defn.name)
    return names


class TestEveryMutatingToolHasDecided(unittest.TestCase):
    def _assert_all_accounted_for(self, package_path: str):
        undecided = sorted(
            name
            for name in _mutating_tool_names(package_path)
            if name not in _REVERSE_BUILDERS and name not in _KNOWN_IRREVERSIBLE
        )
        self.assertEqual(
            undecided,
            [],
            f"mutating tools in {package_path} with no reverse builder and not "
            f"listed in _KNOWN_IRREVERSIBLE: {undecided}. Add a builder, or add "
            f"the name to _KNOWN_IRREVERSIBLE to say so deliberately.",
        )

    def test_ida_tools(self):
        self._assert_all_accounted_for("rikugan.ida.tools")

    def test_binja_tools(self):
        self._assert_all_accounted_for("rikugan.binja.tools")

    def test_the_two_sets_do_not_overlap(self):
        both = sorted(set(_REVERSE_BUILDERS) & _KNOWN_IRREVERSIBLE)
        self.assertEqual(both, [], f"claimed both reversible and not: {both}")

    def test_no_builder_names_a_tool_that_does_not_exist(self):
        # Three keys used to: rename_data, retype_variable and
        # rename_single_variable, none of which either host declares.
        declared = _mutating_tool_names("rikugan.ida.tools") | _mutating_tool_names("rikugan.binja.tools")
        orphans = sorted(name for name in _REVERSE_BUILDERS if name not in declared)
        self.assertEqual(orphans, [], f"reverse builders for nonexistent tools: {orphans}")


if __name__ == "__main__":
    unittest.main()

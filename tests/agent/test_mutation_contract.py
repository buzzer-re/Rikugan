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

from rikugan.agent.mutation import (  # noqa: E402
    _KNOWN_IRREVERSIBLE,
    _REVERSE_BUILDERS,
    build_reverse_record,
    capture_pre_state,
)
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


def _tool_definitions(package_path: str) -> dict[str, ToolDefinition]:
    package = importlib.import_module(package_path)
    found: dict[str, ToolDefinition] = {}
    for module_info in pkgutil.iter_modules(package.__path__):
        try:
            module = importlib.import_module(f"{package_path}.{module_info.name}")
        except Exception:  # a host API missing in the test environment
            continue
        for value in vars(module).values():
            defn = getattr(value, "_tool_definition", None)
            if isinstance(defn, ToolDefinition):
                found[defn.name] = defn
    return found


class TestUndoCallsMatchTheRealTools(unittest.TestCase):
    """Builders fed by the tools' real parameters must call real tools correctly.

    Undo once sent rename_function old_name and new_name when it takes address and
    new_name, so every undo failed. Its unit tests fed the builders those same
    wrong names, and passed. This derives the names from the tools themselves.
    """

    HOSTS = ("rikugan.ida.tools", "rikugan.binja.tools")

    def _assert_valid_call(self, tools, tool_name, arguments, context):
        self.assertIn(tool_name, tools, f"{context} calls {tool_name}, which does not exist")
        parameters = tools[tool_name].parameters
        unknown = set(arguments) - {p.name for p in parameters}
        missing = {p.name for p in parameters if p.required} - set(arguments)
        self.assertFalse(unknown, f"{context} passes {tool_name} arguments it does not take: {unknown}")
        self.assertFalse(missing, f"{context} leaves out {tool_name}'s required arguments: {missing}")

    def _check_host(self, package_path):
        tools = _tool_definitions(package_path)
        for name in _REVERSE_BUILDERS:
            if name not in tools:
                continue
            with self.subTest(host=package_path, tool=name):
                arguments = {p.name: f"sample-{p.name}" for p in tools[name].parameters if p.required}
                reads = []

                def executor(getter, getter_args, reads=reads):
                    reads.append((getter, getter_args))
                    return "the old value"

                pre = capture_pre_state(name, arguments, executor)
                self.assertNotIn("_capture_failed", pre)
                for getter, getter_args in reads:
                    self._assert_valid_call(tools, getter, getter_args, f"capturing {name}")

                record = build_reverse_record(name, arguments, pre)
                self.assertTrue(record.reversible, f"{name} should be undoable: {record.irreversible_reason}")
                self._assert_valid_call(tools, record.reverse_tool, record.reverse_arguments, f"undoing {name}")

    def test_ida(self):
        self._check_host(self.HOSTS[0])

    def test_binary_ninja(self):
        self._check_host(self.HOSTS[1])

    def test_it_really_saw_the_tools(self):
        # A module failing to import is skipped above; that must not empty this test.
        for host in self.HOSTS:
            tools = _tool_definitions(host)
            for name in ("rename_function", "rename_variable", "rename_address", "set_comment"):
                with self.subTest(host=host, tool=name):
                    self.assertIn(name, tools)

    def test_readers_are_not_offered_to_the_model(self):
        for host in self.HOSTS:
            tools = _tool_definitions(host)
            for name in ("read_function_name", "read_comment", "read_function_comment", "read_function_prototype"):
                with self.subTest(host=host, tool=name):
                    self.assertTrue(tools[name].internal)


if __name__ == "__main__":
    unittest.main()

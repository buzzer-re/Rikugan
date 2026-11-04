"""Which tools get marshalled onto the host's UI thread.

Binary Ninja's BinaryView API is thread-safe, so read-only analysis can run on
the agent's background thread. Marshalling it froze the whole window for the
length of every decompile. Writes and UI-driving tools must still go through
the main thread, so the policy is worth pinning down.
"""

from __future__ import annotations

import unittest

from rikugan.tools.base import ToolDefinition
from rikugan.tools.registry import ToolRegistry


def _defn(name: str, mutating: bool = False, handler=None) -> ToolDefinition:
    return ToolDefinition(
        name=name,
        description="",
        parameters=[],
        handler=handler or (lambda: "ok"),
        category="test",
        mutating=mutating,
    )


class _Marshaller:
    """Stand-in for idasync that records what it was asked to wrap."""

    def __init__(self) -> None:
        self.wrapped: list = []

    def __call__(self, handler):
        self.wrapped.append(handler)

        def _wrapper(**kwargs):
            return handler(**kwargs)

        return _wrapper


class TestMarshalPolicy(unittest.TestCase):
    def test_without_a_policy_every_tool_is_marshalled(self):
        """IDA's default: all API calls belong on the main thread."""
        marshaller = _Marshaller()
        registry = ToolRegistry(dispatch_wrapper=marshaller)
        registry.register(_defn("read_only_tool"))
        registry.register(_defn("writing_tool", mutating=True))

        registry.execute("read_only_tool", {})
        registry.execute("writing_tool", {})

        self.assertEqual(len(marshaller.wrapped), 2)

    def test_policy_can_keep_reads_off_the_main_thread(self):
        marshaller = _Marshaller()
        registry = ToolRegistry(
            dispatch_wrapper=marshaller,
            marshal_policy=lambda defn: defn.mutating,
        )
        registry.register(_defn("read_only_tool"))
        registry.register(_defn("writing_tool", mutating=True))

        registry.execute("read_only_tool", {})
        self.assertEqual([], marshaller.wrapped)

        registry.execute("writing_tool", {})
        self.assertEqual(1, len(marshaller.wrapped))

    def test_results_are_unchanged_either_way(self):
        for policy in (None, lambda defn: defn.mutating):
            with self.subTest(policy=policy is not None):
                registry = ToolRegistry(dispatch_wrapper=_Marshaller(), marshal_policy=policy)
                registry.register(_defn("echo", handler=lambda: "value"))
                self.assertEqual("value", registry.execute("echo", {}))


class TestBinaryNinjaPolicy(unittest.TestCase):
    def test_mutating_and_ui_tools_stay_on_the_main_thread(self):
        from rikugan.binja.tools.registry import _needs_main_thread

        self.assertTrue(_needs_main_thread(_defn("rename_function", mutating=True)))
        self.assertTrue(_needs_main_thread(_defn("jump_to")))
        self.assertTrue(_needs_main_thread(_defn("execute_python")))

    def test_read_only_analysis_may_run_in_the_background(self):
        from rikugan.binja.tools.registry import _needs_main_thread

        for name in ("decompile_function", "get_xrefs_to", "list_strings", "get_binary_info"):
            with self.subTest(tool=name):
                self.assertFalse(_needs_main_thread(_defn(name)))

    def test_the_flag_gates_the_whole_behavior(self):
        """Turning the setting off restores main-thread dispatch for every BN API tool."""
        import rikugan.binja.tools.registry as bn_registry

        class _Cfg:
            binja_background_tools = False

        for registry in (bn_registry.create_default_registry(_Cfg()), bn_registry.create_default_registry()):
            for name in ("decompile_function", "get_il", "list_functions"):
                with self.subTest(tool=name):
                    self.assertTrue(registry._marshal_policy(registry.get(name)))

        class _On:
            binja_background_tools = True

        registry = bn_registry.create_default_registry(_On())
        self.assertFalse(registry._marshal_policy(registry.get("list_functions")))
        self.assertTrue(registry._marshal_policy(registry.get("jump_to")))



def _defn_in(category):
    return type("_Defn", (), {"category": category, "name": "x", "mutating": False})()


class TestFileToolsNeverBlockTheHost(unittest.TestCase):
    """Marshalled, a slow file tool freezes the whole window, and the registry's
    timeout cannot rescue it: a started future cannot be cancelled."""

    FILE_TOOLS = ("read_file", "write_file", "edit_file", "restore_file_backup")

    def test_binary_ninja_either_way(self):
        import rikugan.binja.tools.registry as bn_registry

        for background in (True, False):
            config = type("_Cfg", (), {"binja_background_tools": background})()
            registry = bn_registry.create_default_registry(config)
            for name in self.FILE_TOOLS:
                with self.subTest(tool=name, background=background):
                    self.assertFalse(registry._marshal_policy(registry._tools[name]))

    def test_ida(self):
        from rikugan.constants import FILESYSTEM_CATEGORY
        from rikugan.ida.tools.registry import _needs_main_thread

        self.assertFalse(_needs_main_thread(_defn_in(FILESYSTEM_CATEGORY)))
        self.assertTrue(_needs_main_thread(_defn_in("decompiler")))

    def test_they_are_not_mutating_or_they_would_marshal_and_flush_the_cache(self):
        from rikugan.tools import filesystem

        for name in self.FILE_TOOLS:
            with self.subTest(tool=name):
                self.assertFalse(getattr(filesystem, name)._tool_definition.mutating)


if __name__ == "__main__":
    unittest.main()

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
        """Turning the setting off restores main-thread dispatch for everything."""
        import rikugan.binja.tools.registry as bn_registry

        class _Cfg:
            binja_background_tools = False

        self.assertIsNone(bn_registry.create_default_registry(_Cfg())._marshal_policy)
        self.assertIsNone(bn_registry.create_default_registry()._marshal_policy)

        class _On:
            binja_background_tools = True

        self.assertIsNotNone(bn_registry.create_default_registry(_On())._marshal_policy)


if __name__ == "__main__":
    unittest.main()

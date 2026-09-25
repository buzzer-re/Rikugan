"""What the model is told about: tool payload size and readable contrast.

Enabling Binary Ninja's own MCP server declares 75 further tools, named
``bn_*`` so they collide with none of Rikugan's. Left alone that sends two
full tool sets on every turn, which is what made the API refuse the request.
"""

from __future__ import annotations

import unittest

from rikugan.binja import native_mcp
from rikugan.mcp.bridge import _MAX_TOOL_DESCRIPTION, _MAX_TOOL_NAME, describe_payload, register_mcp_tools
from rikugan.tools.base import ToolDefinition
from rikugan.tools.registry import ToolRegistry


def _defn(name: str, mutating: bool = False) -> ToolDefinition:
    return ToolDefinition(
        name=name,
        description=f"does {name}",
        parameters=[],
        category="test",
        handler=lambda **kw: name,
        mutating=mutating,
    )


class _FakeTool:
    def __init__(self, name, description="", input_schema=None):
        self.name = name
        self.description = description
        self.input_schema = input_schema or {}


class _FakeClient:
    def __init__(self, tools):
        self.name = "binaryninja"
        self._tools = tools

    def get_tools(self):
        return self._tools

    def call_tool(self, name, arguments):
        return f"called {name}"


class TestShadowing(unittest.TestCase):
    def setUp(self):
        self.registry = ToolRegistry()
        self.registry.register(_defn("decompile_function"))
        self.registry.register(_defn("rename_function", mutating=True))

    def test_a_shadowed_tool_is_not_declared_to_the_model(self):
        self.registry.set_shadowed(["decompile_function"])
        declared = {t["function"]["name"] for t in self.registry.to_provider_format()}
        self.assertNotIn("decompile_function", declared)
        self.assertIn("rename_function", declared)
        self.assertNotIn("decompile_function", self.registry.list_names())

    def test_a_shadowed_tool_still_runs(self):
        # A saved session or a skill may still name it; hiding it from the
        # declaration must not turn a replay into a ToolNotFoundError.
        self.registry.set_shadowed(["decompile_function"])
        self.assertEqual(self.registry.execute("decompile_function", {}), "decompile_function")

    def test_clearing_brings_the_tool_back(self):
        self.registry.set_shadowed(["decompile_function"])
        self.registry.clear_shadowed()
        self.assertIn("decompile_function", self.registry.list_names())

    def test_shadowing_invalidates_the_schema_cache(self):
        before = {t["function"]["name"] for t in self.registry.to_provider_format()}
        self.assertIn("decompile_function", before)
        self.registry.set_shadowed(["decompile_function"])
        after = {t["function"]["name"] for t in self.registry.to_provider_format()}
        self.assertNotEqual(before, after)


class TestSupersededBuiltins(unittest.TestCase):
    def test_every_rikugan_tool_stands_down(self):
        registry = ToolRegistry()
        registry.register(_defn("list_functions"))
        registry.register(_defn("rename_function", mutating=True))
        registry.register(_defn("execute_python", mutating=True))
        registry.register(_defn("mcp_binaryninja_bn_function_list"))

        superseded = native_mcp.superseded_builtins(registry)

        # It is one tool set or the other: declaring both on every request is
        # what made the API refuse it.
        self.assertEqual(
            sorted(superseded),
            ["execute_python", "list_functions", "rename_function"],
        )

    def test_the_host_server_never_supersedes_itself(self):
        registry = ToolRegistry()
        registry.register(_defn("mcp_binaryninja_bn_function_list"))
        self.assertEqual(native_mcp.superseded_builtins(registry), [])

    def test_the_declaration_falls_back_to_the_host_set_alone(self):
        registry = ToolRegistry()
        for i in range(30):
            registry.register(_defn(f"read_{i}"))
        for i in range(10):
            registry.register(_defn(f"write_{i}", mutating=True))
        for i in range(75):
            registry.register(_defn(f"mcp_binaryninja_bn_{i}"))

        both = len(registry.to_provider_format())
        registry.set_shadowed(native_mcp.superseded_builtins(registry))
        after = len(registry.to_provider_format())

        self.assertEqual(both, 115)
        self.assertEqual(after, 75)


class TestBridgePayload(unittest.TestCase):
    def test_a_long_description_is_clipped(self):
        registry = ToolRegistry()
        client = _FakeClient([_FakeTool("bn_info", "word " * 400)])
        register_mcp_tools(client, registry, prefix="mcp_binaryninja_")
        defn = registry.get("mcp_binaryninja_bn_info")
        assert defn is not None
        # Every declared tool is re-sent each turn, so a server's prose is a
        # per-turn cost; the prefix leaves room for it.
        self.assertLess(len(defn.description), _MAX_TOOL_DESCRIPTION + 40)
        self.assertTrue(defn.description.endswith("…"))

    def test_a_short_description_is_left_alone(self):
        registry = ToolRegistry()
        client = _FakeClient([_FakeTool("bn_info", "Return binary metadata.")])
        register_mcp_tools(client, registry, prefix="mcp_binaryninja_")
        defn = registry.get("mcp_binaryninja_bn_info")
        assert defn is not None
        self.assertTrue(defn.description.endswith("Return binary metadata."))

    def test_a_name_the_providers_would_reject_is_skipped(self):
        registry = ToolRegistry()
        long_name = "bn_" + "x" * _MAX_TOOL_NAME
        client = _FakeClient([_FakeTool(long_name, "x"), _FakeTool("bn_ok", "x")])
        count = register_mcp_tools(client, registry, prefix="mcp_binaryninja_")
        self.assertEqual(count, 1)
        self.assertEqual(registry.list_names(), ["mcp_binaryninja_bn_ok"])

    def test_describe_payload_reports_what_is_sent(self):
        registry = ToolRegistry()
        registry.register(_defn("a"))
        self.assertIn("1 tools declared", describe_payload(registry))


if __name__ == "__main__":
    unittest.main()

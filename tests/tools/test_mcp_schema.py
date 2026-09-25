"""MCP schemas must survive the trip to a model provider.

A server writes full JSON Schema, including $ref pointers into its own $defs.
Relaying one whose target was left behind does not degrade that parameter — it
invalidates the tool, and one bad tool fails the whole request.
"""

from __future__ import annotations

import unittest

from rikugan.mcp.bridge import register_mcp_tools
from rikugan.mcp.schema import collect_defs, sanitize_schema
from rikugan.tools.registry import ToolRegistry


class _FakeTool:
    def __init__(self, name, description="", input_schema=None):
        self.name = name
        self.description = description
        self.input_schema = input_schema or {}


class _FakeClient:
    name = "srv"

    def __init__(self, tools):
        self._tools = tools

    def get_tools(self):
        return self._tools

    def call_tool(self, name, arguments):
        return name


def _walk(node):
    """Yield every dict in a schema tree."""
    if isinstance(node, dict):
        yield node
        for value in node.values():
            yield from _walk(value)
    elif isinstance(node, list):
        for item in node:
            yield from _walk(item)


class TestSanitizeSchema(unittest.TestCase):
    def test_a_local_ref_is_inlined(self):
        doc = {
            "$defs": {"Addr": {"type": "string", "description": "hex address"}},
            "type": "object",
            "properties": {"at": {"$ref": "#/$defs/Addr"}},
        }
        out = sanitize_schema(doc)
        self.assertEqual(out["properties"]["at"]["type"], "string")
        self.assertEqual(out["properties"]["at"]["description"], "hex address")

    def test_a_dangling_ref_is_dropped_not_relayed(self):
        doc = {"type": "object", "properties": {"at": {"$ref": "#/$defs/Missing"}}}
        out = sanitize_schema(doc)
        self.assertEqual(out["properties"]["at"], {})
        self.assertNotIn("$ref", str(out))

    def test_a_remote_ref_is_not_fetched(self):
        doc = {"type": "object", "properties": {"x": {"$ref": "https://example.com/s.json"}}}
        self.assertEqual(sanitize_schema(doc)["properties"]["x"], {})

    def test_document_keywords_are_removed(self):
        doc = {
            "$schema": "https://json-schema.org/draft/2020-12/schema",
            "$id": "urn:x",
            "$comment": "internal",
            "type": "object",
            "properties": {"a": {"type": "string"}},
        }
        out = sanitize_schema(doc)
        self.assertEqual(set(out), {"type", "properties"})

    def test_nested_objects_keep_their_shape(self):
        # Flattening a nested object left the model with no way to fill it.
        doc = {
            "type": "object",
            "properties": {
                "opts": {
                    "type": "object",
                    "properties": {"depth": {"type": "integer"}},
                    "required": ["depth"],
                }
            },
        }
        out = sanitize_schema(doc)
        self.assertEqual(out["properties"]["opts"]["properties"]["depth"]["type"], "integer")
        self.assertEqual(out["properties"]["opts"]["required"], ["depth"])

    def test_a_ref_inside_array_items_is_resolved(self):
        doc = {
            "$defs": {"Sym": {"type": "string"}},
            "type": "object",
            "properties": {"names": {"type": "array", "items": {"$ref": "#/$defs/Sym"}}},
        }
        out = sanitize_schema(doc)
        self.assertEqual(out["properties"]["names"]["items"], {"type": "string"})

    def test_a_self_referential_type_terminates(self):
        doc = {
            "$defs": {"Node": {"type": "object", "properties": {"child": {"$ref": "#/$defs/Node"}}}},
            "type": "object",
            "properties": {"root": {"$ref": "#/$defs/Node"}},
        }
        out = sanitize_schema(doc)  # must not recurse forever
        self.assertEqual(out["properties"]["root"]["type"], "object")

    def test_a_ref_sibling_refines_the_target(self):
        doc = {
            "$defs": {"Addr": {"type": "string"}},
            "type": "object",
            "properties": {"at": {"$ref": "#/$defs/Addr", "description": "where"}},
        }
        out = sanitize_schema(doc)
        self.assertEqual(out["properties"]["at"]["description"], "where")

    def test_empty_combinator_branches_are_dropped(self):
        doc = {"type": "object", "properties": {"x": {"anyOf": [{"$ref": "#/$defs/Gone"}]}}}
        self.assertNotIn("anyOf", sanitize_schema(doc)["properties"]["x"])

    def test_collect_defs_reads_both_spellings(self):
        self.assertEqual(
            set(collect_defs({"$defs": {"A": {}}, "definitions": {"B": {}}})),
            {"A", "B"},
        )


class TestBridgeEmitsResolvableSchemas(unittest.TestCase):
    def test_no_ref_survives_into_a_registered_tool(self):
        schema = {
            "$schema": "https://json-schema.org/draft/2020-12/schema",
            "$defs": {"Addr": {"type": "string"}},
            "type": "object",
            "properties": {
                "at": {"$ref": "#/$defs/Addr"},
                "gone": {"$ref": "#/$defs/NotThere"},
                "opts": {"type": "object", "properties": {"n": {"type": "integer"}}},
            },
            "required": ["at"],
        }
        registry = ToolRegistry()
        register_mcp_tools(_FakeClient([_FakeTool("t", "d", schema)]), registry, prefix="mcp_srv_")
        out = registry.get("mcp_srv_t").to_json_schema()

        self.assertFalse(any("$ref" in node for node in _walk(out)), out)
        self.assertEqual(out["properties"]["at"]["type"], "string")
        self.assertEqual(out["properties"]["opts"]["properties"]["n"]["type"], "integer")
        self.assertEqual(out["required"], ["at"])


if __name__ == "__main__":
    unittest.main()

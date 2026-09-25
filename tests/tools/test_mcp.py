"""Tests for the MCP client system."""

from __future__ import annotations

import json
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(__file__)))
from tests.mocks.ida_mock import install_ida_mocks
install_ida_mocks()

from rikugan.mcp.config import MCPServerConfig, load_mcp_config, save_mcp_config
from rikugan.mcp.protocol import (
    MCPToolSchema,
    encode_jsonrpc_request,
    decode_jsonrpc_response,
)
from rikugan.constants import MCP_TOOL_PREFIX
from rikugan.mcp.bridge import _mcp_schema_to_parameters, register_mcp_tools
from rikugan.mcp.client import MCPClient
from rikugan.mcp.manager import MCPManager
from rikugan.tools.registry import ToolRegistry


class TestMCPConfig(unittest.TestCase):
    def test_load_valid_config(self):
        with tempfile.NamedTemporaryFile(mode="w", suffix=".json", delete=False) as f:
            json.dump({
                "mcpServers": {
                    "test-server": {
                        "command": "python",
                        "args": ["-m", "test_mcp"],
                        "env": {"FOO": "bar"},
                        "enabled": True,
                    },
                    "disabled-server": {
                        "command": "node",
                        "args": ["server.js"],
                        "enabled": False,
                    },
                }
            }, f)
            path = f.name

        try:
            servers = load_mcp_config(path)
            self.assertEqual(len(servers), 2)
            names = {s.name for s in servers}
            self.assertIn("test-server", names)
            self.assertIn("disabled-server", names)
            test_srv = next(s for s in servers if s.name == "test-server")
            self.assertEqual(test_srv.command, "python")
            self.assertEqual(test_srv.args, ["-m", "test_mcp"])
            self.assertEqual(test_srv.env, {"FOO": "bar"})
            self.assertTrue(test_srv.enabled)
        finally:
            os.unlink(path)

    def test_load_missing_file(self):
        servers = load_mcp_config("/nonexistent/mcp.json")
        self.assertEqual(servers, [])

    def test_load_invalid_json(self):
        with tempfile.NamedTemporaryFile(mode="w", suffix=".json", delete=False) as f:
            f.write("not json{{{")
            path = f.name
        try:
            servers = load_mcp_config(path)
            self.assertEqual(servers, [])
        finally:
            os.unlink(path)

    def test_save_and_reload(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            path = os.path.join(tmpdir, "mcp.json")
            servers = [
                MCPServerConfig(name="s1", command="cmd1", args=["a"], enabled=True),
                MCPServerConfig(name="s2", command="cmd2", args=["b", "c"], env={"X": "1"}, enabled=False),
            ]
            save_mcp_config(servers, path)

            loaded = load_mcp_config(path)
            self.assertEqual(len(loaded), 2)
            s1 = next(s for s in loaded if s.name == "s1")
            self.assertEqual(s1.command, "cmd1")
            s2 = next(s for s in loaded if s.name == "s2")
            self.assertFalse(s2.enabled)

    def test_skip_missing_command(self):
        with tempfile.NamedTemporaryFile(mode="w", suffix=".json", delete=False) as f:
            json.dump({
                "mcpServers": {
                    "no-cmd": {"args": ["x"]},
                }
            }, f)
            path = f.name
        try:
            servers = load_mcp_config(path)
            self.assertEqual(len(servers), 0)
        finally:
            os.unlink(path)


class TestMCPProtocol(unittest.TestCase):
    def test_encode_request(self):
        data = encode_jsonrpc_request("initialize", {"foo": "bar"}, id=42)
        text = data.decode("utf-8")
        self.assertIn("Content-Length:", text)
        self.assertIn('"method": "initialize"', text)
        self.assertIn('"id": 42', text)
        self.assertIn('"foo": "bar"', text)

    def test_decode_response(self):
        resp = decode_jsonrpc_response('{"jsonrpc":"2.0","id":1,"result":{"ok":true}}')
        self.assertEqual(resp["id"], 1)
        self.assertTrue(resp["result"]["ok"])

    def test_decode_invalid(self):
        resp = decode_jsonrpc_response("not json")
        self.assertIn("error", resp)

    def test_tool_schema_dataclass(self):
        ts = MCPToolSchema(name="test", description="A test tool", input_schema={
            "type": "object",
            "properties": {"x": {"type": "integer"}},
            "required": ["x"],
        })
        self.assertEqual(ts.name, "test")
        self.assertIn("x", ts.input_schema["properties"])


class TestMCPBridge(unittest.TestCase):
    def test_schema_to_parameters(self):
        schema = {
            "type": "object",
            "properties": {
                "address": {"type": "string", "description": "Memory address"},
                "length": {"type": "integer", "description": "Byte count"},
                "verbose": {"type": "boolean"},
            },
            "required": ["address"],
        }
        params = _mcp_schema_to_parameters(schema)
        self.assertEqual(len(params), 3)

        addr_param = next(p for p in params if p.name == "address")
        self.assertEqual(addr_param.type, "string")
        self.assertTrue(addr_param.required)
        self.assertEqual(addr_param.description, "Memory address")

        length_param = next(p for p in params if p.name == "length")
        self.assertFalse(length_param.required)

    def test_schema_empty(self):
        params = _mcp_schema_to_parameters({})
        self.assertEqual(params, [])

    def test_schema_array_type(self):
        """Handle JSON Schema type arrays like ["string", "null"]."""
        schema = {
            "type": "object",
            "properties": {
                "val": {"type": ["string", "null"]},
            },
        }
        params = _mcp_schema_to_parameters(schema)
        self.assertEqual(params[0].type, "string")

    def test_schema_with_default_and_enum(self):
        schema = {
            "type": "object",
            "properties": {
                "mode": {
                    "type": "string",
                    "enum": ["fast", "slow"],
                    "default": "fast",
                    "description": "Processing mode",
                },
            },
        }
        params = _mcp_schema_to_parameters(schema)
        self.assertEqual(len(params), 1)
        self.assertEqual(params[0].default, "fast")
        self.assertEqual(params[0].enum, ["fast", "slow"])

    def test_register_mcp_tools_with_mock_client(self):
        """Test register_mcp_tools with a mock MCPClient."""
        from unittest.mock import MagicMock

        mock_client = MagicMock(spec=MCPClient)
        mock_client.name = "test-server"
        mock_client.get_tools.return_value = [
            MCPToolSchema(
                name="read_data",
                description="Read data from address",
                input_schema={
                    "type": "object",
                    "properties": {
                        "address": {"type": "string", "description": "Address"},
                    },
                    "required": ["address"],
                },
            ),
            MCPToolSchema(
                name="write_data",
                description="Write data to address",
                input_schema={
                    "type": "object",
                    "properties": {
                        "address": {"type": "string"},
                        "data": {"type": "string"},
                    },
                    "required": ["address", "data"],
                },
            ),
        ]

        registry = ToolRegistry()
        count = register_mcp_tools(mock_client, registry)
        self.assertEqual(count, 2)

        names = registry.list_names()
        self.assertIn(f"{MCP_TOOL_PREFIX}test_server_read_data", names)
        self.assertIn(f"{MCP_TOOL_PREFIX}test_server_write_data", names)

        # Check tool definitions are properly formed
        read_tool = registry.get(f"{MCP_TOOL_PREFIX}test_server_read_data")
        self.assertIsNotNone(read_tool)
        self.assertIn("[MCP:test-server]", read_tool.description)
        self.assertEqual(read_tool.category, "mcp:test-server")

    def test_register_mcp_tools_custom_prefix(self):
        from unittest.mock import MagicMock

        mock_client = MagicMock(spec=MCPClient)
        mock_client.name = "srv"
        mock_client.get_tools.return_value = [
            MCPToolSchema(name="tool1", description="T1", input_schema={}),
        ]

        registry = ToolRegistry()
        count = register_mcp_tools(mock_client, registry, prefix="custom_")
        self.assertEqual(count, 1)
        self.assertIn("custom_tool1", registry.list_names())


class TestMCPManager(unittest.TestCase):
    def test_load_config_missing(self):
        mgr = MCPManager()
        count = mgr.load_config("/nonexistent/mcp.json")
        self.assertEqual(count, 0)

    def test_list_servers_empty(self):
        mgr = MCPManager()
        self.assertEqual(mgr.list_servers(), [])

    def test_stop_all_empty(self):
        mgr = MCPManager()
        mgr.stop_all()  # Should not raise


if __name__ == "__main__":
    unittest.main()

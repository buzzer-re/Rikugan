"""Binary Ninja's own MCP server: config, detection and prompt guidance."""

from __future__ import annotations

import unittest
from unittest.mock import MagicMock, patch

from rikugan.agent.prompts.binja import NATIVE_MCP_TOOL_PREFIX
from rikugan.agent.system_prompt import build_system_prompt
from rikugan.binja import native_mcp
from rikugan.mcp.config import MCPServerConfig, load_mcp_config, save_mcp_config


class TestServerConfig(unittest.TestCase):
    def test_a_url_server_needs_no_command(self):
        cfg = native_mcp.server_config("http://127.0.0.1:9009/mcp")
        self.assertTrue(cfg.is_remote)
        self.assertEqual(cfg.command, "")
        self.assertEqual(cfg.name, native_mcp.SERVER_NAME)

    def test_default_url_is_used_when_unset(self):
        self.assertEqual(native_mcp.server_config("").url, native_mcp.DEFAULT_URL)

    def test_tool_prefix_matches_what_the_bridge_registers(self):
        # The bridge sanitizes the server name the same way; the toggle removes
        # tools by this prefix, so a mismatch would leave them registered.
        self.assertEqual(native_mcp.tool_prefix(), NATIVE_MCP_TOOL_PREFIX)


class TestUrlRoundTrip(unittest.TestCase):
    def test_url_survives_save_and_load(self):
        import tempfile, os  # noqa: E401

        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "mcp.json")
            save_mcp_config([MCPServerConfig(name="bn", url="http://127.0.0.1:9009/mcp")], path)
            loaded = load_mcp_config(path)
            self.assertEqual(len(loaded), 1)
            self.assertEqual(loaded[0].url, "http://127.0.0.1:9009/mcp")
            self.assertTrue(loaded[0].is_remote)

    def test_a_server_with_neither_command_nor_url_is_skipped(self):
        import json, os, tempfile  # noqa: E401

        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "mcp.json")
            with open(path, "w", encoding="utf-8") as f:
                json.dump({"mcpServers": {"broken": {"args": []}}}, f)
            self.assertEqual(load_mcp_config(path), [])


class TestProbe(unittest.TestCase):
    """Detection is a real handshake, so a wrong URL is harmless."""

    def test_unreachable_server_reports_unavailable_without_raising(self):
        with patch("rikugan.binja.native_mcp.MCPClient") as client_cls:
            client_cls.return_value.start.side_effect = OSError("connection refused")
            result = native_mcp.probe("http://127.0.0.1:1/mcp")
        self.assertFalse(result.available)
        self.assertIn("connection refused", result.error)

    def test_a_reachable_server_reports_its_tool_count(self):
        client = MagicMock()
        client.get_tools.return_value = [object(), object(), object()]
        with patch("rikugan.binja.native_mcp.MCPClient", return_value=client):
            result = native_mcp.probe("http://127.0.0.1:9009/mcp")
        self.assertTrue(result.available)
        self.assertEqual(result.tool_count, 3)

    def test_the_probe_connection_is_always_closed(self):
        client = MagicMock()
        with patch("rikugan.binja.native_mcp.MCPClient", return_value=client):
            native_mcp.probe("http://127.0.0.1:9009/mcp")
        client.stop.assert_called_once()


class TestPromptGuidance(unittest.TestCase):
    def test_preference_is_stated_when_the_mcp_tools_are_present(self):
        prompt = build_system_prompt(
            host_name="Binary Ninja",
            tool_names=["decompile_function", f"{NATIVE_MCP_TOOL_PREFIX}get_function"],
        )
        self.assertIn("they are the only tools you have", prompt)

    def test_nothing_is_said_when_they_are_absent(self):
        prompt = build_system_prompt(host_name="Binary Ninja", tool_names=["decompile_function"])
        self.assertNotIn("they are the only tools you have", prompt)

    def test_the_agent_is_told_what_to_do_about_a_gap(self):
        # Rikugan's tools are switched off, so a missing capability has a
        # remedy the agent can name instead of something to guess around.
        prompt = build_system_prompt(
            host_name="Binary Ninja",
            tool_names=[f"{NATIVE_MCP_TOOL_PREFIX}get_function"],
        )
        self.assertIn("no equivalent here", prompt)
        self.assertIn("do not guess", prompt)


if __name__ == "__main__":
    unittest.main()

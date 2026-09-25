"""Tests for Anthropic provider: message formatting, normalization, error handling, auth."""

from __future__ import annotations

import os
import sys
import unittest
from types import SimpleNamespace

sys.path.insert(0, os.path.dirname(os.path.dirname(__file__)))
from tests.mocks.ida_mock import install_ida_mocks
install_ida_mocks()

from rikugan.core.types import Message, Role, ToolCall, ToolResult


def _reload_anthropic_provider_module() -> None:
    """Force the real provider module to load, not leftover test stubs."""
    sys.modules.pop("rikugan.providers.anthropic_provider", None)
    sys.modules.pop("rikugan.core.types", None)


def _make_provider():
    _reload_anthropic_provider_module()
    from rikugan.providers.anthropic_provider import AnthropicProvider
    return AnthropicProvider(api_key="test-key", model="claude-test")


class TestAnthropicFormatMessages(unittest.TestCase):
    """Test AnthropicProvider._format_messages."""

    def test_user_message(self):
        p = _make_provider()
        msgs = [Message(role=Role.USER, content="Hello")]
        result = p._format_messages(msgs)
        self.assertEqual(len(result), 1)
        self.assertEqual(result[0]["role"], "user")
        self.assertEqual(result[0]["content"], "Hello")

    def test_system_message_skipped(self):
        p = _make_provider()
        msgs = [
            Message(role=Role.SYSTEM, content="You are a helper"),
            Message(role=Role.USER, content="Hi"),
        ]
        result = p._format_messages(msgs)
        self.assertEqual(len(result), 1)
        self.assertEqual(result[0]["role"], "user")

    def test_assistant_with_tool_calls(self):
        p = _make_provider()
        msgs = [Message(
            role=Role.ASSISTANT,
            content="Let me check",
            tool_calls=[ToolCall(id="tc_1", name="get_info", arguments={"x": 1})],
        )]
        result = p._format_messages(msgs)
        self.assertEqual(len(result), 1)
        self.assertEqual(result[0]["role"], "assistant")
        content = result[0]["content"]
        self.assertIsInstance(content, list)
        self.assertEqual(content[0]["type"], "text")
        self.assertEqual(content[0]["text"], "Let me check")
        self.assertEqual(content[1]["type"], "tool_use")
        self.assertEqual(content[1]["id"], "tc_1")
        self.assertEqual(content[1]["name"], "get_info")
        self.assertEqual(content[1]["input"], {"x": 1})

    def test_tool_results_become_user_messages(self):
        """Anthropic maps tool results to user messages with tool_result content."""
        p = _make_provider()
        msgs = [Message(
            role=Role.TOOL,
            tool_results=[
                ToolResult(tool_call_id="tc_1", name="get_info", content="result1"),
                ToolResult(tool_call_id="tc_2", name="get_more", content="result2", is_error=True),
            ],
        )]
        result = p._format_messages(msgs)
        self.assertEqual(len(result), 2)
        for r in result:
            self.assertEqual(r["role"], "user")
            self.assertIsInstance(r["content"], list)
            self.assertEqual(r["content"][0]["type"], "tool_result")

        self.assertEqual(result[0]["content"][0]["tool_use_id"], "tc_1")
        self.assertFalse(result[0]["content"][0]["is_error"])
        self.assertEqual(result[1]["content"][0]["tool_use_id"], "tc_2")
        self.assertTrue(result[1]["content"][0]["is_error"])

    def test_full_conversation(self):
        p = _make_provider()
        msgs = [
            Message(role=Role.USER, content="Hello"),
            Message(role=Role.ASSISTANT, content="Hi there",
                    tool_calls=[ToolCall(id="tc_1", name="test", arguments={})]),
            Message(role=Role.TOOL,
                    tool_results=[ToolResult(tool_call_id="tc_1", name="test", content="done")]),
            Message(role=Role.ASSISTANT, content="All done"),
        ]
        result = p._format_messages(msgs)
        self.assertEqual(len(result), 4)
        self.assertEqual(result[0]["role"], "user")
        self.assertEqual(result[1]["role"], "assistant")
        self.assertEqual(result[2]["role"], "user")  # tool result
        self.assertEqual(result[3]["role"], "assistant")


class TestAnthropicFormatTools(unittest.TestCase):
    def test_converts_openai_format_to_anthropic(self):
        p = _make_provider()
        tools = [{
            "type": "function",
            "function": {
                "name": "test_tool",
                "description": "A test tool",
                "parameters": {"type": "object", "properties": {"x": {"type": "integer"}}},
            },
        }]
        result = p._format_tools(tools)
        self.assertEqual(len(result), 1)
        self.assertEqual(result[0]["name"], "test_tool")
        self.assertEqual(result[0]["description"], "A test tool")
        self.assertIn("properties", result[0]["input_schema"])


class TestAnthropicNormalizeResponse(unittest.TestCase):
    def test_text_response(self):
        p = _make_provider()
        response = SimpleNamespace(
            content=[SimpleNamespace(type="text", text="Hello world")],
            usage=SimpleNamespace(
                input_tokens=10, output_tokens=5,
                cache_read_input_tokens=0, cache_creation_input_tokens=0,
            ),
        )
        msg = p._normalize_response(response)
        self.assertEqual(msg.role, Role.ASSISTANT)
        self.assertEqual(msg.content, "Hello world")
        self.assertEqual(msg.tool_calls, [])
        self.assertEqual(msg.token_usage.total_tokens, 15)

    def test_tool_use_response(self):
        p = _make_provider()
        response = SimpleNamespace(
            content=[
                SimpleNamespace(type="text", text="Let me check"),
                SimpleNamespace(type="tool_use", id="tc_1", name="get_info", input={"key": "val"}),
            ],
            usage=SimpleNamespace(
                input_tokens=20, output_tokens=10,
                cache_read_input_tokens=5, cache_creation_input_tokens=2,
            ),
        )
        msg = p._normalize_response(response)
        self.assertEqual(msg.content, "Let me check")
        self.assertEqual(len(msg.tool_calls), 1)
        self.assertEqual(msg.tool_calls[0].name, "get_info")
        self.assertEqual(msg.tool_calls[0].arguments, {"key": "val"})
        self.assertEqual(msg.token_usage.cache_read_tokens, 5)
        self.assertEqual(msg.token_usage.cache_creation_tokens, 2)


class TestAnthropicHandleApiError(unittest.TestCase):
    def test_generic_error_raises_provider_error(self):
        from rikugan.core.errors import ProviderError
        p = _make_provider()
        with self.assertRaises(ProviderError):
            p._handle_api_error(RuntimeError("something broke"))

    def test_context_length_error(self):
        from rikugan.core.errors import ProviderError
        p = _make_provider()
        with self.assertRaises(ProviderError):
            p._handle_api_error(RuntimeError("context window exceeded token limit"))


class TestAnthropicAuthResolution(unittest.TestCase):
    """Test resolve_anthropic_auth priority order."""

    def test_explicit_api_key(self):
        _reload_anthropic_provider_module()
        from rikugan.providers.anthropic_provider import resolve_anthropic_auth
        token, auth_type = resolve_anthropic_auth("sk-ant-api03-test")
        self.assertEqual(token, "sk-ant-api03-test")
        self.assertEqual(auth_type, "api_key")

    def test_explicit_oauth_token(self):
        _reload_anthropic_provider_module()
        from rikugan.providers.anthropic_provider import resolve_anthropic_auth
        token, auth_type = resolve_anthropic_auth("sk-ant-oat01-test")
        self.assertEqual(token, "sk-ant-oat01-test")
        self.assertEqual(auth_type, "oauth")

    def test_env_var_api_key(self):
        _reload_anthropic_provider_module()
        from rikugan.providers.anthropic_provider import resolve_anthropic_auth
        old = os.environ.get("ANTHROPIC_API_KEY")
        try:
            os.environ["ANTHROPIC_API_KEY"] = "sk-from-env"
            token, auth_type = resolve_anthropic_auth("")
            self.assertEqual(token, "sk-from-env")
            self.assertEqual(auth_type, "api_key")
        finally:
            if old is None:
                os.environ.pop("ANTHROPIC_API_KEY", None)
            else:
                os.environ["ANTHROPIC_API_KEY"] = old

    def test_empty_returns_empty(self):
        _reload_anthropic_provider_module()
        from rikugan.providers.anthropic_provider import resolve_anthropic_auth
        old_api = os.environ.pop("ANTHROPIC_API_KEY", None)
        old_oauth = os.environ.pop("CLAUDE_CODE_OAUTH_TOKEN", None)
        try:
            token, auth_type = resolve_anthropic_auth("")
            # May find Keychain token on macOS, but should not crash
            self.assertIsInstance(token, str)
            self.assertIsInstance(auth_type, str)
        finally:
            if old_api is not None:
                os.environ["ANTHROPIC_API_KEY"] = old_api
            if old_oauth is not None:
                os.environ["CLAUDE_CODE_OAUTH_TOKEN"] = old_oauth

    def test_auth_status_with_key(self):
        _reload_anthropic_provider_module()
        from rikugan.providers.anthropic_provider import AnthropicProvider
        p = AnthropicProvider(api_key="sk-test", model="test")
        label, status = p.auth_status()
        self.assertEqual(status, "ok")

    def test_auth_status_oauth(self):
        _reload_anthropic_provider_module()
        from rikugan.providers.anthropic_provider import AnthropicProvider
        p = AnthropicProvider(api_key="sk-ant-oat01-test", model="test")
        label, status = p.auth_status()
        self.assertEqual(status, "ok")
        self.assertEqual(label, "OAuth")


if __name__ == "__main__":
    unittest.main()


class TestQuotaRejectionHint(unittest.TestCase):
    """A rejection about extra usage explains nothing on its own.

    On a subscription token the account is simply at its cap, and nothing
    Rikugan trims adds allowance — so say that, and name the way round it,
    rather than leaving it looking like a bug in Rikugan.
    """

    def _provider(self, auth_type="oauth", tool_payload=None):
        p = _make_provider()
        p._auth_type = auth_type
        if tool_payload is not None:
            p._last_tool_payload = tool_payload
        return p

    def test_it_names_the_cap_and_the_way_round_it(self):
        p = self._provider(tool_payload=(137, 60000))
        hint = p._quota_rejection_hint("You're out of extra usage. Add more at claude.ai/settings/usage")
        self.assertIn("subscription", hint)
        self.assertIn("API key", hint)

    def test_a_large_tool_set_is_mentioned_as_a_factor(self):
        p = self._provider(tool_payload=(137, 60000))
        hint = p._quota_rejection_hint("You're out of extra usage.")
        self.assertIn("137 tools", hint)
        # But never as the cause: trimming does not add allowance.
        self.assertIn("does not raise it", hint)

    def test_an_api_key_user_gets_no_subscription_advice(self):
        # There is no extra-usage cap on API credits, so this rejection means
        # something else and the advice would be wrong.
        p = self._provider(auth_type="api_key", tool_payload=(137, 60000))
        self.assertEqual(p._quota_rejection_hint("You're out of extra usage."), "")

    def test_an_unrelated_rejection_gets_no_hint(self):
        p = self._provider(tool_payload=(137, 60000))
        self.assertEqual(p._quota_rejection_hint("invalid model name"), "")

    def test_a_small_tool_set_still_gets_the_cap_explanation(self):
        p = self._provider(tool_payload=(40, 15000))
        hint = p._quota_rejection_hint("You're out of extra usage.")
        self.assertIn("API key", hint)
        self.assertNotIn("40 tools", hint)

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

    def test_tool_results_become_one_user_message(self):
        """All results from one assistant turn belong in a single user message.

        Emitting one message per result put two user messages back to back,
        which breaks the alternation the API requires — and the prompt asks
        the model to batch tool calls, so that was every parallel turn.
        """
        p = _make_provider()
        msgs = [Message(
            role=Role.TOOL,
            tool_results=[
                ToolResult(tool_call_id="tc_1", name="get_info", content="result1"),
                ToolResult(tool_call_id="tc_2", name="get_more", content="result2", is_error=True),
            ],
        )]
        result = p._format_messages(msgs)
        self.assertEqual(len(result), 1)
        self.assertEqual(result[0]["role"], "user")
        blocks = result[0]["content"]
        self.assertEqual([b["type"] for b in blocks], ["tool_result", "tool_result"])

        self.assertEqual(blocks[0]["tool_use_id"], "tc_1")
        self.assertFalse(blocks[0]["is_error"])
        self.assertEqual(blocks[1]["tool_use_id"], "tc_2")
        self.assertTrue(blocks[1]["is_error"])

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


class TestFailedRequestDump(unittest.TestCase):
    """An API rejection names a symptom; the request settles the cause."""

    def _provider_with_request(self, tools=None):
        p = _make_provider()
        p._last_request = {
            "model": "claude-test",
            "max_tokens": 8192,
            "system": [{"type": "text", "text": "prompt", "cache_control": {"type": "ephemeral"}}],
            "messages": [{"role": "user", "content": [{"type": "text", "text": "hi"}]}],
            "tools": tools if tools is not None else [{"name": "t", "input_schema": {"type": "object"}}],
        }
        return p

    def test_it_writes_the_tools_verbatim(self):
        import json as _json
        import tempfile

        tools = [{"name": "mcp_x_a", "description": "d", "input_schema": {"type": "object", "properties": {}}}]
        p = self._provider_with_request(tools)
        with tempfile.TemporaryDirectory() as tmp:
            path = p.dump_failed_request("boom", directory=tmp)
            self.assertTrue(path)
            with open(path, encoding="utf-8") as f:
                data = _json.load(f)
        # Tools are what usually differ between a request that works and one
        # that does not, so they are not summarised.
        self.assertEqual(data["tools"], tools)
        self.assertEqual(data["tool_count"], 1)
        self.assertEqual(data["error"], "boom")

    def test_message_bodies_are_summarised_not_copied(self):
        import tempfile

        p = self._provider_with_request()
        p._last_request["messages"] = [
            {"role": "user", "content": [{"type": "text", "text": "SECRET-BINARY-STRING" * 100}]}
        ]
        with tempfile.TemporaryDirectory() as tmp:
            path = p.dump_failed_request("boom", directory=tmp)
            with open(path, encoding="utf-8") as f:
                raw = f.read()
        self.assertNotIn("SECRET-BINARY-STRING", raw)
        self.assertIn('"blocks"', raw)

    def test_nothing_is_written_when_no_request_was_built(self):
        p = _make_provider()
        self.assertEqual(p.dump_failed_request("boom"), "")


class TestCacheMarks(unittest.TestCase):
    def test_it_counts_breakpoints_in_a_block_list(self):
        from rikugan.providers.anthropic_provider import _cache_marks

        content = [{"type": "text"}, {"type": "text", "cache_control": {"type": "ephemeral"}}]
        self.assertEqual(_cache_marks(content), 1)

    def test_a_plain_string_carries_none(self):
        from rikugan.providers.anthropic_provider import _cache_marks

        self.assertEqual(_cache_marks("hello"), 0)


class TestRoleAlternation(unittest.TestCase):
    """The API requires alternating roles; failed turns break that.

    A turn that errors leaves its user message in the history with no reply,
    so a run of failures builds a run of user messages and every later
    request carries a malformed conversation.
    """

    def _formatted(self, messages):
        return _make_provider()._format_messages(messages)

    def test_a_run_of_user_messages_is_folded_into_one(self):
        out = self._formatted([Message(role=Role.USER, content=f"hello {i}") for i in range(6)])
        self.assertEqual(len(out), 1)
        self.assertEqual(out[0]["role"], "user")
        # Nothing the user typed is discarded.
        for i in range(6):
            self.assertIn(f"hello {i}", out[0]["content"])

    def test_alternating_history_is_untouched(self):
        out = self._formatted(
            [
                Message(role=Role.USER, content="a"),
                Message(role=Role.ASSISTANT, content="b"),
                Message(role=Role.USER, content="c"),
            ]
        )
        self.assertEqual([m["role"] for m in out], ["user", "assistant", "user"])

    def test_roles_always_alternate_after_folding(self):
        out = self._formatted(
            [
                Message(role=Role.USER, content="hell"),
                Message(role=Role.USER, content="hello"),
                Message(role=Role.ASSISTANT, content="hi"),
                Message(role=Role.USER, content="hello"),
                Message(role=Role.USER, content="hello"),
                Message(role=Role.USER, content="hello"),
            ]
        )
        roles = [m["role"] for m in out]
        self.assertEqual(roles, ["user", "assistant", "user"])
        for a, b in zip(roles, roles[1:], strict=False):
            self.assertNotEqual(a, b)

    def test_block_and_string_bodies_join_without_loss(self):
        out = self._formatted(
            [
                Message(role=Role.ASSISTANT, content="text", tool_calls=[ToolCall(id="1", name="t", arguments={})]),
                Message(role=Role.ASSISTANT, content="more"),
            ]
        )
        self.assertEqual(len(out), 1)
        kinds = [b["type"] for b in out[0]["content"]]
        self.assertIn("tool_use", kinds)
        self.assertIn("text", kinds)


class TestModelLimits(unittest.TestCase):
    """An unknown model fell through to 200K/8192, wrong in both directions."""

    def _limits(self, model):
        from rikugan.providers.anthropic_provider import AnthropicProvider

        return AnthropicProvider._model_limits(model)

    def test_the_claude_5_family_gets_its_real_limits(self):
        for model in ("claude-sonnet-5", "claude-opus-5", "claude-opus-5-5", "claude-fable-5-1"):
            with self.subTest(model=model):
                self.assertEqual(self._limits(model), (1000000, 128000))

    def test_haiku_4_5_keeps_its_smaller_window(self):
        self.assertEqual(self._limits("claude-haiku-4-5")[0], 200000)

    def test_an_unknown_model_stays_conservative(self):
        # Guessing high would have the context manager overrun the window.
        self.assertEqual(self._limits("claude-something-new"), (200000, 8192))

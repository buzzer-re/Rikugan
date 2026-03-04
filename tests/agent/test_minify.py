"""Tests for rikugan.agent.minify."""

from __future__ import annotations

from rikugan.agent.minify import minify_messages, minify_text
from rikugan.core.types import Message, ToolResult


class TestMinifyText:
    def test_empty_string(self):
        assert minify_text("") == ""

    def test_no_change_needed(self):
        assert minify_text("hello world") == "hello world"

    def test_strips_trailing_whitespace(self):
        assert minify_text("line   \nother") == "line\nother"

    def test_collapses_triple_blank_lines(self):
        result = minify_text("a\n\n\n\nb")
        assert result == "a\n\nb"

    def test_preserves_double_blank_line(self):
        result = minify_text("a\n\nb")
        assert result == "a\n\nb"

    def test_strips_leading_trailing_blank_lines(self):
        result = minify_text("\n\nhello\n\n")
        assert result == "hello"

    def test_preserves_indentation(self):
        code = "def foo():\n    return 1"
        assert minify_text(code) == code

    def test_multi_blank_collapse(self):
        result = minify_text("a\n\n\n\n\n\nb")
        assert result == "a\n\nb"


class TestMinifyMessages:
    def test_empty_list(self):
        assert minify_messages([]) == []

    def test_minifies_content(self):
        msg = Message(role="user", content="hello   \n\n\n\nworld")
        result = minify_messages([msg])
        assert result[0].content == "hello\n\nworld"

    def test_original_message_unchanged(self):
        msg = Message(role="user", content="hello   ")
        minify_messages([msg])
        assert msg.content == "hello   "

    def test_minifies_tool_results(self):
        tr = ToolResult(tool_call_id="1", name="t", content="result   \n\n\n\n", is_error=False)
        msg = Message(role="tool", content="", tool_results=[tr])
        result = minify_messages([msg])
        assert result[0].tool_results[0].content == "result"

    def test_none_content_unchanged(self):
        msg = Message(role="assistant", content=None)
        result = minify_messages([msg])
        assert result[0].content is None

    def test_preserves_role(self):
        msg = Message(role="assistant", content="hello")
        result = minify_messages([msg])
        assert result[0].role == "assistant"

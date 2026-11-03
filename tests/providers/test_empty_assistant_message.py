"""An empty reply must not poison the rest of the chat (issue #72).

A model reply can arrive with no text and no tool calls — a refusal carries
`stop_reason: "refusal"` and empty content, and an empty `end_turn` looks the
same. Stored, it goes back out with every later request, and the API rejects a
history holding it:

    messages.1: all messages must have non-empty content except for the
    optional final assistant message

so every later message in that chat fails until a new one is started.
"""

from __future__ import annotations

import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(__file__)))
from tests.mocks.ida_mock import install_ida_mocks

install_ida_mocks()

# Other tests stub these; this file needs the real ones.
for _mod_name in ("rikugan.core.types", "rikugan.core.config", "rikugan.state.session"):
    sys.modules.pop(_mod_name, None)

from rikugan.agent.modes.turn_helpers import execute_single_turn  # noqa: E402
from rikugan.core.types import Message, Role, ToolCall  # noqa: E402
from rikugan.providers.anthropic_provider import AnthropicProvider  # noqa: E402
from rikugan.providers.openai_provider import OpenAIProvider  # noqa: E402
from rikugan.state.session import SessionState  # noqa: E402

_EMPTY_REPLY = Message(role=Role.ASSISTANT, content="")


def _history_with_empty_reply():
    return [
        Message(role=Role.USER, content="hi"),
        _EMPTY_REPLY,
        Message(role=Role.USER, content="are you there?"),
    ]


def _tool_only_reply():
    return [
        Message(
            role=Role.ASSISTANT,
            content="",
            tool_calls=[ToolCall(id="tc_1", name="get_binary_info", arguments={})],
        )
    ]


class TestAnthropicDropsEmptyReplies(unittest.TestCase):
    def setUp(self):
        self.provider = AnthropicProvider(api_key="test", model="claude-sonnet-5")

    def test_an_empty_reply_is_left_out(self):
        formatted = self.provider._format_messages(_history_with_empty_reply())
        self.assertNotIn("assistant", [m["role"] for m in formatted])

    def test_no_message_goes_out_empty(self):
        # This is what the API rejects by name.
        for message in self.provider._format_messages(_history_with_empty_reply()):
            with self.subTest(role=message["role"]):
                self.assertTrue(message["content"])

    def test_a_reply_that_only_calls_a_tool_is_kept(self):
        # It says nothing, but the tool call is the turn — dropping it would
        # orphan the tool result that follows.
        formatted = self.provider._format_messages(_tool_only_reply())
        self.assertEqual(len(formatted), 1)
        self.assertEqual(formatted[0]["content"][0]["type"], "tool_use")

    def test_a_reply_with_text_is_kept(self):
        formatted = self.provider._format_messages(
            [Message(role=Role.ASSISTANT, content="here is what I found")]
        )
        self.assertEqual(len(formatted), 1)


class TestOpenAIDropsEmptyReplies(unittest.TestCase):
    def setUp(self):
        self.provider = OpenAIProvider(api_key="test", model="gpt-4o")

    def test_an_empty_reply_is_left_out(self):
        formatted = self.provider._format_messages(_history_with_empty_reply())
        self.assertNotIn("assistant", [m["role"] for m in formatted])

    def test_no_assistant_turn_goes_out_without_content_or_tool_calls(self):
        for message in self.provider._format_messages(_history_with_empty_reply()):
            if message["role"] != "assistant":
                continue
            with self.subTest(message=message):
                self.assertTrue(message.get("content") or message.get("tool_calls"))

    def test_a_reply_that_only_calls_a_tool_is_kept(self):
        formatted = self.provider._format_messages(_tool_only_reply())
        self.assertEqual(len(formatted), 1)
        self.assertTrue(formatted[0]["tool_calls"])


if __name__ == "__main__":
    unittest.main()


class TestAnEmptyReplyIsNeverStored(unittest.TestCase):
    """The real fix: it does not enter the history at all.

    Skipping it when formatting saves chats that already hold one, but a reply
    that says nothing has no business in the transcript either — it is not
    something the user said, saw, or can act on.
    """

    def _run_turn(self, text, tool_calls):
        """Drive execute_single_turn with a stream that returned *text*."""
        from unittest.mock import MagicMock

        loop = MagicMock()
        loop.session = SessionState()

        def _stream(_system, _tools):
            return (text, tool_calls, None, None, "end_turn")
            yield  # pragma: no cover - makes this a generator

        def _execute(_calls):
            # A generator that *returns* its results, as the real one does.
            return []
            yield  # pragma: no cover

        loop._stream_llm_turn = _stream
        loop._execute_tool_calls = _execute
        list(execute_single_turn(loop, "sys", []))
        return loop.session.messages

    def test_an_empty_reply_leaves_the_history_untouched(self):
        self.assertEqual(self._run_turn("", []), [])

    def test_a_reply_with_text_is_stored(self):
        stored = self._run_turn("found the parser", [])
        self.assertEqual(len(stored), 1)
        self.assertEqual(stored[0].content, "found the parser")

    def test_a_reply_that_only_calls_a_tool_is_stored(self):
        # Dropping it would orphan the tool result that follows.
        stored = self._run_turn("", [ToolCall(id="tc_1", name="t", arguments={})])
        self.assertTrue(stored)
        self.assertTrue(stored[0].tool_calls)

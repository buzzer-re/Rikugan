"""A turn must always end, however long the provider asks us to wait.

A rate limit can come back with a retry-after measured in hours. Sleeping it
out inside the turn leaves the panel saying "working" while everything the
user sends queues behind a turn that will not finish today — which is how a
queued message ends up never delivered.
"""

from __future__ import annotations

import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(__file__)))
from tests.mocks.ida_mock import install_ida_mocks

install_ida_mocks()

# Other UI tests replace these in sys.modules; this file needs the real ones.
for _mod_name in ("rikugan.core.types", "rikugan.core.config", "rikugan.state.session"):
    sys.modules.pop(_mod_name, None)

import time  # noqa: E402
from unittest.mock import MagicMock  # noqa: E402

from rikugan.agent.loop import AgentLoop  # noqa: E402
from rikugan.core.config import RikuganConfig  # noqa: E402
from rikugan.core.errors import RateLimitError  # noqa: E402
from rikugan.core.types import Message, Role  # noqa: E402
from rikugan.state.session import SessionState  # noqa: E402

# What the provider actually asked for in the report that prompted this: 3.7h.
_ABSURD_WAIT = 13338.0


class _Loop(AgentLoop):
    """An AgentLoop whose LLM call always rate-limits."""

    def __init__(self, retry_after):
        super().__init__(
            provider=MagicMock(),
            tool_registry=MagicMock(),
            config=RikuganConfig(),
            session=SessionState(),
        )
        self._retry_after = retry_after
        self.attempts = 0

    def _stream_llm_turn_inner(self, system_prompt, tools_schema):
        self.attempts += 1
        raise RateLimitError("slow down", provider="test", retry_after=self._retry_after)
        yield  # pragma: no cover - makes this a generator


class TestRetryBackoffCeiling(unittest.TestCase):
    def _drive(self, loop):
        started = time.monotonic()
        with self.assertRaises(RateLimitError) as caught:
            list(loop._stream_llm_turn("sys", []))
        return caught.exception, time.monotonic() - started

    def test_an_hours_long_wait_ends_the_turn_instead_of_sleeping(self):
        loop = _Loop(_ABSURD_WAIT)
        error, elapsed = self._drive(loop)
        # It must not have slept: the turn has to end so the queue behind it
        # can drain.
        self.assertLess(elapsed, 5.0)
        self.assertEqual(loop.attempts, 1)
        self.assertIn("minutes", str(error))

    def test_a_short_wait_is_still_retried(self):
        # Capping must not turn every rate limit into an immediate failure.
        loop = _Loop(0.01)
        _, elapsed = self._drive(loop)
        self.assertGreater(loop.attempts, 1)
        self.assertLess(elapsed, 10.0)


if __name__ == "__main__":
    unittest.main()


class TestExplorationContext(unittest.TestCase):
    """A goal that refers back to the conversation needs the conversation.

    Exploration Phase 1 runs in an isolated context on purpose — the raw
    decompilation must not follow it in. But a follow-up like "continue" is
    only meaningful against what came before, and handed it alone the
    subagent can do no better than say it does not know what was meant.
    """

    def _loop_with(self, messages):
        loop = MagicMock()
        loop.session.messages = [Message(role=r, content=c) for r, c in messages]
        return loop

    def test_recent_turns_are_carried_in(self):
        from rikugan.agent.modes.exploration import _recent_context

        ctx = _recent_context(
            self._loop_with([(Role.USER, "map the pack file reader"), (Role.ASSISTANT, "Found PackFileSystem")])
        )
        self.assertIn("map the pack file reader", ctx)
        self.assertIn("PackFileSystem", ctx)

    def test_an_empty_conversation_carries_nothing(self):
        from rikugan.agent.modes.exploration import _recent_context

        self.assertEqual(_recent_context(self._loop_with([])), "")

    def test_long_turns_are_trimmed(self):
        from rikugan.agent.modes.exploration import _recent_context

        ctx = _recent_context(self._loop_with([(Role.ASSISTANT, "x" * 5000)]))
        # The point of the isolated context is not to drag a decompile in.
        self.assertLess(len(ctx), 1200)

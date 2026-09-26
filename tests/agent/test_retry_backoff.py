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

import time  # noqa: E402
from unittest.mock import MagicMock  # noqa: E402

from rikugan.agent.loop import AgentLoop  # noqa: E402
from rikugan.core.config import RikuganConfig  # noqa: E402
from rikugan.core.errors import RateLimitError  # noqa: E402
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

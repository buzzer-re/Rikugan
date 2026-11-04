"""Context windows come from the API, not from a table we keep editing.

A hardcoded table is a snapshot of a list that changes: a model released
after the build falls through to the conservative default, and a wrong
window either compacts a conversation that had room or overruns one that
did not. The Models API reports max_input_tokens and max_tokens, so those
are what we use, with the table only as an offline fallback.
"""

from __future__ import annotations

import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(__file__)))
from tests.mocks.ida_mock import install_ida_mocks

install_ida_mocks()

for _mod_name in ("rikugan.core.types", "rikugan.core.config"):
    sys.modules.pop(_mod_name, None)

from rikugan.providers.anthropic_provider import AnthropicProvider  # noqa: E402

_MEGATOKEN = 1_000_000


class _ApiModel:
    """What client.models.list() hands back."""

    def __init__(self, model_id, max_input_tokens=None, max_tokens=None):
        self.id = model_id
        self.display_name = model_id
        if max_input_tokens is not None:
            self.max_input_tokens = max_input_tokens
        if max_tokens is not None:
            self.max_tokens = max_tokens


class TestLimitsLearnedFromTheApi(unittest.TestCase):
    def setUp(self):
        AnthropicProvider._LEARNED_LIMITS.clear()

    tearDown = setUp

    def test_the_api_value_is_used(self):
        limits = AnthropicProvider._learn_limits(
            _ApiModel("claude-future-9", max_input_tokens=2_000_000, max_tokens=200_000)
        )
        self.assertEqual(limits, (2_000_000, 200_000))

    def test_a_model_released_after_this_build_is_measured_correctly(self):
        # The whole point: no table edit needed for a model we have never heard of.
        before = AnthropicProvider._model_limits("claude-future-9")
        AnthropicProvider._learn_limits(
            _ApiModel("claude-future-9", max_input_tokens=2_000_000, max_tokens=200_000)
        )
        after = AnthropicProvider._model_limits("claude-future-9")
        self.assertEqual(before, (200_000, 8192))
        self.assertEqual(after, (2_000_000, 200_000))

    def test_an_older_api_that_reports_no_limits_keeps_the_table_value(self):
        limits = AnthropicProvider._learn_limits(_ApiModel("claude-opus-4-6"))
        self.assertEqual(limits, (_MEGATOKEN, 128_000))

    def test_a_nonsense_value_does_not_replace_a_known_one(self):
        # A zero or negative window would make the context manager compact
        # on every turn.
        limits = AnthropicProvider._learn_limits(
            _ApiModel("claude-opus-4-6", max_input_tokens=0, max_tokens=-1)
        )
        self.assertEqual(limits, (_MEGATOKEN, 128_000))


class TestFallbackTable(unittest.TestCase):
    """Only reached offline, or before the model list has been fetched."""

    def setUp(self):
        AnthropicProvider._LEARNED_LIMITS.clear()

    tearDown = setUp

    def test_the_current_families_hold_a_megatoken(self):
        for model in (
            "claude-opus-5-5",
            "claude-opus-5",
            "claude-sonnet-5-5",
            "claude-sonnet-5",
            "claude-haiku-5-5",
            "claude-opus-4-8",
            "claude-opus-4-7",
            "claude-opus-4-6",
            "claude-sonnet-4-6",
            "claude-fable-5-1",
        ):
            with self.subTest(model=model):
                self.assertEqual(AnthropicProvider._model_limits(model), (_MEGATOKEN, 128_000))

    def test_haiku_4_5_keeps_its_smaller_window(self):
        self.assertEqual(AnthropicProvider._model_limits("claude-haiku-4-5")[0], 200_000)

    def test_an_unknown_model_stays_conservative(self):
        # Guessing high would have the context manager overrun the window.
        self.assertEqual(AnthropicProvider._model_limits("claude-made-up"), (200_000, 8192))


if __name__ == "__main__":
    unittest.main()

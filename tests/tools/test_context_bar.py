"""Tests for rikugan.ui.context_bar — pure logic in set_tokens, set_function, _function_name_at."""

from __future__ import annotations

import sys
import types
import unittest
from unittest.mock import MagicMock, patch

from tests.qt_stubs import ensure_pyside6_stubs

ensure_pyside6_stubs()

sys.modules.setdefault("binaryninja", types.ModuleType("binaryninja"))

from rikugan.ui.context_bar import (  # noqa: E402
    ContextBar,
    _function_name_at,
    elide_middle,
    format_token_count,
    shorten_model_name,
)


# ---------------------------------------------------------------------------
# Helper: create a ContextBar without calling __init__
# ---------------------------------------------------------------------------


def _make_bar() -> ContextBar:
    bar = object.__new__(ContextBar)
    bar._stopped = False
    # The bar shows bare values; there are no caption labels to pair them with.
    bar._address_label = MagicMock()
    bar._function_label = MagicMock()
    bar._model_label = MagicMock()
    bar._tokens_label = MagicMock()
    bar._dot = MagicMock()
    bar._timer = MagicMock()
    return bar


# ---------------------------------------------------------------------------
# set_tokens
# ---------------------------------------------------------------------------


class TestSetTokens(unittest.TestCase):
    def test_small_count_shown_as_int(self):
        bar = _make_bar()
        bar.set_tokens(500)
        bar._tokens_label.setText.assert_called_once_with("500")

    def test_large_count_shown_in_k(self):
        bar = _make_bar()
        bar.set_tokens(2500)
        bar._tokens_label.setText.assert_called_once_with("2.5k")

    def test_exactly_1000_shown_in_k(self):
        bar = _make_bar()
        bar.set_tokens(1000)
        bar._tokens_label.setText.assert_called_once_with("1.0k")

    def test_context_window_shows_percentage(self):
        bar = _make_bar()
        bar.set_tokens(500, context_window=1000)
        call_arg = bar._tokens_label.setText.call_args[0][0]
        self.assertIn("500", call_arg)
        self.assertIn("50%", call_arg)

    def test_context_window_percentage_capped_at_100(self):
        bar = _make_bar()
        bar.set_tokens(2000, context_window=1000)
        call_arg = bar._tokens_label.setText.call_args[0][0]
        self.assertIn("100%", call_arg)

    def test_no_percentage_when_context_window_zero(self):
        bar = _make_bar()
        bar.set_tokens(100, context_window=0)
        call_arg = bar._tokens_label.setText.call_args[0][0]
        self.assertNotIn("%", call_arg)


# ---------------------------------------------------------------------------
# set_function
# ---------------------------------------------------------------------------


class TestSetFunction(unittest.TestCase):
    def test_short_name_passed_through(self):
        bar = _make_bar()
        bar.set_function("my_func")
        bar._function_label.setText.assert_called_once_with("my_func")

    def test_long_name_elided_in_the_middle(self):
        bar = _make_bar()
        long_name = "a" * 35
        bar.set_function(long_name)
        call_arg = bar._function_label.setText.call_args[0][0]
        self.assertIn("\u2026", call_arg)
        self.assertEqual(len(call_arg), 22)

    def test_full_name_kept_in_the_tooltip(self):
        bar = _make_bar()
        long_name = "sub_" + "f" * 40
        bar.set_function(long_name)
        bar._function_label.setToolTip.assert_called_once_with(long_name)

    def test_exactly_22_chars_not_elided(self):
        bar = _make_bar()
        name = "b" * 22
        bar.set_function(name)
        bar._function_label.setText.assert_called_once_with(name)

    def test_23_chars_elided(self):
        bar = _make_bar()
        name = "c" * 23
        bar.set_function(name)
        call_arg = bar._function_label.setText.call_args[0][0]
        self.assertIn("\u2026", call_arg)


# ---------------------------------------------------------------------------
# set_address / set_model
# ---------------------------------------------------------------------------


class TestSetAddress(unittest.TestCase):
    def test_set_address_updates_label(self):
        bar = _make_bar()
        bar.set_address("0x1000")
        bar._address_label.setText.assert_called_once_with("0x1000")


class TestSetModel(unittest.TestCase):
    def test_set_model_shortens_the_id(self):
        bar = _make_bar()
        bar.set_model("claude-opus-4")
        bar._model_label.setText.assert_called_once_with("opus-4")

    def test_set_model_keeps_full_id_in_tooltip(self):
        bar = _make_bar()
        bar.set_model("claude-sonnet-4-5-20250929")
        bar._model_label.setToolTip.assert_called_once_with("claude-sonnet-4-5-20250929")


class TestShortenModelName(unittest.TestCase):
    def test_strips_vendor_prefix_and_date(self):
        self.assertEqual(shorten_model_name("claude-sonnet-4-5-20250929"), "sonnet-4.5")

    def test_collapses_version_tail(self):
        self.assertEqual(shorten_model_name("claude-opus-4-1"), "opus-4.1")

    def test_leaves_short_third_party_ids_alone(self):
        self.assertEqual(shorten_model_name("gpt-4o"), "gpt-4o")

    def test_empty_model_shows_a_dash(self):
        self.assertEqual(shorten_model_name(""), "\u2014")

    def test_over_long_id_is_elided_from_the_left(self):
        result = shorten_model_name("some-extremely-long-local-model-name")
        self.assertEqual(len(result), 18)
        self.assertTrue(result.startswith("\u2026"))


class TestElideMiddle(unittest.TestCase):
    def test_short_text_untouched(self):
        self.assertEqual(elide_middle("abc", 10), "abc")

    def test_keeps_both_ends(self):
        result = elide_middle("start_middle_end", 9)
        self.assertEqual(len(result), 9)
        self.assertTrue(result.startswith("sta"))
        self.assertTrue(result.endswith("end"))


class TestFormatTokenCount(unittest.TestCase):
    def test_percentage_appended_when_window_known(self):
        self.assertEqual(format_token_count(500, 1000), "500 (50%)")

    def test_thousands_use_k(self):
        self.assertEqual(format_token_count(24100), "24.1k")


class TestSetState(unittest.TestCase):
    def test_state_change_repolishes_the_dot(self):
        bar = _make_bar()
        bar._dot.property.return_value = "idle"
        bar.set_state("running")
        bar._dot.setProperty.assert_called_once_with("state", "running")

    def test_same_state_is_a_noop(self):
        bar = _make_bar()
        bar._dot.property.return_value = "running"
        bar.set_state("running")
        bar._dot.setProperty.assert_not_called()


# ---------------------------------------------------------------------------
# _function_name_at — standalone host
# ---------------------------------------------------------------------------


class TestFunctionNameAt(unittest.TestCase):
    def test_returns_none_in_standalone_mode(self):
        with (
            patch("rikugan.ui.context_bar.is_ida", return_value=False),
            patch("rikugan.ui.context_bar.is_binary_ninja", return_value=False),
        ):
            result = _function_name_at(0x1000)
            self.assertIsNone(result)

    def test_returns_none_when_binary_ninja_no_bv(self):
        with (
            patch("rikugan.ui.context_bar.is_ida", return_value=False),
            patch("rikugan.ui.context_bar.is_binary_ninja", return_value=True),
            patch("rikugan.ui.context_bar.get_binary_ninja_view", return_value=None),
        ):
            result = _function_name_at(0x1000)
            self.assertIsNone(result)

    def test_returns_function_name_via_get_function_at(self):
        mock_bv = MagicMock()
        mock_func = MagicMock()
        mock_func.name = "target_fn"
        mock_bv.get_function_at.return_value = mock_func
        with (
            patch("rikugan.ui.context_bar.is_ida", return_value=False),
            patch("rikugan.ui.context_bar.is_binary_ninja", return_value=True),
            patch("rikugan.ui.context_bar.get_binary_ninja_view", return_value=mock_bv),
        ):
            result = _function_name_at(0x1000)
            self.assertEqual(result, "target_fn")

    def test_returns_none_when_get_function_at_returns_none(self):
        mock_bv = MagicMock()
        mock_bv.get_function_at.return_value = None
        mock_bv.get_functions_containing.return_value = []
        with (
            patch("rikugan.ui.context_bar.is_ida", return_value=False),
            patch("rikugan.ui.context_bar.is_binary_ninja", return_value=True),
            patch("rikugan.ui.context_bar.get_binary_ninja_view", return_value=mock_bv),
        ):
            result = _function_name_at(0x1000)
            self.assertIsNone(result)


# ---------------------------------------------------------------------------
# stop
# ---------------------------------------------------------------------------


class TestContextBarStop(unittest.TestCase):
    def test_stop_sets_stopped_flag(self):
        bar = _make_bar()
        bar.stop()
        self.assertTrue(bar._stopped)

    def test_stop_calls_timer_stop(self):
        bar = _make_bar()
        bar.stop()
        bar._timer.stop.assert_called_once()


if __name__ == "__main__":
    unittest.main()

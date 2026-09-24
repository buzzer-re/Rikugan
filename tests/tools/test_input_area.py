"""Tests for rikugan.ui.input_area — pure logic helpers."""

from __future__ import annotations

import sys
import unittest
from unittest.mock import MagicMock, patch

from tests.qt_stubs import ensure_pyside6_stubs

ensure_pyside6_stubs()

sys.modules.pop("rikugan.ui.input_area", None)

from rikugan.ui.input_area import InputArea, _SkillPopup  # noqa: E402


# ---------------------------------------------------------------------------
# _SkillPopup — pure list logic
# ---------------------------------------------------------------------------


def _make_popup() -> _SkillPopup:
    popup = object.__new__(_SkillPopup)
    popup._slugs = []
    popup._selected_idx = 0
    popup._items = []
    return popup


class TestSkillPopupCurrentSlug(unittest.TestCase):
    def test_empty_list_returns_none(self):
        popup = _make_popup()
        self.assertIsNone(popup.current_slug())

    def test_returns_first_item_by_default(self):
        popup = _make_popup()
        popup._slugs = ["plan", "modify", "explore"]
        self.assertEqual(popup.current_slug(), "plan")

    def test_returns_selected_item(self):
        popup = _make_popup()
        popup._slugs = ["a", "b", "c"]
        popup._selected_idx = 2
        self.assertEqual(popup.current_slug(), "c")

    def test_out_of_bounds_idx_returns_none(self):
        popup = _make_popup()
        popup._slugs = ["a"]
        popup._selected_idx = 5
        self.assertIsNone(popup.current_slug())


class TestSkillPopupIsEmpty(unittest.TestCase):
    def test_empty_list(self):
        popup = _make_popup()
        self.assertTrue(popup.is_empty())

    def test_non_empty_list(self):
        popup = _make_popup()
        popup._slugs = ["plan"]
        self.assertFalse(popup.is_empty())


class TestSkillPopupMoveSelection(unittest.TestCase):
    def test_wraps_forward(self):
        popup = _make_popup()
        popup._slugs = ["a", "b", "c"]
        popup._selected_idx = 2
        popup._update_highlight = MagicMock()
        popup.move_selection(1)
        self.assertEqual(popup._selected_idx, 0)

    def test_wraps_backward(self):
        popup = _make_popup()
        popup._slugs = ["a", "b", "c"]
        popup._selected_idx = 0
        popup._update_highlight = MagicMock()
        popup.move_selection(-1)
        self.assertEqual(popup._selected_idx, 2)

    def test_no_move_when_empty(self):
        popup = _make_popup()
        popup._selected_idx = 0
        popup._update_highlight = MagicMock()
        popup.move_selection(1)
        self.assertEqual(popup._selected_idx, 0)


# ---------------------------------------------------------------------------
# InputArea — pure logic methods
# ---------------------------------------------------------------------------


def _make_input() -> InputArea:
    area = object.__new__(InputArea)
    area._enabled = True
    area._skill_slugs = []
    area._popup = None
    area._submit_callback = None
    area._cancel_callback = None
    area._applying_theme = False
    area._theme_css = ""
    area._flat = False
    area._focus_callback = None
    area.setStyleSheet = MagicMock()
    return area


class TestInputAreaSetCallbacks(unittest.TestCase):
    def test_set_submit_callback(self):
        area = _make_input()
        cb = MagicMock()
        area.set_submit_callback(cb)
        self.assertIs(area._submit_callback, cb)

    def test_set_cancel_callback(self):
        area = _make_input()
        cb = MagicMock()
        area.set_cancel_callback(cb)
        self.assertIs(area._cancel_callback, cb)


class TestInputAreaSetSkillSlugs(unittest.TestCase):
    def test_includes_builtin_commands(self):
        area = _make_input()
        area.set_skill_slugs([])
        self.assertIn("plan", area._skill_slugs)
        self.assertIn("modify", area._skill_slugs)
        self.assertIn("explore", area._skill_slugs)

    def test_merges_custom_slugs(self):
        area = _make_input()
        area.set_skill_slugs(["decompile", "rename"])
        self.assertIn("decompile", area._skill_slugs)
        self.assertIn("rename", area._skill_slugs)

    def test_result_is_sorted(self):
        area = _make_input()
        area.set_skill_slugs(["z", "a", "m"])
        self.assertEqual(area._skill_slugs, sorted(area._skill_slugs))

    def test_no_duplicates(self):
        area = _make_input()
        area.set_skill_slugs(["plan", "plan", "explore"])
        self.assertEqual(area._skill_slugs.count("plan"), 1)


class TestInputAreaCheckAutocomplete(unittest.TestCase):
    def test_no_slash_dismisses_popup(self):
        area = _make_input()
        area._skill_slugs = ["plan"]
        area._dismiss_popup = MagicMock()
        area.toPlainText = MagicMock(return_value="hello")
        area._check_autocomplete()
        area._dismiss_popup.assert_called_once()

    def test_slash_with_space_dismisses_popup(self):
        area = _make_input()
        area._skill_slugs = ["plan"]
        area._dismiss_popup = MagicMock()
        area.toPlainText = MagicMock(return_value="/plan some text")
        area._check_autocomplete()
        area._dismiss_popup.assert_called_once()

    def test_partial_slug_shows_popup(self):
        area = _make_input()
        area._skill_slugs = ["plan", "modify"]
        area._show_popup = MagicMock()
        area._dismiss_popup = MagicMock()
        area.toPlainText = MagicMock(return_value="/pl")
        area._check_autocomplete()
        area._show_popup.assert_called_once_with(["plan"])

    def test_no_match_dismisses_popup(self):
        area = _make_input()
        area._skill_slugs = ["plan", "modify"]
        area._dismiss_popup = MagicMock()
        area.toPlainText = MagicMock(return_value="/xyz")
        area._check_autocomplete()
        area._dismiss_popup.assert_called_once()


class TestInputAreaApplyTheme(unittest.TestCase):
    def test_apply_theme_sets_stylesheet_and_caches_css(self):
        area = _make_input()
        css = "QPlainTextEdit#input_area { background-color: #222222; color: #eeeeee; }"
        with patch("rikugan.ui.input_area.use_native_host_theme", return_value=True):
            with patch("rikugan.ui.input_area.build_input_area_stylesheet", return_value=css):
                area._apply_theme()
        area.setStyleSheet.assert_called_once()
        self.assertEqual(area._theme_css, css)
        self.assertFalse(area._applying_theme)

    def test_apply_theme_skips_when_css_is_unchanged(self):
        area = _make_input()
        expected_css = "QPlainTextEdit#input_area { background-color: #222222; color: #eeeeee; }"
        area._theme_css = expected_css
        with patch("rikugan.ui.input_area.use_native_host_theme", return_value=True):
            with patch("rikugan.ui.input_area.build_input_area_stylesheet", return_value=expected_css):
                area._apply_theme()
        area.setStyleSheet.assert_not_called()


if __name__ == "__main__":
    unittest.main()

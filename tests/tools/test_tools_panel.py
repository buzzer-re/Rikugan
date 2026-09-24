"""Tests for rikugan.ui.tools_panel — tab replacement and selection."""

from __future__ import annotations

import sys
import unittest
from unittest.mock import MagicMock

from tests.qt_stubs import ensure_pyside6_stubs

ensure_pyside6_stubs()

# Re-import this module only. Popping rikugan.ui.styles here would hand other
# test modules a second module object while their own imports still point at
# the first, and their monkeypatches would stop taking effect.
sys.modules.pop("rikugan.ui.tools_panel", None)

from rikugan.ui.tools_panel import (  # noqa: E402
    AGENTS_TAB_INDEX,
    RENAMER_TAB_INDEX,
    ToolsPanel,
)


def _make_panel(current: int) -> ToolsPanel:
    panel = object.__new__(ToolsPanel)
    panel._tabs = MagicMock()
    panel._tabs.currentIndex.return_value = current
    panel._tabs.widget.return_value = MagicMock()
    return panel


class TestReplaceTab(unittest.TestCase):
    """Replacing a placeholder must not move the user to another tab.

    ``QTabWidget.removeTab`` on the current tab selects its neighbour, which is
    how swapping in the real Renamer left the panel sitting on Agents.
    """

    def test_replacing_the_current_tab_restores_the_selection(self):
        panel = _make_panel(current=RENAMER_TAB_INDEX)
        panel._replace_tab(RENAMER_TAB_INDEX, MagicMock(), "Renamer")
        panel._tabs.setCurrentIndex.assert_called_once_with(RENAMER_TAB_INDEX)

    def test_replacing_another_tab_leaves_the_selection_alone(self):
        panel = _make_panel(current=RENAMER_TAB_INDEX)
        panel._replace_tab(AGENTS_TAB_INDEX, MagicMock(), "Agents")
        panel._tabs.setCurrentIndex.assert_not_called()

    def test_the_old_widget_is_scheduled_for_deletion(self):
        panel = _make_panel(current=RENAMER_TAB_INDEX)
        old = MagicMock()
        panel._tabs.widget.return_value = old
        panel._replace_tab(RENAMER_TAB_INDEX, MagicMock(), "Renamer")
        old.deleteLater.assert_called_once()

    def test_renamer_is_the_first_tab(self):
        self.assertEqual(RENAMER_TAB_INDEX, 0)
        self.assertLess(RENAMER_TAB_INDEX, AGENTS_TAB_INDEX)

    def test_loading_both_widgets_ends_on_the_renamer(self):
        """The order panel_core loads them in: agents first, then renamer."""
        panel = _make_panel(current=RENAMER_TAB_INDEX)
        selected = [RENAMER_TAB_INDEX]

        def _remove(index: int) -> None:
            # Qt moves the selection to the neighbour when the current tab goes.
            if index == selected[0]:
                selected[0] = index

        panel._tabs.removeTab.side_effect = _remove
        panel._tabs.setCurrentIndex.side_effect = lambda i: selected.__setitem__(0, i)
        panel._tabs.currentIndex.side_effect = lambda: selected[0]

        panel.set_agents_widget(MagicMock())
        panel.set_renamer_widget(MagicMock())
        self.assertEqual(selected[0], RENAMER_TAB_INDEX)


class TestPanelStylesheet(unittest.TestCase):
    def test_tools_chrome_uses_the_shared_product_palette(self):
        import rikugan.ui.tools_panel as tools_panel

        if not isinstance(tools_panel.PRODUCT_PANEL, str):
            self.skipTest("another test module left a stub in place of rikugan.ui.styles")
        css = tools_panel._panel_stylesheet()
        self.assertIn(tools_panel.PRODUCT_PANEL, css)
        self.assertIn(tools_panel.PRODUCT_ACCENT, css)
        self.assertNotIn("font-size: 11px", css)


if __name__ == "__main__":
    unittest.main()

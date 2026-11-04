"""Tests for rikugan.ui.mutation_log_view — pure list logic."""

from __future__ import annotations

import unittest
from unittest.mock import MagicMock, patch

from tests.qt_stubs import ensure_pyside6_stubs
ensure_pyside6_stubs()


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _make_record(reversible: bool = True, description: str = "desc", tool_name: str = "tool") -> MagicMock:
    rec = MagicMock()
    rec.reversible = reversible
    rec.description = description
    rec.tool_name = tool_name
    rec.timestamp = 0.0
    return rec


def _make_panel():
    """Build a MutationLogPanel with all Qt calls mocked out."""
    from rikugan.ui.mutation_log_view import MutationLogPanel
    panel = object.__new__(MutationLogPanel)
    panel._entries = []
    panel._count_label = MagicMock()
    panel._undo_btn = MagicMock()
    panel._entries_layout = MagicMock()
    return panel


# ---------------------------------------------------------------------------
# _update_count — label text and button state
# ---------------------------------------------------------------------------

class TestUpdateCount(unittest.TestCase):
    def setUp(self):
        self.panel = _make_panel()

    def _make_entry(self, reversible: bool):
        entry = MagicMock()
        entry.record = _make_record(reversible=reversible)
        return entry

    def test_zero_entries_label(self):
        self.panel._update_count()
        self.panel._count_label.setText.assert_called_with("0 mutations")

    def test_one_entry_singular(self):
        self.panel._entries = [self._make_entry(True)]
        self.panel._update_count()
        self.panel._count_label.setText.assert_called_with("1 mutation")

    def test_two_entries_plural(self):
        self.panel._entries = [self._make_entry(True), self._make_entry(True)]
        self.panel._update_count()
        self.panel._count_label.setText.assert_called_with("2 mutations")

    def test_three_entries_plural(self):
        self.panel._entries = [self._make_entry(True)] * 3
        self.panel._update_count()
        self.panel._count_label.setText.assert_called_with("3 mutations")

    def test_undo_disabled_when_empty(self):
        self.panel._update_count()
        self.panel._undo_btn.setEnabled.assert_called_with(False)

    def test_undo_enabled_when_reversible_entry(self):
        self.panel._entries = [self._make_entry(True)]
        self.panel._update_count()
        self.panel._undo_btn.setEnabled.assert_called_with(True)

    def test_undo_disabled_when_all_irreversible(self):
        self.panel._entries = [self._make_entry(False), self._make_entry(False)]
        self.panel._update_count()
        self.panel._undo_btn.setEnabled.assert_called_with(False)

    def test_undo_enabled_when_mixed_reversibility(self):
        self.panel._entries = [self._make_entry(False), self._make_entry(True)]
        self.panel._update_count()
        self.panel._undo_btn.setEnabled.assert_called_with(True)


# ---------------------------------------------------------------------------
# set_records
# ---------------------------------------------------------------------------

class TestSetRecords(unittest.TestCase):
    def _listed(self, panel):
        return [e.record for e in panel._entries]

    def _show(self, panel, records):
        def entry(_index, record, _parent):
            widget = MagicMock()
            widget.record = record
            return widget

        panel._entries_widget = MagicMock()
        panel._entries_layout.count.return_value = 1
        with patch("rikugan.ui.mutation_log_view.MutationEntryWidget", entry):
            panel.set_records(records)

    def test_lists_the_records_in_order(self):
        panel = _make_panel()
        records = [_make_record(description="a"), _make_record(description="b")]
        self._show(panel, records)
        self.assertEqual(self._listed(panel), records)

    def test_replaces_what_was_listed(self):
        # Switching to a chat must not leave the previous chat's entries behind.
        panel = _make_panel()
        self._show(panel, [_make_record(description="other chat")])
        kept = [_make_record(description="this chat")]
        self._show(panel, kept)
        self.assertEqual(self._listed(panel), kept)

    def test_an_empty_log_empties_the_panel(self):
        panel = _make_panel()
        self._show(panel, [_make_record()])
        self._show(panel, [])
        self.assertEqual(panel._entries, [])
        panel._undo_btn.setEnabled.assert_called_with(False)


class TestReversibilityTooltip(unittest.TestCase):
    def _tooltip(self, record):
        from rikugan.ui.mutation_log_view import _reversibility_tooltip

        return _reversibility_tooltip(record)

    def test_says_why_a_mutation_cannot_be_undone(self):
        record = _make_record(reversible=False)
        record.irreversible_reason = "no getter for the prior value"
        self.assertIn("no getter for the prior value", self._tooltip(record))

    def test_no_reason_still_reads_cleanly(self):
        record = _make_record(reversible=False)
        record.irreversible_reason = ""
        self.assertEqual(self._tooltip(record), "Not reversible")

    def test_reversible(self):
        self.assertEqual(self._tooltip(_make_record()), "Reversible")


# ---------------------------------------------------------------------------
# clear_all
# ---------------------------------------------------------------------------

class TestClearAll(unittest.TestCase):
    def _panel_with_n(self, n: int):
        panel = _make_panel()
        for _ in range(n):
            entry = MagicMock()
            entry.record = _make_record()
            panel._entries.append(entry)
        return panel

    def test_clears_all_entries(self):
        panel = self._panel_with_n(4)
        panel.clear_all()
        self.assertEqual(len(panel._entries), 0)

    def test_clear_empty_noop(self):
        panel = self._panel_with_n(0)
        panel.clear_all()  # must not raise
        self.assertEqual(len(panel._entries), 0)

    def test_clear_calls_delete_later_for_each(self):
        panel = self._panel_with_n(3)
        entries = list(panel._entries)
        panel.clear_all()
        for e in entries:
            e.deleteLater.assert_called_once()

    def test_clear_calls_remove_widget_for_each(self):
        panel = self._panel_with_n(3)
        entries = list(panel._entries)
        panel.clear_all()
        for e in entries:
            panel._entries_layout.removeWidget.assert_any_call(e)

    def test_count_label_updated_after_clear(self):
        panel = self._panel_with_n(3)
        panel.clear_all()
        panel._count_label.setText.assert_called_with("0 mutations")


if __name__ == "__main__":
    unittest.main()

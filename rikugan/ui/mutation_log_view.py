"""Mutation log panel: displays the history of mutating tool calls with undo support."""

from __future__ import annotations

import time
from typing import TYPE_CHECKING

from .qt_compat import (
    QFrame,
    QHBoxLayout,
    QLabel,
    QPushButton,
    QScrollArea,
    Qt,
    QVBoxLayout,
    QWidget,
    Signal,
)

if TYPE_CHECKING:
    from ..agent.mutation import MutationRecord


def _reversibility_tooltip(record: MutationRecord) -> str:
    if record.reversible:
        return "Reversible"
    if record.irreversible_reason:
        return f"Not reversible: {record.irreversible_reason}"
    return "Not reversible"


class MutationEntryWidget(QFrame):
    """Single mutation entry with description and undo status."""

    undo_clicked = Signal(int)  # emits the entry index

    def __init__(self, index: int, record: MutationRecord, parent: QWidget = None):
        super().__init__(parent)
        self.setObjectName("mutation_entry")
        self._index = index
        self._record = record

        layout = QHBoxLayout(self)
        layout.setContentsMargins(8, 4, 8, 4)

        # Reversibility indicator
        self._indicator = QLabel("↩" if record.reversible else "⊘")
        self._indicator.setFixedWidth(20)
        self._indicator.setStyleSheet(
            "color: #4ec9b0; font-size: 14px;" if record.reversible else "color: #808080; font-size: 14px;"
        )
        self._indicator.setToolTip(_reversibility_tooltip(record))
        layout.addWidget(self._indicator)

        # Description
        ts = time.strftime("%H:%M:%S", time.localtime(record.timestamp))
        self._desc = QLabel(f"[{ts}] {record.description}")
        self._desc.setWordWrap(True)
        self._desc.setStyleSheet("color: #d4d4d4; font-size: 11px;")
        layout.addWidget(self._desc, 1)

        # Tool name badge
        self._tool_badge = QLabel(record.tool_name)
        self._tool_badge.setStyleSheet(
            "color: #808080; font-size: 10px; padding: 1px 4px; background: #2d2d2d; border-radius: 3px;"
        )
        layout.addWidget(self._tool_badge)

    @property
    def record(self) -> MutationRecord:
        return self._record


class MutationLogPanel(QFrame):
    """Panel showing the mutation history with undo support."""

    undo_requested = Signal(int)  # emits count to undo

    def __init__(self, parent: QWidget = None):
        super().__init__(parent)
        self.setObjectName("mutation_log_panel")

        main_layout = QVBoxLayout(self)
        main_layout.setContentsMargins(0, 0, 0, 0)
        main_layout.setSpacing(0)

        # Header
        self._header = QFrame()
        self._header.setObjectName("mutation_log_header")
        header_layout = QHBoxLayout(self._header)
        header_layout.setContentsMargins(12, 8, 12, 8)

        self._title = QLabel("Mutation Log")
        self._title.setStyleSheet("color: #d4d4d4; font-weight: bold; font-size: 12px;")
        header_layout.addWidget(self._title)

        self._count_label = QLabel("0 mutations")
        self._count_label.setStyleSheet("color: #808080; font-size: 11px;")
        header_layout.addWidget(self._count_label)

        header_layout.addStretch()

        self._undo_btn = QPushButton("Undo Last")
        self._undo_btn.setStyleSheet(
            "QPushButton { color: #4ec9b0; background: #2d2d2d; "
            "border: 1px solid #4ec9b0; border-radius: 3px; "
            "padding: 3px 10px; font-size: 11px; }"
            "QPushButton:hover { background: #3d3d3d; }"
            "QPushButton:disabled { color: #555; border-color: #555; }"
        )
        self._undo_btn.clicked.connect(lambda: self.undo_requested.emit(1))
        self._undo_btn.setEnabled(False)
        header_layout.addWidget(self._undo_btn)

        main_layout.addWidget(self._header)

        # Scroll area for entries
        self._scroll = QScrollArea()
        self._scroll.setWidgetResizable(True)
        self._scroll.setHorizontalScrollBarPolicy(Qt.ScrollBarAlwaysOff)
        self._scroll.setStyleSheet("QScrollArea { border: none; background: transparent; }")

        self._entries_widget = QWidget()
        self._entries_layout = QVBoxLayout(self._entries_widget)
        self._entries_layout.setContentsMargins(4, 4, 4, 4)
        self._entries_layout.setSpacing(2)
        self._entries_layout.addStretch()

        self._scroll.setWidget(self._entries_widget)
        main_layout.addWidget(self._scroll)

        self._entries: list[MutationEntryWidget] = []

    def add_mutation(self, record: MutationRecord) -> None:
        """Add a new mutation entry to the log."""
        index = len(self._entries)
        entry = MutationEntryWidget(index, record, self._entries_widget)
        # Insert before the stretch
        self._entries_layout.insertWidget(self._entries_layout.count() - 1, entry)
        self._entries.append(entry)
        self._update_count()

    def set_records(self, records: list[MutationRecord]) -> None:
        """Show exactly these records, replacing whatever is listed."""
        self.clear_all()
        for record in records:
            self.add_mutation(record)

    def clear_all(self) -> None:
        """Clear all entries."""
        for entry in self._entries:
            self._entries_layout.removeWidget(entry)
            entry.deleteLater()
        self._entries.clear()
        self._update_count()

    def _update_count(self) -> None:
        n = len(self._entries)
        self._count_label.setText(f"{n} mutation{'s' if n != 1 else ''}")
        self._undo_btn.setEnabled(n > 0 and any(e.record.reversible for e in self._entries))

"""Agent tree view for the Agents tab (bulk-rename manager)."""

from __future__ import annotations

from dataclasses import dataclass

from .qt_compat import (
    QAbstractItemView,
    QComboBox,
    QHBoxLayout,
    QLabel,
    QPushButton,
    Qt,
    QTextEdit,
    QTreeWidget,
    QTreeWidgetItem,
    QVBoxLayout,
    QWidget,
    Signal,
)

_STATUS_COLORS: dict[str, str] = {
    "PENDING": "#808080",
    "RUNNING": "#dcdcaa",
    "COMPLETED": "#4ec9b0",
    "FAILED": "#f44747",
    "CANCELLED": "#808080",
}

_BTN_STYLE = (
    "QPushButton { background: #2d2d2d; color: #d4d4d4; border: 1px solid #3c3c3c; "
    "border-radius: 4px; padding: 4px 10px; font-size: 11px; }"
    "QPushButton:hover { background: #3c3c3c; }"
    "QPushButton:disabled { color: #555; }"
)

_TREE_STYLE = """
    QTreeWidget {
        background: #1e1e1e;
        color: #d4d4d4;
        border: 1px solid #3c3c3c;
        font-size: 11px;
        alternate-background-color: #252525;
    }
    QTreeWidget::item {
        padding: 2px 4px;
    }
    QTreeWidget::item:selected {
        background: #264f78;
        color: #ffffff;
    }
    QTreeWidget::item:hover {
        background: #2a2d2e;
    }
    QHeaderView::section {
        background: #2d2d2d;
        color: #d4d4d4;
        border: 1px solid #3c3c3c;
        padding: 3px 6px;
        font-size: 10px;
    }
"""


@dataclass
class AgentInfo:
    """Snapshot of an agent's state for display."""

    agent_id: str
    name: str
    agent_type: str
    status: str = "PENDING"
    turns: int = 0
    elapsed_seconds: float = 0.0
    summary: str = ""
    category: str = ""


class AgentTreeWidget(QWidget):
    """Tree-based view of running and completed sub-agents."""

    cancel_requested = Signal(str)  # agent_id
    inject_summary_requested = Signal(str)  # agent_id

    def __init__(self, parent: QWidget = None):
        super().__init__(parent)
        self.setObjectName("agent_tree_widget")

        main_layout = QVBoxLayout(self)
        main_layout.setContentsMargins(4, 4, 4, 4)
        main_layout.setSpacing(4)

        # Toolbar
        toolbar = QHBoxLayout()
        toolbar.setSpacing(4)

        self._kill_btn = QPushButton("Kill Selected")
        self._kill_btn.setStyleSheet(_BTN_STYLE)
        self._kill_btn.clicked.connect(self._on_kill_selected)
        toolbar.addWidget(self._kill_btn)

        self._clean_btn = QPushButton("Clean")
        self._clean_btn.setStyleSheet(_BTN_STYLE)
        self._clean_btn.setToolTip("Remove selected finished agents (or all finished if none selected)")
        self._clean_btn.clicked.connect(self._on_clean)
        toolbar.addWidget(self._clean_btn)

        self._filter_combo = QComboBox()
        self._filter_combo.setFixedWidth(130)
        self._filter_combo.setStyleSheet(
            "QComboBox { background: #2d2d2d; color: #d4d4d4; border: 1px solid #3c3c3c; "
            "border-radius: 4px; padding: 3px 6px; font-size: 11px; }"
        )
        self._filter_combo.addItems(["All Agents", "General", "Bulk Rename"])
        self._filter_combo.currentTextChanged.connect(self._apply_filter)
        toolbar.addWidget(self._filter_combo)

        toolbar.addStretch()

        self._status_label = QLabel("0 running / 0 completed")
        self._status_label.setStyleSheet("color: #808080; font-size: 11px;")
        toolbar.addWidget(self._status_label)

        main_layout.addLayout(toolbar)

        # Tree widget
        self._tree = QTreeWidget()
        self._tree.setObjectName("agent_tree")
        self._tree.setStyleSheet(_TREE_STYLE)
        self._tree.setHeaderLabels(["Name", "Type", "Status", "Turns", "Time"])
        self._tree.setColumnWidth(0, 150)
        self._tree.setColumnWidth(1, 100)
        self._tree.setColumnWidth(2, 80)
        self._tree.setColumnWidth(3, 50)
        self._tree.setColumnWidth(4, 60)
        self._tree.setAlternatingRowColors(True)
        self._tree.setRootIsDecorated(False)
        self._tree.setSelectionMode(QAbstractItemView.SelectionMode.ExtendedSelection)
        self._tree.setSelectionBehavior(QAbstractItemView.SelectionBehavior.SelectRows)
        self._tree.setFocusPolicy(Qt.FocusPolicy.StrongFocus)
        self._tree.itemSelectionChanged.connect(self._on_item_selected)
        main_layout.addWidget(self._tree)

        # Output preview
        self._preview = QTextEdit()
        self._preview.setObjectName("agent_preview")
        self._preview.setReadOnly(True)
        self._preview.setFixedHeight(80)
        self._preview.setStyleSheet(
            "QTextEdit { background: #252525; color: #d4d4d4; border: 1px solid #3c3c3c; "
            "font-size: 11px; padding: 4px; }"
        )
        self._preview.setPlaceholderText("Select an agent to preview its output...")
        main_layout.addWidget(self._preview)

        # Internal agent tracking: agent_id -> AgentInfo
        self._agents: dict[str, AgentInfo] = {}
        # Map agent_id -> QTreeWidgetItem
        self._items: dict[str, QTreeWidgetItem] = {}

    def _on_kill_selected(self) -> None:
        """Cancel the currently selected agent(s)."""
        items = self._tree.selectedItems()
        if not items:
            return
        for item in items:
            agent_id = item.data(0, Qt.ItemDataRole.UserRole)
            if agent_id:
                info = self._agents.get(agent_id)
                if info and info.status in ("PENDING", "RUNNING"):
                    self.cancel_requested.emit(agent_id)

    def _on_clean(self) -> None:
        """Remove finished agents from the tree.

        If agents are selected, only remove those that are finished.
        If nothing is selected, remove all finished agents.
        """
        _FINISHED = {"COMPLETED", "FAILED", "CANCELLED"}
        selected = self._tree.selectedItems()

        if selected:
            to_remove = []
            for item in selected:
                agent_id = item.data(0, Qt.ItemDataRole.UserRole)
                if agent_id:
                    info = self._agents.get(agent_id)
                    if info and info.status in _FINISHED:
                        to_remove.append(agent_id)
        else:
            to_remove = [aid for aid, info in self._agents.items() if info.status in _FINISHED]

        for agent_id in to_remove:
            item = self._items.pop(agent_id, None)
            if item is not None:
                idx = self._tree.indexOfTopLevelItem(item)
                if idx >= 0:
                    self._tree.takeTopLevelItem(idx)
            self._agents.pop(agent_id, None)

        self._update_status_counts()

    def update_agent(self, info: AgentInfo) -> None:
        """Add or update a tree item for the given agent."""
        self._agents[info.agent_id] = info

        if info.agent_id in self._items:
            item = self._items[info.agent_id]
        else:
            item = QTreeWidgetItem(self._tree)
            item.setData(0, Qt.ItemDataRole.UserRole, info.agent_id)
            item.setFlags(Qt.ItemFlag.ItemIsEnabled | Qt.ItemFlag.ItemIsSelectable)
            self._items[info.agent_id] = item

        item.setText(0, info.name)
        item.setText(1, info.agent_type)
        item.setText(2, info.status)
        item.setText(3, str(info.turns))
        item.setText(4, self._format_elapsed(info.elapsed_seconds))

        # Status color
        color = _STATUS_COLORS.get(info.status, "#d4d4d4")
        from .qt_compat import QColor

        item.setForeground(2, QColor(color))

        # Apply current category filter to this item
        filter_text = self._filter_combo.currentText()
        if filter_text == "Bulk Rename":
            item.setHidden(info.category != "bulk_rename")
        elif filter_text == "General":
            item.setHidden(info.category == "bulk_rename")
        else:
            item.setHidden(False)

        self._update_status_counts()

        # Auto-update preview if this agent is selected
        selected = self._tree.selectedItems()
        if selected and selected[0].data(0, Qt.ItemDataRole.UserRole) == info.agent_id:
            self._preview.setPlainText(info.summary or "(no output yet)")

    def _apply_filter(self, _text: str = "") -> None:
        """Show/hide tree items based on the selected category filter."""
        selected = self._filter_combo.currentText()
        for agent_id, item in self._items.items():
            info = self._agents.get(agent_id)
            if info is None:
                continue
            if selected == "All Agents":
                item.setHidden(False)
            elif selected == "Bulk Rename":
                item.setHidden(info.category != "bulk_rename")
            elif selected == "General":
                item.setHidden(info.category == "bulk_rename")
        self._update_status_counts()

    def _on_item_selected(self) -> None:
        """Show the summary of the selected agent in the preview pane."""
        items = self._tree.selectedItems()
        if not items:
            self._preview.clear()
            return
        agent_id = items[0].data(0, Qt.ItemDataRole.UserRole)
        info = self._agents.get(agent_id)
        if info:
            self._preview.setPlainText(info.summary or "(no output yet)")
        else:
            self._preview.clear()

    def _update_status_counts(self) -> None:
        """Refresh the running / completed counts label."""
        running = sum(1 for a in self._agents.values() if a.status == "RUNNING")
        completed = sum(1 for a in self._agents.values() if a.status == "COMPLETED")
        self._status_label.setText(f"{running} running / {completed} completed")

    @staticmethod
    def _format_elapsed(seconds: float) -> str:
        """Format elapsed seconds as m:ss."""
        mins = int(seconds) // 60
        secs = int(seconds) % 60
        return f"{mins}:{secs:02d}"

"""A2A bridge widget: external agent integration UI."""

from __future__ import annotations

from dataclasses import dataclass

from .qt_compat import (
    QCheckBox,
    QComboBox,
    QGroupBox,
    QHBoxLayout,
    QHeaderView,
    QLabel,
    QListWidget,
    QListWidgetItem,
    QPushButton,
    Qt,
    QTableWidget,
    QTableWidgetItem,
    QTextEdit,
    QVBoxLayout,
    QWidget,
    Signal,
)

_BTN_STYLE = (
    "QPushButton { background: #2d2d2d; color: #d4d4d4; border: 1px solid #3c3c3c; "
    "border-radius: 4px; padding: 4px 10px; font-size: 11px; }"
    "QPushButton:hover { background: #3c3c3c; }"
    "QPushButton:disabled { color: #555; }"
)

_SEND_BTN_STYLE = (
    "QPushButton { background: #2d2d2d; color: #4ec9b0; border: 1px solid #4ec9b0; "
    "border-radius: 4px; padding: 5px 16px; font-size: 11px; font-weight: bold; }"
    "QPushButton:hover { background: #3c3c3c; }"
    "QPushButton:disabled { color: #555; border-color: #555; }"
)

_GROUP_STYLE = """
    QGroupBox {
        color: #d4d4d4;
        border: 1px solid #3c3c3c;
        border-radius: 4px;
        margin-top: 8px;
        padding-top: 14px;
        font-size: 11px;
        font-weight: bold;
    }
    QGroupBox::title {
        subcontrol-origin: margin;
        left: 8px;
        padding: 0 4px;
    }
"""

_LIST_STYLE = (
    "QListWidget { background: #1e1e1e; color: #d4d4d4; border: 1px solid #3c3c3c; "
    "font-size: 11px; }"
    "QListWidget::item { padding: 4px 6px; }"
    "QListWidget::item:selected { background: #2d2d2d; }"
)

_TABLE_STYLE = """
    QTableWidget {
        background: #1e1e1e;
        color: #d4d4d4;
        border: 1px solid #3c3c3c;
        gridline-color: #3c3c3c;
        font-size: 11px;
        alternate-background-color: #252525;
    }
    QTableWidget::item {
        padding: 2px 4px;
    }
    QTableWidget::item:selected {
        background: #2d2d2d;
    }
    QHeaderView::section {
        background: #2d2d2d;
        color: #d4d4d4;
        border: 1px solid #3c3c3c;
        padding: 3px 6px;
        font-size: 10px;
    }
"""

_COMBO_STYLE = (
    "QComboBox { background: #2d2d2d; color: #d4d4d4; border: 1px solid #3c3c3c; "
    "border-radius: 3px; padding: 3px 6px; font-size: 11px; }"
)

_TEXT_STYLE = (
    "QTextEdit { background: #2d2d2d; color: #d4d4d4; border: 1px solid #3c3c3c; "
    "border-radius: 3px; padding: 4px; font-size: 11px; }"
    "QTextEdit:focus { border-color: #4ec9b0; }"
)

_CHECK_STYLE = "QCheckBox { color: #d4d4d4; font-size: 11px; spacing: 6px; }"

_STATUS_COLORS: dict[str, str] = {
    "pending": "#808080",
    "running": "#dcdcaa",
    "completed": "#4ec9b0",
    "failed": "#f44747",
    "cancelled": "#808080",
}

# Status display markers for the agent list
_AGENT_STATUS_MARKERS: dict[str, str] = {
    "online": "\u2022",  # bullet
    "offline": "\u25cb",  # circle outline
    "busy": "\u25cf",  # filled circle
}

# Column indices for task history table
_COL_AGENT = 0
_COL_TASK = 1
_COL_STATUS = 2
_COL_ACTIONS = 3


@dataclass
class AgentEntry:
    """An external agent discovered via A2A."""

    name: str
    description: str = ""
    status: str = "online"


@dataclass
class TaskEntry:
    """A delegated task record."""

    task_id: str
    agent_name: str
    task: str
    status: str = "pending"
    result: str = ""


class A2ABridgeWidget(QWidget):
    """External agent integration interface for the A2A tab."""

    task_requested = Signal(str, str, bool)  # agent_name, task, include_context
    inject_result_requested = Signal(str)  # result text

    def __init__(self, parent: QWidget = None):
        super().__init__(parent)
        self.setObjectName("a2a_bridge_widget")

        main_layout = QVBoxLayout(self)
        main_layout.setContentsMargins(4, 4, 4, 4)
        main_layout.setSpacing(6)

        # --- Available Agents ---
        agents_group = QGroupBox("Available Agents")
        agents_group.setStyleSheet(_GROUP_STYLE)
        agents_layout = QVBoxLayout(agents_group)

        self._agent_list = QListWidget()
        self._agent_list.setObjectName("a2a_agent_list")
        self._agent_list.setStyleSheet(_LIST_STYLE)
        self._agent_list.setMaximumHeight(120)
        agents_layout.addWidget(self._agent_list)

        main_layout.addWidget(agents_group)

        # --- Delegate Task ---
        delegate_group = QGroupBox("Delegate Task")
        delegate_group.setStyleSheet(_GROUP_STYLE)
        delegate_layout = QVBoxLayout(delegate_group)
        delegate_layout.setSpacing(6)

        # Target agent combo
        target_row = QHBoxLayout()
        target_label = QLabel("Target Agent:")
        target_label.setStyleSheet("color: #d4d4d4; font-size: 11px;")
        target_row.addWidget(target_label)

        self._target_combo = QComboBox()
        self._target_combo.setStyleSheet(_COMBO_STYLE)
        target_row.addWidget(self._target_combo, 1)
        delegate_layout.addLayout(target_row)

        # Task description
        self._task_edit = QTextEdit()
        self._task_edit.setObjectName("a2a_task_edit")
        self._task_edit.setStyleSheet(_TEXT_STYLE)
        self._task_edit.setPlaceholderText("Describe the task to delegate...")
        self._task_edit.setFixedHeight(80)
        delegate_layout.addWidget(self._task_edit)

        # Include context checkbox
        self._include_context_check = QCheckBox("Include current context summary")
        self._include_context_check.setStyleSheet(_CHECK_STYLE)
        self._include_context_check.setChecked(True)
        delegate_layout.addWidget(self._include_context_check)

        # Send button
        self._send_btn = QPushButton("Send Task")
        self._send_btn.setStyleSheet(_SEND_BTN_STYLE)
        self._send_btn.clicked.connect(self._on_send_task)
        delegate_layout.addWidget(self._send_btn, alignment=Qt.AlignmentFlag.AlignRight)

        main_layout.addWidget(delegate_group)

        # --- Task History ---
        history_group = QGroupBox("Task History")
        history_group.setStyleSheet(_GROUP_STYLE)
        history_layout = QVBoxLayout(history_group)

        self._history_table = QTableWidget()
        self._history_table.setObjectName("a2a_history_table")
        self._history_table.setStyleSheet(_TABLE_STYLE)
        self._history_table.setColumnCount(4)
        self._history_table.setHorizontalHeaderLabels(["Agent", "Task", "Status", "Actions"])
        self._history_table.setAlternatingRowColors(True)
        self._history_table.setSelectionBehavior(QTableWidget.SelectionBehavior.SelectRows)
        self._history_table.verticalHeader().setVisible(False)

        header = self._history_table.horizontalHeader()
        header.setSectionResizeMode(0, QHeaderView.ResizeMode.Fixed)
        header.setSectionResizeMode(1, QHeaderView.ResizeMode.Stretch)
        header.setSectionResizeMode(2, QHeaderView.ResizeMode.Fixed)
        header.setSectionResizeMode(3, QHeaderView.ResizeMode.Fixed)
        self._history_table.setColumnWidth(0, 100)
        self._history_table.setColumnWidth(2, 80)
        self._history_table.setColumnWidth(3, 70)

        history_layout.addWidget(self._history_table)

        main_layout.addWidget(history_group)

        # Internal state
        self._agents: list[AgentEntry] = []
        self._tasks: dict[str, TaskEntry] = {}  # task_id -> TaskEntry
        self._task_rows: dict[str, int] = {}  # task_id -> row index

    def set_agents(self, agents: list[dict]) -> None:
        """Populate the agent list and target combo.

        Each dict: {"name": str, "description": str, "status": str}
        """
        self._agent_list.clear()
        self._target_combo.clear()
        self._agents.clear()

        for agent_dict in agents:
            entry = AgentEntry(
                name=agent_dict["name"],
                description=agent_dict.get("description", ""),
                status=agent_dict.get("status", "online"),
            )
            self._agents.append(entry)

            # List item with status marker
            marker = _AGENT_STATUS_MARKERS.get(entry.status, "\u2022")
            item = QListWidgetItem(f"{marker} {entry.name}")
            item.setToolTip(entry.description or entry.name)
            from .qt_compat import QColor

            if entry.status == "online":
                item.setForeground(QColor("#d4d4d4"))
            elif entry.status == "busy":
                item.setForeground(QColor("#dcdcaa"))
            else:
                item.setForeground(QColor("#808080"))
            self._agent_list.addItem(item)

            # Combo entry
            self._target_combo.addItem(entry.name)

        self._send_btn.setEnabled(len(self._agents) > 0)

    def add_task_entry(self, agent_name: str, task: str, task_id: str) -> None:
        """Add a new task to the history table."""
        entry = TaskEntry(
            task_id=task_id,
            agent_name=agent_name,
            task=task,
            status="pending",
        )
        self._tasks[task_id] = entry

        row = self._history_table.rowCount()
        self._history_table.insertRow(row)
        self._task_rows[task_id] = row

        # Agent column
        agent_item = QTableWidgetItem(agent_name)
        agent_item.setFlags(Qt.ItemFlag.ItemIsEnabled | Qt.ItemFlag.ItemIsSelectable)
        self._history_table.setItem(row, _COL_AGENT, agent_item)

        # Task column (truncated)
        display_task = task[:60] + "..." if len(task) > 60 else task
        task_item = QTableWidgetItem(display_task)
        task_item.setFlags(Qt.ItemFlag.ItemIsEnabled | Qt.ItemFlag.ItemIsSelectable)
        task_item.setToolTip(task)
        self._history_table.setItem(row, _COL_TASK, task_item)

        # Status column
        status_item = QTableWidgetItem("pending")
        status_item.setFlags(Qt.ItemFlag.ItemIsEnabled | Qt.ItemFlag.ItemIsSelectable)
        from .qt_compat import QColor

        status_item.setForeground(QColor(_STATUS_COLORS.get("pending", "#808080")))
        self._history_table.setItem(row, _COL_STATUS, status_item)

        # Actions column - placeholder until completed
        actions_item = QTableWidgetItem("")
        actions_item.setFlags(Qt.ItemFlag.ItemIsEnabled)
        self._history_table.setItem(row, _COL_ACTIONS, actions_item)

    def update_task_status(self, task_id: str, status: str, result: str) -> None:
        """Update task status and result. Add Inject button when completed."""
        entry = self._tasks.get(task_id)
        if entry is None:
            return

        entry.status = status
        entry.result = result

        row = self._task_rows.get(task_id)
        if row is None:
            return

        # Update status cell
        status_item = self._history_table.item(row, _COL_STATUS)
        if status_item:
            status_item.setText(status)
            from .qt_compat import QColor

            color = _STATUS_COLORS.get(status, "#d4d4d4")
            status_item.setForeground(QColor(color))

        # Add Inject button for completed tasks
        if status == "completed" and result:
            inject_btn = QPushButton("Inject")
            inject_btn.setStyleSheet(
                "QPushButton { background: #2d2d2d; color: #4ec9b0; "
                "border: 1px solid #4ec9b0; border-radius: 3px; "
                "padding: 2px 8px; font-size: 10px; }"
                "QPushButton:hover { background: #3c3c3c; }"
            )
            inject_btn.clicked.connect(lambda checked, r=result: self.inject_result_requested.emit(r))
            self._history_table.setCellWidget(row, _COL_ACTIONS, inject_btn)

    def _on_send_task(self) -> None:
        """Validate inputs and emit task_requested."""
        if self._target_combo.count() == 0:
            return

        agent_name = self._target_combo.currentText()
        task = self._task_edit.toPlainText().strip()
        if not task:
            return

        include_context = self._include_context_check.isChecked()
        self.task_requested.emit(agent_name, task, include_context)

        # Clear the task input after sending
        self._task_edit.clear()

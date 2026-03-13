"""Bulk function renaming UI for the Renamer tab."""

from __future__ import annotations

from dataclasses import dataclass

from .qt_compat import (
    QAbstractItemView,
    QCheckBox,
    QComboBox,
    QHBoxLayout,
    QHeaderView,
    QIntValidator,
    QLabel,
    QLineEdit,
    QProgressBar,
    QPushButton,
    QRadioButton,
    Qt,
    QTableWidget,
    QTableWidgetItem,
    QTimer,
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

_STOP_BTN_STYLE = (
    "QPushButton { background: #2d2d2d; color: #c42b1c; border: 1px solid #c42b1c; "
    "border-radius: 4px; padding: 4px 10px; font-size: 11px; font-weight: bold; }"
    "QPushButton:hover { background: #3c3c3c; }"
    "QPushButton:disabled { color: #555; border-color: #555; }"
)

_START_BTN_STYLE = (
    "QPushButton { background: #2d2d2d; color: #d4d4d4; border: 1px solid #d4d4d4; "
    "border-radius: 4px; padding: 4px 14px; font-size: 11px; font-weight: bold; }"
    "QPushButton:hover { background: #3c3c3c; }"
    "QPushButton:disabled { color: #555; border-color: #555; }"
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

_FILTER_STYLE = (
    "QLineEdit { background: #2d2d2d; color: #d4d4d4; border: 1px solid #3c3c3c; "
    "border-radius: 3px; padding: 3px 6px; font-size: 11px; }"
    "QLineEdit:focus { border-color: #4ec9b0; }"
)

_COMBO_STYLE = (
    "QComboBox { background: #2d2d2d; color: #d4d4d4; border: 1px solid #3c3c3c; "
    "border-radius: 3px; padding: 3px 6px; font-size: 11px; }"
)

_NUM_INPUT_STYLE = (
    "QLineEdit { background: #2d2d2d; color: #d4d4d4; border: 1px solid #3c3c3c; "
    "border-radius: 3px; padding: 2px 4px; font-size: 11px; }"
)

_PROGRESS_STYLE = (
    "QProgressBar { background: #2d2d2d; border: 1px solid #3c3c3c; "
    "border-radius: 3px; text-align: center; color: #d4d4d4; font-size: 10px; }"
    "QProgressBar::chunk { background: #808080; border-radius: 2px; }"
)

_RADIO_STYLE = "QRadioButton { color: #d4d4d4; font-size: 11px; spacing: 4px; }"

_CHECK_STYLE = "QCheckBox { spacing: 0px; } QCheckBox::indicator { width: 14px; height: 14px; }"

_STATUS_COLORS: dict[str, str] = {
    "queued": "#808080",
    "analyzing": "#dcdcaa",
    "renamed": "#4ec9b0",
    "reverted": "#569cd6",
    "skipped": "#d7ba7d",
    "error": "#f44747",
}

# Column indices
_COL_CHECK = 0
_COL_ADDR = 1
_COL_NAME = 2
_COL_LENGTH = 3
_COL_NEWNAME = 4
_COL_STATUS = 5


@dataclass
class FunctionEntry:
    """A function loaded into the renamer table."""

    address: int
    name: str
    is_import: bool
    instruction_count: int


class _NumericTableItem(QTableWidgetItem):
    """Table item that sorts numerically instead of lexicographically."""

    def __init__(self, text: str, sort_value: int):
        super().__init__(text)
        self._sort_value = sort_value

    def __lt__(self, other: QTableWidgetItem) -> bool:
        if isinstance(other, _NumericTableItem):
            return self._sort_value < other._sort_value
        return super().__lt__(other)


class BulkRenamerWidget(QWidget):
    """Bulk function renaming interface with filtering and batch controls."""

    start_requested = Signal(list, str, int, int)  # jobs, mode, batch_size, max_concurrent
    pause_requested = Signal()
    cancel_requested = Signal()
    undo_requested = Signal()
    seek_requested = Signal(object)  # address (64-bit int, can't use Signal(int))

    def __init__(self, parent: QWidget = None):
        super().__init__(parent)
        self.setObjectName("bulk_renamer_widget")
        self._loading = False  # guard to suppress filter during load

        main_layout = QVBoxLayout(self)
        main_layout.setContentsMargins(4, 4, 4, 4)
        main_layout.setSpacing(4)

        # --- Top bar: filter + selection controls ---
        top_bar = QHBoxLayout()
        top_bar.setSpacing(4)

        self._filter_edit = QLineEdit()
        self._filter_edit.setPlaceholderText("Filter by name or address...")
        self._filter_edit.setStyleSheet(_FILTER_STYLE)
        self._filter_edit.textChanged.connect(self._on_filter_changed)
        top_bar.addWidget(self._filter_edit, 1)

        self._filter_combo = QComboBox()
        self._filter_combo.setStyleSheet(_COMBO_STYLE)
        self._filter_combo.addItems(["All Functions", "Auto-named Only", "User-renamed", "Imports"])
        self._filter_combo.currentIndexChanged.connect(self._on_filter_changed)
        top_bar.addWidget(self._filter_combo)

        self._selection_label = QLabel("0 / 0 selected")
        self._selection_label.setStyleSheet("color: #808080; font-size: 11px;")
        top_bar.addWidget(self._selection_label)

        main_layout.addLayout(top_bar)

        # --- Table ---
        self._table = QTableWidget()
        self._table.setObjectName("renamer_table")
        self._table.setStyleSheet(_TABLE_STYLE)
        self._table.setColumnCount(6)
        self._table.setHorizontalHeaderLabels(["", "Address", "Current Name", "Length", "New Name", "Status"])
        self._table.setAlternatingRowColors(True)
        self._table.setSelectionMode(QAbstractItemView.SelectionMode.ExtendedSelection)
        self._table.setSelectionBehavior(QAbstractItemView.SelectionBehavior.SelectRows)
        self._table.verticalHeader().setVisible(False)
        self._table.setSortingEnabled(True)

        # Header checkbox for column 0 (select all / deselect all)
        self._header_check = QCheckBox()
        self._header_check.setStyleSheet(_CHECK_STYLE)
        self._header_check.setChecked(False)
        self._header_check.stateChanged.connect(self._on_header_check_changed)
        header = self._table.horizontalHeader()
        header.setSectionResizeMode(0, QHeaderView.ResizeMode.Fixed)
        header.setSectionResizeMode(1, QHeaderView.ResizeMode.Interactive)
        header.setSectionResizeMode(2, QHeaderView.ResizeMode.Interactive)
        header.setSectionResizeMode(3, QHeaderView.ResizeMode.Interactive)
        header.setSectionResizeMode(4, QHeaderView.ResizeMode.Stretch)
        header.setSectionResizeMode(5, QHeaderView.ResizeMode.Fixed)
        self._table.setColumnWidth(0, 30)
        self._table.setColumnWidth(1, 110)
        self._table.setColumnWidth(2, 180)
        self._table.setColumnWidth(3, 60)
        self._table.setColumnWidth(5, 80)
        # Disable sort indicator on checkbox column
        header.setSortIndicatorShown(True)

        # Place the checkbox widget over the first header section
        self._header_check.setParent(self._table.horizontalHeader())
        self._header_check.setGeometry(8, 3, 16, 16)
        header.sectionResized.connect(self._reposition_header_check)

        self._table.itemChanged.connect(self._on_item_changed)
        self._table.cellClicked.connect(self._on_cell_clicked)
        self._table.cellDoubleClicked.connect(self._on_cell_double_clicked)
        main_layout.addWidget(self._table)

        # --- Analysis controls ---
        analysis_bar = QHBoxLayout()
        analysis_bar.setSpacing(6)

        mode_label = QLabel("Mode:")
        mode_label.setStyleSheet("color: #d4d4d4; font-size: 11px;")
        analysis_bar.addWidget(mode_label)

        self._quick_radio = QRadioButton("Quick")
        self._quick_radio.setStyleSheet(_RADIO_STYLE)
        self._quick_radio.setChecked(True)
        analysis_bar.addWidget(self._quick_radio)

        self._deep_radio = QRadioButton("Deep")
        self._deep_radio.setStyleSheet(_RADIO_STYLE)
        self._deep_radio.toggled.connect(lambda: self._update_selection_count())
        analysis_bar.addWidget(self._deep_radio)

        analysis_bar.addSpacing(12)

        batch_label = QLabel("Batch:")
        batch_label.setStyleSheet("color: #d4d4d4; font-size: 11px;")
        batch_label.setToolTip("Quick: functions per LLM prompt. Deep: ignored (1 agent per function).")
        analysis_bar.addWidget(batch_label)

        self._batch_input = QLineEdit("10")
        self._batch_input.setStyleSheet(_NUM_INPUT_STYLE)
        self._batch_input.setValidator(QIntValidator(1, 999999))
        self._batch_input.setFixedWidth(50)
        self._batch_input.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self._batch_input.setToolTip("Quick: functions per LLM prompt. Deep: ignored (1 agent per function).")
        self._batch_input.textChanged.connect(lambda: self._update_selection_count())
        analysis_bar.addWidget(self._batch_input)

        concurrent_label = QLabel("Jobs:")
        concurrent_label.setStyleSheet("color: #d4d4d4; font-size: 11px;")
        concurrent_label.setToolTip("Max parallel agents/requests running at the same time")
        analysis_bar.addWidget(concurrent_label)

        self._concurrent_input = QLineEdit("3")
        self._concurrent_input.setStyleSheet(_NUM_INPUT_STYLE)
        self._concurrent_input.setValidator(QIntValidator(1, 999999))
        self._concurrent_input.setFixedWidth(50)
        self._concurrent_input.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self._concurrent_input.setToolTip("Max parallel agents/requests running at the same time")
        analysis_bar.addWidget(self._concurrent_input)

        analysis_bar.addStretch()
        main_layout.addLayout(analysis_bar)

        # --- Action bar ---
        action_bar = QHBoxLayout()
        action_bar.setSpacing(4)

        self._start_btn = QPushButton("Start")
        self._start_btn.setStyleSheet(_START_BTN_STYLE)
        self._start_btn.clicked.connect(self._on_start)
        action_bar.addWidget(self._start_btn)

        self._stop_btn = QPushButton("Stop")
        self._stop_btn.setStyleSheet(_STOP_BTN_STYLE)
        self._stop_btn.setEnabled(False)
        self._stop_btn.clicked.connect(self._on_stop)
        action_bar.addWidget(self._stop_btn)

        self._pause_btn = QPushButton("Pause")
        self._pause_btn.setStyleSheet(_BTN_STYLE)
        self._pause_btn.setEnabled(False)
        self._pause_btn.clicked.connect(self._on_pause_toggle)
        action_bar.addWidget(self._pause_btn)

        self._undo_btn = QPushButton("Undo All")
        self._undo_btn.setStyleSheet(_BTN_STYLE)
        self._undo_btn.setEnabled(False)
        self._undo_btn.clicked.connect(self.undo_requested.emit)
        action_bar.addWidget(self._undo_btn)

        self._progress = QProgressBar()
        self._progress.setStyleSheet(_PROGRESS_STYLE)
        self._progress.setFixedHeight(18)
        self._progress.setValue(0)
        action_bar.addWidget(self._progress, 1)

        self._progress_label = QLabel("0 / 0")
        self._progress_label.setStyleSheet("color: #808080; font-size: 11px;")
        action_bar.addWidget(self._progress_label)

        main_layout.addLayout(action_bar)

        # Internal state
        self._entries: list[FunctionEntry] = []
        self._addr_to_entry: dict[int, int] = {}  # address -> index in _entries
        self._paused = False

    def _reposition_header_check(self, _idx: int = 0, _old: int = 0, _new: int = 0) -> None:
        """Keep the header checkbox centred in the first header section."""
        x = (self._table.columnWidth(0) - 16) // 2
        self._header_check.setGeometry(x, 3, 16, 16)

    def _on_header_check_changed(self, state: int) -> None:
        """Toggle all visible row checkboxes based on header checkbox."""
        checked = state == Qt.CheckState.Checked.value
        self._table.itemChanged.disconnect(self._on_item_changed)
        for row in range(self._table.rowCount()):
            if not self._table.isRowHidden(row):
                item = self._table.item(row, _COL_CHECK)
                if item:
                    item.setCheckState(Qt.CheckState.Checked if checked else Qt.CheckState.Unchecked)
        self._table.itemChanged.connect(self._on_item_changed)
        self._update_selection_count()

    # Rows to insert per timer tick during chunked loading.
    _LOAD_CHUNK_SIZE = 200

    def load_functions(self, functions: list[dict]) -> None:
        """Populate the table from a list of function dicts.

        Each dict: {"address": int, "name": str, "is_import": bool, "instruction_count": int}

        For large lists the rows are inserted in chunks via a QTimer so the UI
        thread stays responsive (prevents the "blank panel" freeze).
        """
        # Cancel any in-flight chunked load
        self._cancel_chunked_load()

        self._loading = True
        self._table.setSortingEnabled(False)
        self._table.itemChanged.disconnect(self._on_item_changed)
        self._table.setRowCount(0)
        self._entries.clear()
        self._addr_to_entry.clear()

        self._table.setRowCount(len(functions))

        if len(functions) <= self._LOAD_CHUNK_SIZE:
            # Small list — populate synchronously for snappy feel
            self._populate_rows(functions, 0, len(functions))
            self._finish_load()
        else:
            # Large list — process in chunks to keep UI alive
            self._pending_functions = functions
            self._load_cursor = 0
            self._load_timer = QTimer(self)
            self._load_timer.setInterval(0)  # process next chunk ASAP
            self._load_timer.timeout.connect(self._load_next_chunk)
            self._load_timer.start()

    def _load_next_chunk(self) -> None:
        """Insert the next chunk of rows."""
        funcs = self._pending_functions
        start = self._load_cursor
        end = min(start + self._LOAD_CHUNK_SIZE, len(funcs))

        self._populate_rows(funcs, start, end)
        self._load_cursor = end

        if end >= len(funcs):
            self._finish_load()
            self._cancel_chunked_load()

    def _cancel_chunked_load(self) -> None:
        """Stop and clean up any in-flight chunked load timer."""
        timer = getattr(self, "_load_timer", None)
        if timer is not None:
            timer.stop()
            timer.deleteLater()
            self._load_timer = None
        self._pending_functions = []
        self._load_cursor = 0

    def _populate_rows(self, functions: list[dict], start: int, end: int) -> None:
        """Insert rows [start, end) into the table."""
        for row in range(start, end):
            func = functions[row]
            entry = FunctionEntry(
                address=func["address"],
                name=func["name"],
                is_import=func.get("is_import", False),
                instruction_count=func.get("instruction_count", 0),
            )
            self._entries.append(entry)
            self._addr_to_entry[entry.address] = row

            ic = entry.instruction_count

            # Checkbox column
            check_item = QTableWidgetItem()
            is_auto = self._is_auto_named(entry.name)
            check_item.setCheckState(
                Qt.CheckState.Checked if (is_auto and not entry.is_import) else Qt.CheckState.Unchecked
            )
            check_item.setFlags(Qt.ItemFlag.ItemIsUserCheckable | Qt.ItemFlag.ItemIsEnabled)
            self._table.setItem(row, _COL_CHECK, check_item)

            # Address (numeric sort, store address in UserRole for lookup)
            addr_item = _NumericTableItem(f"0x{entry.address:X}", entry.address)
            addr_item.setFlags(Qt.ItemFlag.ItemIsEnabled | Qt.ItemFlag.ItemIsSelectable)
            addr_item.setData(Qt.ItemDataRole.UserRole, entry.address)
            addr_item.setToolTip(f"0x{entry.address:016X}")
            self._table.setItem(row, _COL_ADDR, addr_item)

            # Current name
            name_item = QTableWidgetItem(entry.name)
            name_item.setFlags(Qt.ItemFlag.ItemIsEnabled | Qt.ItemFlag.ItemIsSelectable)
            self._table.setItem(row, _COL_NAME, name_item)

            # Length (numeric sort)
            length_item = _NumericTableItem(str(ic) if ic else "0", ic)
            length_item.setFlags(Qt.ItemFlag.ItemIsEnabled | Qt.ItemFlag.ItemIsSelectable)
            length_item.setTextAlignment(Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter)
            self._table.setItem(row, _COL_LENGTH, length_item)

            # New name (initially empty)
            new_item = QTableWidgetItem("")
            new_item.setFlags(Qt.ItemFlag.ItemIsEnabled | Qt.ItemFlag.ItemIsSelectable)
            self._table.setItem(row, _COL_NEWNAME, new_item)

            # Status
            status_item = QTableWidgetItem("")
            status_item.setFlags(Qt.ItemFlag.ItemIsEnabled | Qt.ItemFlag.ItemIsSelectable)
            self._table.setItem(row, _COL_STATUS, status_item)

    def _finish_load(self) -> None:
        """Re-enable table features after load completes."""
        self._table.itemChanged.connect(self._on_item_changed)
        self._table.setSortingEnabled(True)
        self._loading = False
        self._update_selection_count()

    def update_job(self, address: int, new_name: str, status: str, error: str) -> None:
        """Update a row by address with new name, status, and optional error."""
        row = self._find_row_for_address(address)
        if row is None:
            return

        # Block signals to prevent sorting/item-change side-effects
        self._table.blockSignals(True)

        new_item = self._table.item(row, _COL_NEWNAME)
        if new_item:
            new_item.setText(new_name if new_name else "")

        status_item = self._table.item(row, _COL_STATUS)
        if status_item:
            display = error if error else status
            status_item.setText(display)
            color = _STATUS_COLORS.get(status, "#d4d4d4")
            from .qt_compat import QColor

            status_item.setForeground(QColor(color))

        self._table.blockSignals(False)

    def _find_row_for_address(self, address: int) -> int | None:
        """Find the current visual row for a given address (sort-safe)."""
        for row in range(self._table.rowCount()):
            addr_item = self._table.item(row, _COL_ADDR)
            if addr_item is not None and addr_item.data(Qt.ItemDataRole.UserRole) == address:
                return row
        return None

    def set_progress(self, current: int, total: int) -> None:
        """Update the progress bar and label."""
        if total > 0:
            self._progress.setMaximum(total)
            self._progress.setValue(current)
        else:
            self._progress.setMaximum(1)
            self._progress.setValue(0)
        self._progress_label.setText(f"{current} / {total}")

        # Enable undo if any work has been done
        self._undo_btn.setEnabled(current > 0)

        # Toggle buttons based on completion
        if current >= total and total > 0:
            self._start_btn.setEnabled(True)
            self._stop_btn.setEnabled(False)
            self._pause_btn.setEnabled(False)
            self._pause_btn.setText("Pause")
            self._paused = False

    def _on_cell_clicked(self, row: int, column: int) -> None:
        """Handle single-click: toggle checkboxes for multi-select."""
        if column == _COL_CHECK:
            selected_rows = {idx.row() for idx in self._table.selectionModel().selectedRows()}
            if len(selected_rows) > 1 and row in selected_rows:
                clicked_item = self._table.item(row, _COL_CHECK)
                if clicked_item is None:
                    return
                new_state = clicked_item.checkState()
                self._table.itemChanged.disconnect(self._on_item_changed)
                for r in selected_rows:
                    item = self._table.item(r, _COL_CHECK)
                    if item:
                        item.setCheckState(new_state)
                self._table.itemChanged.connect(self._on_item_changed)
                self._update_selection_count()

    def _on_cell_double_clicked(self, row: int, column: int) -> None:
        """Double-click on Address, Name, or New Name navigates to that function."""
        if column in (_COL_ADDR, _COL_NAME, _COL_NEWNAME):
            entry = self._entry_for_row(row)
            if entry is not None:
                self.seek_requested.emit(entry.address)

    def _on_pause_toggle(self) -> None:
        """Toggle pause/resume and update button text."""
        self._paused = not self._paused
        self._pause_btn.setText("Resume" if self._paused else "Pause")
        self.pause_requested.emit()

    def _on_stop(self) -> None:
        """Stop the running renamer engine."""
        self._stop_btn.setEnabled(False)
        self._pause_btn.setEnabled(False)
        self._pause_btn.setText("Pause")
        self._start_btn.setEnabled(True)
        self._paused = False
        self.cancel_requested.emit()

    def _entry_for_row(self, row: int) -> FunctionEntry | None:
        """Get the FunctionEntry for a visual table row (sort-safe)."""
        addr_item = self._table.item(row, _COL_ADDR)
        if addr_item is None:
            return None
        addr = addr_item.data(Qt.ItemDataRole.UserRole)
        if addr is None:
            return None
        idx = self._addr_to_entry.get(addr)
        return self._entries[idx] if idx is not None else None

    def _on_filter_changed(self) -> None:
        """Filter table rows based on text filter and combo selection."""
        if self._loading:
            return

        text = self._filter_edit.text().strip().lower()
        combo_idx = self._filter_combo.currentIndex()

        for row in range(self._table.rowCount()):
            entry = self._entry_for_row(row)
            if entry is None:
                continue
            name = entry.name.lower()

            # Text filter — match name or hex address
            text_match = not text or text in name or text in f"0x{entry.address:x}" or text in f"0x{entry.address:X}"

            # Combo filter
            combo_match = True
            if combo_idx == 1:  # Auto-named Only
                combo_match = self._is_auto_named(entry.name)
            elif combo_idx == 2:  # User-renamed
                combo_match = not self._is_auto_named(entry.name) and not entry.is_import
            elif combo_idx == 3:  # Imports
                combo_match = entry.is_import

            self._table.setRowHidden(row, not (text_match and combo_match))

    def _on_item_changed(self, item: QTableWidgetItem) -> None:
        """Track checkbox state changes."""
        if item.column() == _COL_CHECK:
            self._update_selection_count()

    def _get_selected_jobs(self) -> list[dict]:
        """Return list of dicts with address and current_name for checked rows."""
        jobs = []
        for row in range(self._table.rowCount()):
            check_item = self._table.item(row, _COL_CHECK)
            if check_item and check_item.checkState() == Qt.CheckState.Checked:
                entry = self._entry_for_row(row)
                if entry is not None:
                    jobs.append(
                        {
                            "address": entry.address,
                            "current_name": entry.name,
                        }
                    )
        return jobs

    def _batch_value(self) -> int:
        """Parse batch size from the text input, default 10."""
        try:
            return max(1, int(self._batch_input.text()))
        except (ValueError, TypeError):
            return 10

    def _concurrent_value(self) -> int:
        """Parse concurrent jobs from the text input, default 3."""
        try:
            return max(1, int(self._concurrent_input.text()))
        except (ValueError, TypeError):
            return 3

    def _on_start(self) -> None:
        """Collect selected functions and emit start_requested."""
        jobs = self._get_selected_jobs()
        if not jobs:
            return
        mode = "deep" if self._deep_radio.isChecked() else "quick"
        batch_size = self._batch_value()
        max_concurrent = self._concurrent_value()

        self._start_btn.setEnabled(False)
        self._stop_btn.setEnabled(True)
        self._pause_btn.setEnabled(True)
        self._pause_btn.setText("Pause")
        self._paused = False

        # Mark selected jobs as queued
        for job in jobs:
            self.update_job(job["address"], "", "queued", "")

        self.set_progress(0, len(jobs))
        self.start_requested.emit(jobs, mode, batch_size, max_concurrent)

    def _update_selection_count(self) -> None:
        """Update the selection count label with subagent estimation."""
        total = self._table.rowCount()
        selected = 0
        for row in range(total):
            item = self._table.item(row, _COL_CHECK)
            if item and item.checkState() == Qt.CheckState.Checked:
                selected += 1

        if self._deep_radio.isChecked() and selected > 0:
            self._selection_label.setText(f"{selected} / {total} selected \u2022 {selected} subagents")
        else:
            batch = self._batch_value()
            batches = (selected + batch - 1) // batch if selected > 0 else 0
            if selected > 0:
                self._selection_label.setText(f"{selected} / {total} selected \u2022 {batches} batch(es)")
            else:
                self._selection_label.setText(f"{selected} / {total} selected")

    def select_and_filter_address(self, address: int) -> None:
        """Filter to a specific address and check it — used by send_to_bulk_rename."""
        addr_str = f"0x{address:x}"
        self._filter_edit.setText(addr_str)

        row = self._find_row_for_address(address)
        if row is not None:
            self._table.itemChanged.disconnect(self._on_item_changed)
            item = self._table.item(row, _COL_CHECK)
            if item:
                item.setCheckState(Qt.CheckState.Checked)
            self._table.itemChanged.connect(self._on_item_changed)
            self._update_selection_count()

    @staticmethod
    def _is_auto_named(name: str) -> bool:
        """Heuristic: detect auto-generated function names."""
        prefixes = ("sub_", "fn_", "loc_", "j_", "nullsub_", "unknown_", "FUN_")
        return name.startswith(prefixes)

"""Shared Rikugan panel widget used by host-specific wrappers."""

from __future__ import annotations

import os
import queue
import re
import threading
import time
from collections.abc import Callable
from typing import Any

from ..agent.mutation import MutationRecord
from ..agent.turn import TurnEvent, TurnEventType
from ..core.config import RikuganConfig
from ..core.host import get_current_address
from ..core.logging import log_debug, log_error, log_info, log_warning
from ..core.types import Role
from ..providers.auth_cache import resolve_auth_cached
from ..providers.auth_compat import apply_keychain_consent
from ..providers.registry import ProviderRegistry
from .binary_summary import BinarySummary, build_binary_summary, parse_total_count
from .chat_view import ChatView
from .composer import Composer
from .context_bar import ContextBar
from .mutation_log_view import MutationLogPanel
from .panel_header import PanelHeader
from .qt_compat import (
    QCheckBox,
    QDialog,
    QDialogButtonBox,
    QFileDialog,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QListWidget,
    QListWidgetItem,
    QMenu,
    QMessageBox,
    QSize,
    QSizePolicy,
    QSplitter,
    QStackedWidget,
    Qt,
    QTabBar,
    QTabWidget,
    QTimer,
    QToolButton,
    QVBoxLayout,
    QWidget,
    qt_flags,
    qt_run,
)
from .styles import (
    build_chat_drawer_stylesheet,
    build_chat_sidebar_stylesheet,
    build_chat_view_stylesheet,
    build_mode_bar_stylesheet,
    build_panel_header_stylesheet,
    build_theme_stylesheet,
    maybe_host_stylesheet,
)
from .tool_widgets import _SharedSpinnerTimer
from .tools_panel import ToolsPanel
from .welcome_view import WelcomeView, build_suggestions

_TOOL_RESULT_TRUNCATE_CHARS = 2000

# Below this width the chat list costs more than half the panel, so it becomes
# an overlay drawer instead of a splitter column.
_WIDE_LAYOUT_MIN_WIDTH = 620
_DRAWER_MAX_WIDTH = 250
_DRAWER_WIDTH_RATIO = 0.72

_BINARY_SUMMARY_MAX_ATTEMPTS = 20

# Upper bound on one drain pass of a finished runner. The queue holds at most
# 500 events, so this empties it without risking an unbounded loop.
_MAX_DRAIN_EVENTS = 1000

_PLACEHOLDER_IDLE = "Ask about this binary\u2026"
_PLACEHOLDER_RUNNING = "Rikugan is working\u2026 press Enter to queue a follow-up."
_PLACEHOLDER_APPROVAL = "Use the Approve/Reject buttons above to continue."


def should_use_drawer(width: int) -> bool:
    """Whether the chat list should float over the chat instead of sharing it."""
    return width < _WIDE_LAYOUT_MIN_WIDTH


_SANITIZER_TAG_RE = re.compile(
    r"^\[The following is (?:a tool execution result|output from an EXTERNAL MCP server)"
    r"[^\]]*\]\n?",
    re.MULTILINE,
)
_SANITIZER_WRAP_RE = re.compile(
    r"<(?:tool_result|mcp_result|binary_data|persistent_memory|skill)\b[^>]*>\n?"
    r"|</(?:tool_result|mcp_result|binary_data|persistent_memory|skill)>\n?",
)
_FUNCTION_PAGE_HEADER_RE = re.compile(r"^Functions\s+\d+[\-\u2013]\d+\s+of\s+([^:]+):")
_FUNCTION_ROW_RE = re.compile(r"\s*0x([0-9a-fA-F]+)\s+(.+)")
_FUNCTION_INFO_NAME_RE = re.compile(r"^Name:\s+(.+)$", re.MULTILINE)
_FUNCTION_INFO_ADDRESS_RE = re.compile(r"^Address:\s+0x([0-9a-fA-F]+)", re.MULTILINE)
_FUNCTION_INFO_INSTRUCTIONS_RE = re.compile(r"^Instructions:\s+(\d+)", re.MULTILINE)
_HEX_ADDRESS_QUERY_RE = re.compile(r"0x[0-9a-fA-F]+")


def _parse_function_page(raw: str) -> tuple[list[dict], int | str | None]:
    """Parse a list_functions page into renamer rows and an optional total hint."""
    functions = []
    total_hint: int | str | None = None
    for line in raw.splitlines():
        header = _FUNCTION_PAGE_HEADER_RE.match(line)
        if header:
            total_text = header.group(1).strip()
            try:
                total_hint = int(total_text)
            except ValueError:
                total_hint = total_text or None
            continue

        match = _FUNCTION_ROW_RE.match(line)
        if match:
            functions.append(
                {
                    "address": int(match.group(1), 16),
                    "name": match.group(2).strip(),
                    "is_import": False,
                    "instruction_count": 0,
                }
            )
    return functions, total_hint


def _parse_function_info_result(raw: str) -> dict | None:
    """Parse get_function_info output into one renamer row."""
    name_match = _FUNCTION_INFO_NAME_RE.search(raw)
    address_match = _FUNCTION_INFO_ADDRESS_RE.search(raw)
    if not name_match or not address_match:
        return None

    instructions_match = _FUNCTION_INFO_INSTRUCTIONS_RE.search(raw)
    instruction_count = int(instructions_match.group(1)) if instructions_match else 0
    return {
        "address": int(address_match.group(1), 16),
        "name": name_match.group(1).strip(),
        "is_import": False,
        "instruction_count": instruction_count,
    }


def _strip_sanitizer_tags(text: str) -> str:
    """Remove sanitization wrappers added for the LLM from exported content."""
    text = _SANITIZER_TAG_RE.sub("", text)
    text = _SANITIZER_WRAP_RE.sub("", text)
    return text.strip()


_TOOL_LANG_MAP = {
    "execute_python": "python",
    "decompile_function": "c",
    "get_il": "c",
    "declare_c_type": "c",
    "define_types": "c",
    "set_function_prototype": "c",
    "fetch_disassembly": "x86asm",
}


def _export_detect_lang(text: str, tool_name: str = "", arg_key: str = "") -> str:
    """Detect markdown language hint from content heuristics and tool/arg context."""
    if arg_key in ("code", "python"):
        return "python"
    if arg_key in ("c_code", "c_declaration", "prototype"):
        return "c"
    if tool_name in _TOOL_LANG_MAP:
        return _TOOL_LANG_MAP[tool_name]

    sample = text[:_TOOL_RESULT_TRUNCATE_CHARS]
    if re.search(r"^[0-9a-fA-F]{8,16}\s+([0-9a-fA-F]{2}\s+){4,}", sample, re.M):
        return "text"

    asm_pat = r"(?:mov|lea|push|pop|call|ret|jmp|je|jne|jz|jnz|cmp|test|xor|add|sub|nop|int)\s"
    if re.search(asm_pat, sample, re.I) and re.search(r"0x[0-9a-fA-F]+", sample):
        return "x86asm"

    c_indicators = 0
    if re.search(r"\b(void|int|char|uint\d+_t|int\d+_t|struct|enum|typedef)\b", sample):
        c_indicators += 1
    if re.search(r"[{};]", sample):
        c_indicators += 1
    if re.search(r"\b(if|while|for|return|switch)\s*\(", sample):
        c_indicators += 1
    if c_indicators >= 2:
        return "c"

    if re.search(r"^(def |class |import |from .+ import |print\()", sample, re.M):
        return "python"

    return ""


def _export_format_tool_args(tc) -> str:
    """Format tool call arguments as markdown with per-argument code blocks."""
    parts = []
    for k, v in tc.arguments.items():
        if isinstance(v, str) and ("\n" in v or len(v) > 80):
            lang = _export_detect_lang(v, tc.name, k)
            parts.append(f"  - `{k}`:\n\n```{lang}\n{v}\n```\n")
        else:
            parts.append(f"  - `{k}`: `{v!r}`")
    return "\n".join(parts)


def _export_format_tool_result(tr) -> str:
    """Format tool result content as a markdown code block."""
    content = _strip_sanitizer_tags(tr.content)
    if len(content) > _TOOL_RESULT_TRUNCATE_CHARS:
        content = content[:_TOOL_RESULT_TRUNCATE_CHARS] + "\n... (truncated)"
    lang = _export_detect_lang(content, tr.name)
    return f"```{lang}\n{content}\n```"


def _export_format_subagent_log(messages) -> str:
    """Format a subagent's message log as a collapsible markdown section."""
    tool_count = sum(len(m.tool_calls) for m in messages if m.role == Role.ASSISTANT)
    parts = [
        f"<details>\n<summary>Subagent Log ({tool_count} tool calls)</summary>\n",
    ]
    for msg in messages:
        if msg.role == Role.USER:
            parts.append(f"> **Task**: {msg.content}\n")
        elif msg.role == Role.ASSISTANT:
            if msg.content:
                parts.append(f"> **Subagent**:\n> {msg.content}\n")
            for tc in msg.tool_calls:
                parts.append(f"> **Tool call**: `{tc.name}`\n")
                parts.append(f"> {_export_format_tool_args(tc)}\n")
        elif msg.role == Role.TOOL:
            for tr in msg.tool_results:
                status = "Error" if tr.is_error else "Result"
                parts.append(f"> **{status}** (`{tr.name}`):\n")
                parts.append(f"> {_export_format_tool_result(tr)}\n")
    parts.append("</details>\n")
    return "\n".join(parts)


class _AddButtonTabBar(QTabBar):
    """Tab bar with an integrated '+' button positioned after the last tab."""

    def __init__(self, parent=None):
        super().__init__(parent)
        self._add_tab_callback: Callable[[], None] | None = None
        self._export_tab_callback: Callable[[int], None] | None = None
        self._fork_tab_callback: Callable[[int], None] | None = None
        self.setContextMenuPolicy(Qt.ContextMenuPolicy.CustomContextMenu)
        self.customContextMenuRequested.connect(self._show_context_menu)
        self._add_btn = QToolButton(self)
        self._add_btn.setText("+")
        self._add_btn.setAutoRaise(True)
        self._add_btn.setFixedSize(20, 20)
        self._add_btn.setStyleSheet(
            maybe_host_stylesheet(
                "QToolButton { color: #d4d4d4; font-size: 14px; font-weight: bold; "
                "border: none; background: transparent; }"
                "QToolButton:hover { background: #3c3c3c; border-radius: 3px; }"
            )
        )
        self._add_btn.clicked.connect(self._handle_add_tab)

    def set_add_tab_callback(self, callback: Callable[[], None] | None) -> None:
        self._add_tab_callback = callback

    def set_export_tab_callback(self, callback: Callable[[int], None] | None) -> None:
        self._export_tab_callback = callback

    def set_fork_tab_callback(self, callback: Callable[[int], None] | None) -> None:
        self._fork_tab_callback = callback

    def _handle_add_tab(self) -> None:
        if self._add_tab_callback is not None:
            self._add_tab_callback()

    def _show_context_menu(self, pos):
        index = self.tabAt(pos)
        if index < 0:
            return
        menu = QMenu(self)
        export_action = menu.addAction("Export Chat")
        fork_action = menu.addAction("Fork Session")
        action = qt_run(menu, self.mapToGlobal(pos))
        if action == export_action and self._export_tab_callback is not None:
            self._export_tab_callback(index)
        elif action == fork_action and self._fork_tab_callback is not None:
            self._fork_tab_callback(index)

    def tabInserted(self, index):
        super().tabInserted(index)
        self._reposition()

    def tabRemoved(self, index):
        super().tabRemoved(index)
        self._reposition()

    def resizeEvent(self, event):
        super().resizeEvent(event)
        self._reposition()

    def _reposition(self):
        count = self.count()
        if count > 0:
            rect = self.tabRect(count - 1)
            y = (self.height() - self._add_btn.height()) // 2
            self._add_btn.move(rect.right() + 2, max(0, y))
        else:
            self._add_btn.move(0, 0)


_BADGE_CSS: dict[str, str] = {
    "Running": "color:#4ec9b0; background:rgba(78,201,176,0.15); border:1px solid rgba(78,201,176,0.4);",
    "Approval": "color:#d7ba7d; background:rgba(215,186,125,0.15); border:1px solid rgba(215,186,125,0.4);",
    "Queued": "color:#9cdcfe; background:rgba(156,220,254,0.12); border:1px solid rgba(156,220,254,0.35);",
    "Error": "color:#f87171; background:rgba(248,113,113,0.15); border:1px solid rgba(248,113,113,0.4);",
    "Cancelled": "color:#808080; background:rgba(128,128,128,0.10); border:1px solid rgba(128,128,128,0.3);",
}


class ChatThreadRow(QWidget):
    """Compact row widget for one chat session."""

    HEIGHT = 48

    def __init__(self, parent: QWidget = None):
        super().__init__(parent)
        self.setMinimumHeight(self.HEIGHT)
        layout = QHBoxLayout(self)
        layout.setContentsMargins(8, 5, 8, 5)
        layout.setSpacing(8)

        text_layout = QVBoxLayout()
        text_layout.setContentsMargins(0, 0, 0, 0)
        text_layout.setSpacing(1)
        self._title = QLabel(self)
        self._title.setObjectName("chat_row_title")
        self._title.setSizePolicy(QSizePolicy.Policy.Ignored, QSizePolicy.Policy.Preferred)
        self._title.setMinimumWidth(0)
        self._detail = QLabel(self)
        self._detail.setObjectName("chat_row_detail")
        self._detail.setSizePolicy(QSizePolicy.Policy.Ignored, QSizePolicy.Policy.Preferred)
        self._detail.setMinimumWidth(0)
        text_layout.addWidget(self._title)
        text_layout.addWidget(self._detail)
        layout.addLayout(text_layout, 1)

        self._badge = QLabel(self)
        self._badge.setObjectName("chat_row_badge")
        self._badge.hide()
        layout.addWidget(self._badge)

    def set_chat(self, title: str, detail: str, badge: str) -> None:
        self._title.setText(title)
        self._title.setToolTip(title)
        self._detail.setText(detail)
        self._detail.setVisible(bool(detail))
        self._badge.setText(badge)
        self._badge.setVisible(bool(badge))
        if badge:
            key = badge.split()[0].capitalize()
            css = _BADGE_CSS.get(key, "")
            self._badge.setStyleSheet(f"QLabel {{ {css} border-radius: 3px; padding: 1px 5px; font-size: 10px; }}")


class ChatThreadList(QWidget):
    """Chat list shown as an overlay drawer (narrow) or a splitter column (wide).

    Per-chat actions live in the row's context menu and the header overflow
    menu rather than a button row: at sidebar width four buttons clip their own
    labels.
    """

    _ROLE_TAB_ID = 32
    _UNGROUPED = ""

    def __init__(self, parent: QWidget = None):
        super().__init__(parent)
        self._select_callback: Callable[[str], None] | None = None
        self._new_callback: Callable[[], None] | None = None
        self._delete_callback: Callable[[str], None] | None = None
        self._fork_callback: Callable[[str], None] | None = None
        self._export_callback: Callable[[str], None] | None = None
        self._close_callback: Callable[[], None] | None = None
        self._items: dict[str, QListWidgetItem] = {}
        self._rows: dict[str, ChatThreadRow] = {}
        self._titles: dict[str, str] = {}
        self._details: dict[str, str] = {}
        self._statuses: dict[str, str] = {}
        self._groups: dict[str, str] = {}
        self._group_order: list[str] = []
        self._group_members: dict[str, list[str]] = {}
        self._group_items: dict[str, QListWidgetItem] = {}
        self._selected_tab_id: str | None = None
        self._suppress_select = False
        self.setObjectName("chat_sidebar")
        self.setProperty("drawer", "false")
        # A plain QWidget ignores a stylesheet background unless it is told to
        # style its own background — as an overlay it must be opaque.
        self.setAttribute(Qt.WidgetAttribute.WA_StyledBackground, True)

        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(4)

        layout.addWidget(self._build_header())

        self._search = QLineEdit(self)
        self._search.setObjectName("chat_search")
        self._search.setPlaceholderText("Search chats")
        self._search.textChanged.connect(self._apply_filter)
        layout.addWidget(self._search)

        self._list = QListWidget(self)
        self._list.setObjectName("chat_thread_list")
        self._list.setContextMenuPolicy(Qt.ContextMenuPolicy.CustomContextMenu)
        self._list.currentItemChanged.connect(self._on_current_changed)
        self._list.itemClicked.connect(self._on_item_clicked)
        self._list.itemActivated.connect(self._on_item_clicked)
        self._list.customContextMenuRequested.connect(self._show_menu)
        layout.addWidget(self._list, 1)

        layout.addWidget(self._build_footer())

        self.setMinimumWidth(224)
        self.setMaximumWidth(340)
        self.refresh_theme()

    def _build_header(self) -> QWidget:
        header = QWidget(self)
        header_layout = QHBoxLayout(header)
        header_layout.setContentsMargins(8, 7, 6, 4)
        header_layout.setSpacing(4)
        title = QLabel("Chats", header)
        title.setObjectName("chat_sidebar_title")
        header_layout.addWidget(title)
        header_layout.addStretch()
        self._new_btn = self._make_icon_button("+", "New chat", self._on_new, parent=header)
        header_layout.addWidget(self._new_btn)
        # Always offered, in both layouts: collapsing the list used to be
        # possible only through the header's chat name, which reads as a
        # switcher rather than a hide control.
        self._close_btn = self._make_icon_button(
            "\u00ab",
            "Hide chat list",
            self._on_close,
            parent=header,
        )
        header_layout.addWidget(self._close_btn)
        return header

    def _build_footer(self) -> QWidget:
        footer = QWidget(self)
        footer.setObjectName("chat_sidebar_footer")
        footer_layout = QHBoxLayout(footer)
        footer_layout.setContentsMargins(6, 5, 6, 6)
        footer_layout.setSpacing(4)
        self._settings_btn = self._make_chip_button("Settings", "Open settings", self._on_settings, footer)
        self._export_btn = self._make_chip_button(
            "Export",
            "Export the selected chat",
            self._on_export_selected,
            footer,
        )
        footer_layout.addWidget(self._settings_btn, 1)
        footer_layout.addWidget(self._export_btn, 1)
        return footer

    def refresh_theme(self) -> None:
        """Re-derive the list's colors from the live host palette.

        The drawer rules must be applied together with the base sheet: Qt takes
        the last setStyleSheet call wholesale, so applying only one of the two
        silently drops the other (including the drawer's opaque background).
        """
        self.setStyleSheet(build_chat_sidebar_stylesheet(self) + build_chat_drawer_stylesheet(self))

    def _make_icon_button(
        self,
        text: str,
        tooltip: str,
        callback: Callable[[], None],
        parent: QWidget | None = None,
    ) -> QToolButton:
        button = QToolButton(parent or self)
        button.setObjectName("header_icon")
        button.setText(text)
        button.setToolTip(tooltip)
        button.setFixedSize(24, 24)
        button.setStyleSheet(build_panel_header_stylesheet(self))
        button.clicked.connect(callback)
        return button

    def _make_chip_button(
        self,
        text: str,
        tooltip: str,
        callback: Callable[[], None],
        parent: QWidget | None = None,
    ) -> QToolButton:
        button = QToolButton(parent or self)
        button.setObjectName("drawer_chip")
        button.setText(text)
        button.setToolTip(tooltip)
        button.clicked.connect(callback)
        return button

    def set_callbacks(
        self,
        select: Callable[[str], None],
        new: Callable[[], None],
        delete: Callable[[str], None],
        fork: Callable[[str], None],
        export: Callable[[str], None],
    ) -> None:
        self._select_callback = select
        self._new_callback = new
        self._delete_callback = delete
        self._fork_callback = fork
        self._export_callback = export

    def set_close_callback(self, callback: Callable[[], None] | None) -> None:
        self._close_callback = callback

    def set_drawer_mode(self, drawer: bool) -> None:
        """Draw the drawer edge when floating over the chat."""
        self.setProperty("drawer", "true" if drawer else "false")
        self.style().unpolish(self)
        self.style().polish(self)

    # --- chat entries ---------------------------------------------------

    def add_chat(self, tab_id: str, title: str, detail: str = "", group: str = "") -> None:
        item = QListWidgetItem()
        item.setData(self._ROLE_TAB_ID, tab_id)
        row = ChatThreadRow(self._list)
        self._items[tab_id] = item
        self._rows[tab_id] = row
        self._titles[tab_id] = title
        self._details[tab_id] = detail
        self._statuses.setdefault(tab_id, "idle")
        self._groups[tab_id] = group
        self._list.insertItem(self._register_in_group(tab_id, group), item)
        self._list.setItemWidget(item, row)
        self._refresh_item(tab_id)

    def remove_chat(self, tab_id: str) -> None:
        item = self._items.pop(tab_id, None)
        row_widget = self._rows.pop(tab_id, None)
        self._titles.pop(tab_id, None)
        self._details.pop(tab_id, None)
        self._statuses.pop(tab_id, None)
        group = self._groups.pop(tab_id, self._UNGROUPED)
        if item is None:
            return
        if row_widget is not None:
            row_widget.deleteLater()
        row = self._list.row(item)
        if row >= 0:
            self._list.takeItem(row)
        self._unregister_from_group(tab_id, group)

    def clear(self) -> None:
        """Remove every chat entry (e.g. when switching databases)."""
        for tab_id in list(self._items.keys()):
            self.remove_chat(tab_id)
        self._selected_tab_id = None

    def select_chat(self, tab_id: str) -> None:
        item = self._items.get(tab_id)
        if item is not None:
            self._suppress_select = True
            self._list.setCurrentItem(item)
            self._suppress_select = False
            self._selected_tab_id = tab_id

    def update_chat(self, tab_id: str, title: str | None = None, detail: str | None = None) -> None:
        if title is not None:
            self._titles[tab_id] = title
        if detail is not None:
            self._details[tab_id] = detail
        self._refresh_item(tab_id)

    def set_status(self, tab_id: str, status: str, pending: int = 0) -> None:
        self._statuses[tab_id] = f"{status}:{pending}" if pending else status
        self._refresh_item(tab_id)

    # --- grouping -------------------------------------------------------

    def _register_in_group(self, tab_id: str, group: str) -> int:
        """Add *tab_id* to *group* and return the list row it belongs at."""
        if group not in self._group_members:
            self._group_members[group] = []
            self._group_order.append(group)
            if group != self._UNGROUPED:
                self._insert_group_header(group)
        self._group_members[group].append(tab_id)
        return self._group_end_row(group)

    def _insert_group_header(self, group: str) -> None:
        header_item = QListWidgetItem()
        header_item.setFlags(Qt.ItemFlag.NoItemFlags)
        label = QLabel(group, self._list)
        label.setObjectName("chat_group_header")
        label.setToolTip(group)
        header_item.setSizeHint(QSize(0, 22))
        self._group_items[group] = header_item
        self._list.insertItem(self._group_start_row(group), header_item)
        self._list.setItemWidget(header_item, label)

    def _group_start_row(self, group: str) -> int:
        row = 0
        for name in self._group_order:
            if name == group:
                return row
            row += len(self._group_members.get(name, ()))
            if name != self._UNGROUPED:
                row += 1  # its header
        return row

    def _group_end_row(self, group: str) -> int:
        row = self._group_start_row(group) + len(self._group_members.get(group, ()))
        if group != self._UNGROUPED:
            row += 1  # its header
        return max(0, row - 1)

    def _unregister_from_group(self, tab_id: str, group: str) -> None:
        members = self._group_members.get(group)
        if members is None:
            return
        if tab_id in members:
            members.remove(tab_id)
        if members:
            return
        header_item = self._group_items.pop(group, None)
        if header_item is not None:
            header_row = self._list.row(header_item)
            if header_row >= 0:
                self._list.takeItem(header_row)
        self._group_members.pop(group, None)
        if group in self._group_order:
            self._group_order.remove(group)

    # --- rendering ------------------------------------------------------

    def _refresh_item(self, tab_id: str) -> None:
        item = self._items.get(tab_id)
        if item is None:
            return
        title = self._titles.get(tab_id, "Untitled")
        detail = self._details.get(tab_id, "")
        status_raw = self._statuses.get(tab_id, "idle")
        status, _, pending = status_raw.partition(":")
        badge = {
            "running": "Running",
            "approval": "Approval",
            "queued": f"{pending} queued" if pending else "Queued",
            "error": "Error",
            "cancelled": "Cancelled",
        }.get(status, "")
        # The custom row widget owns visible text. Leaving fallback item text
        # set makes QListWidget paint a second, overlapping title in some hosts.
        item.setText("")
        item.setToolTip(title)
        row = self._rows.get(tab_id)
        height = ChatThreadRow.HEIGHT
        if row is not None:
            row.set_chat(title, detail, badge)
            # Two lines of the host font, so larger UI fonts are not clipped.
            height = max(height, row.sizeHint().height())
        item.setSizeHint(QSize(0, height))
        self._apply_filter()

    def _apply_filter(self) -> None:
        needle = self._search.text().strip().lower()
        visible_per_group: dict[str, int] = {}
        for tab_id, item in self._items.items():
            haystack = f"{self._titles.get(tab_id, '')} {self._details.get(tab_id, '')}".lower()
            hidden = bool(needle and needle not in haystack)
            item.setHidden(hidden)
            group = self._groups.get(tab_id, self._UNGROUPED)
            visible_per_group[group] = visible_per_group.get(group, 0) + (0 if hidden else 1)
        for group, header_item in self._group_items.items():
            header_item.setHidden(visible_per_group.get(group, 0) == 0)

    # --- actions --------------------------------------------------------

    def _on_new(self) -> None:
        if self._new_callback is not None:
            self._new_callback()

    def _on_close(self) -> None:
        if self._close_callback is not None:
            self._close_callback()

    def _on_current_changed(self, current, _previous) -> None:
        if current is None or self._select_callback is None or self._suppress_select:
            return
        tab_id = current.data(self._ROLE_TAB_ID)
        if tab_id:
            self._selected_tab_id = str(tab_id)
            self._select_callback(self._selected_tab_id)

    def _on_item_clicked(self, item) -> None:
        if item is None or self._select_callback is None:
            return
        tab_id = item.data(self._ROLE_TAB_ID)
        if tab_id:
            self._selected_tab_id = str(tab_id)
            self._select_callback(self._selected_tab_id)

    def _on_fork_selected(self) -> None:
        if self._selected_tab_id and self._fork_callback is not None:
            self._fork_callback(self._selected_tab_id)

    def _on_export_selected(self) -> None:
        if self._selected_tab_id and self._export_callback is not None:
            self._export_callback(self._selected_tab_id)

    def _on_delete_selected(self) -> None:
        if self._selected_tab_id and self._delete_callback is not None:
            self._delete_callback(self._selected_tab_id)

    def _on_settings(self) -> None:
        parent = self.parent()
        while parent is not None:
            handler = getattr(parent, "_on_settings", None)
            if callable(handler):
                handler()
                return
            parent = parent.parent()

    def _show_menu(self, pos) -> None:
        item = self._list.itemAt(pos)
        if item is None:
            return
        tab_id = item.data(self._ROLE_TAB_ID)
        if not tab_id:  # a group header
            return
        tab_id = str(tab_id)
        menu = QMenu(self)
        fork_action = menu.addAction("Fork Chat")
        export_action = menu.addAction("Export Chat")
        delete_action = menu.addAction("Delete Chat")
        action = qt_run(menu, self._list.mapToGlobal(pos))
        if action == fork_action and self._fork_callback is not None:
            self._fork_callback(tab_id)
        elif action == export_action and self._export_callback is not None:
            self._export_callback(tab_id)
        elif action == delete_action and self._delete_callback is not None:
            self._delete_callback(tab_id)


class _DrawerScrim(QWidget):
    """Dimmed overlay behind the chat drawer; a click closes it."""

    def __init__(self, parent: QWidget = None):
        super().__init__(parent)
        self.setObjectName("chat_drawer_scrim")
        self.setAttribute(Qt.WidgetAttribute.WA_StyledBackground, True)
        self.setStyleSheet(build_chat_drawer_stylesheet(self))
        self._callback: Callable[[], None] | None = None

    def set_dismiss_callback(self, callback: Callable[[], None] | None) -> None:
        self._callback = callback

    def mousePressEvent(self, event) -> None:
        super().mousePressEvent(event)
        if self._callback is not None:
            self._callback()


class RikuganPanelCore(QWidget):
    """Host-agnostic chat panel widget."""

    def __init__(
        self,
        controller_factory: Callable[[RikuganConfig], Any],
        ui_hooks_factory: Callable[[Callable[[], Any]], Any] | None = None,
        tools_form_factory: Callable[..., Any] | None = None,
        parent: QWidget = None,
    ):
        super().__init__(parent)
        self._config = RikuganConfig.load_or_create()
        self._dependency_warnings = ProviderRegistry().dependency_warnings()
        log_debug(
            f"Config loaded: provider={self._config.provider.name} model={self._config.provider.model}",
        )
        for warning in self._dependency_warnings:
            log_warning(f"Dependency warning: {warning}")
        if self._config.has_encrypted_keys():
            self._prompt_decryption_password()
        self._ctrl = controller_factory(self._config)
        self._poll_timer: QTimer | None = None
        self._polling = False
        self._pending_answer = False
        self._awaiting_button_approval = False
        self._pending_answer_tabs: set[str] = set()
        self._awaiting_approval_tabs: set[str] = set()
        self._is_shutdown = False
        self._ui_hooks_factory = ui_hooks_factory
        self._ui_hooks = None
        self._tools_form_factory = tools_form_factory
        self._tools_form: Any = None  # IDA PluginForm wrapper (if available)

        # Tab-to-ChatView mapping
        self._chat_views: dict[str, ChatView] = {}
        self._pending_restore_messages: dict[str, list] = {}
        self._chat_area_stack: QStackedWidget | None = None
        self._chat_sidebar: ChatThreadList | None = None
        self._chat_column: QWidget | None = None
        self._panel_header: PanelHeader | None = None
        self._composer: Composer | None = None
        self._scrim: _DrawerScrim | None = None
        self._drawer_mode = False
        # The chat list starts collapsed and only the user opens it, at any
        # panel width. It used to appear on its own whenever the panel grew.
        self._sidebar_open = False
        self._welcome_views: dict[str, WelcomeView] = {}
        self._binary_summary = BinarySummary()
        self._binary_summary_loaded = False
        self._binary_summary_attempts = 0
        self._native_mcp_probe: queue.Queue | None = None
        self._native_mcp_timer: QTimer | None = None
        self._native_mcp_available = False
        self._native_mcp_active = False
        self._native_mcp_user_initiated = False
        self._binary_queue: queue.Queue | None = None
        self._binary_timer: QTimer | None = None
        self._tab_status: dict[str, str] = {}
        # Last values pushed into each sidebar row, so the 50ms poll can
        # skip rows that have not changed.
        self._sidebar_rows: dict[str, tuple] = {}
        self._context_bar: ContextBar | None = None
        self._mutation_panel: MutationLogPanel | None = None
        self._skills_refresh_timer: QTimer | None = None
        self._restore_timer: QTimer | None = None
        self._restore_queue: queue.Queue | None = None

        self._check_oauth_consent()

        def _warm_oauth() -> None:
            try:
                resolve_auth_cached()
            except Exception as e:
                log_debug(f"OAuth warm-up failed: {e}")

        threading.Thread(target=_warm_oauth, daemon=True).start()
        self._build_ui()

    def _prompt_decryption_password(self) -> None:
        """Prompt for the encryption password at session start."""
        from .qt_compat import QDialog, QDialogButtonBox, QLabel, QLineEdit, QMessageBox, QVBoxLayout

        for _attempt in range(3):
            dlg = QDialog()
            dlg.setWindowTitle("Rikugan — Encrypted API Keys")
            dlg.setMinimumWidth(350)
            layout = QVBoxLayout(dlg)
            layout.addWidget(QLabel("Enter password to decrypt API keys:"))
            pw_edit = QLineEdit()
            pw_edit.setEchoMode(QLineEdit.EchoMode.Password)
            pw_edit.setPlaceholderText("Password")
            layout.addWidget(pw_edit)
            buttons = QDialogButtonBox(
                qt_flags(
                    QDialogButtonBox.StandardButton.Ok,
                    QDialogButtonBox.StandardButton.Cancel,
                ),
            )
            buttons.accepted.connect(dlg.accept)
            buttons.rejected.connect(dlg.reject)
            layout.addWidget(buttons)

            if qt_run(dlg) != QDialog.DialogCode.Accepted:
                break  # user cancelled — keys stay empty
            if self._config.decrypt_stored_keys(pw_edit.text()):
                log_debug("API keys decrypted successfully")
                return
            QMessageBox.warning(None, "Wrong Password", "Incorrect password. Please try again.")
        log_debug("API key decryption skipped or failed — keys will be empty")

    def _check_oauth_consent(self) -> None:
        """Apply persisted OAuth consent to the auth cache.

        The consent dialog itself is only shown from the settings checkbox
        (``_on_oauth_toggled``).  This method just restores the persisted
        state so the warm-up thread knows whether keychain autoload is
        allowed.
        """
        apply_keychain_consent(self._config.oauth_consent_accepted)

    def _ensure_skills_refresh_timer(self) -> None:
        """Refresh skill autocomplete once background discovery completes."""
        if self._skills_refresh_timer is not None:
            return
        self._skills_refresh_timer = QTimer(self)
        self._skills_refresh_timer.setInterval(300)
        self._skills_refresh_timer.timeout.connect(self._refresh_skill_slugs)
        self._skills_refresh_timer.start()

    def _stop_skills_refresh_timer(self) -> None:
        if self._skills_refresh_timer is None:
            return
        self._skills_refresh_timer.stop()
        try:
            self._skills_refresh_timer.timeout.disconnect(self._refresh_skill_slugs)
        except (RuntimeError, TypeError) as e:
            log_debug(f"skills refresh timer disconnect failed: {e}")
        self._skills_refresh_timer.deleteLater()
        self._skills_refresh_timer = None

    def _refresh_skill_slugs(self) -> None:
        if self._is_shutdown:
            self._stop_skills_refresh_timer()
            return
        slugs = self._ctrl.skill_slugs
        if slugs:
            self._input_area.set_skill_slugs(slugs)
            self._refresh_all_welcomes()
            self._stop_skills_refresh_timer()
            return
        if getattr(self._ctrl, "runtime_ready", False):
            # Runtime init completed but no skills found; stop polling.
            self._stop_skills_refresh_timer()

    def _build_ui(self) -> None:
        self.setObjectName("rikugan_panel")
        self.setStyleSheet(build_theme_stylesheet(self))

        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(0)

        # Header: identity mark, chat switcher, new chat, overflow menu.
        self._panel_header = PanelHeader()
        self._panel_header.set_callbacks(
            switcher=self._toggle_drawer,
            new=self._on_new_tab,
            fork=self._on_fork_current,
            export=self._on_export_current,
            delete=self._on_delete_current,
            settings=self._on_settings,
            mutations=self._on_toggle_mutation_log,
        )
        self._panel_header.set_native_mcp_callback(self._toggle_native_mcp)
        self._panel_header.set_native_mcp_available(self._ctrl.host_name == "Binary Ninja")
        layout.addWidget(self._panel_header)

        # Top-level mode switcher: Chat | Tools.
        # Hosts may optionally provide tools in a separate form.
        self._mode_bar = QTabBar()
        self._mode_bar.setObjectName("mode_bar")
        self._mode_bar.setStyleSheet(build_mode_bar_stylesheet(self._mode_bar))
        self._mode_bar.setExpanding(False)
        self._mode_bar.setDrawBase(False)
        self._mode_bar.addTab("Chat")
        self._mode_bar.addTab("Tools")
        self._mode_bar.currentChanged.connect(self._on_mode_changed)
        if self._tools_form_factory is not None:
            self._mode_bar.setVisible(False)
        layout.addWidget(self._mode_bar)

        # Stacked content: page 0 = chat, page 1 = tools
        self._mode_stack = QStackedWidget()
        layout.addWidget(self._mode_stack, 1)

        # --- Page 0: Chat ---
        chat_page = QWidget()
        chat_layout = QVBoxLayout(chat_page)
        chat_layout.setContentsMargins(0, 0, 0, 0)
        chat_layout.setSpacing(0)
        self._build_tab_widget()
        self._build_main_splitter(chat_layout)
        if self._config.dont_auto_load_chats:
            # Open to the placeholder; chats are listed in the sidebar by the
            # restore pass and materialized on demand when selected.
            self._show_placeholder()
        else:
            self._create_tab(self._ctrl.active_tab_id, "Untitled")
        self._mode_stack.addWidget(chat_page)

        # --- Page 1: Tools (lazily populated on first switch) ---
        self._tools_panel: ToolsPanel | None = ToolsPanel()
        self._tools_panel.hide_header()
        if self._tools_form_factory is not None:
            # Separate tools-form hosts keep a lightweight placeholder in the
            # stack so page indices stay stable while tools live elsewhere.
            _tools_placeholder = QWidget()
            self._mode_stack.addWidget(_tools_placeholder)
        else:
            # Binary Ninja: embed directly in the mode stack.
            self._mode_stack.addWidget(self._tools_panel)
        self._tools_tab_index = -1  # kept for IDA compat

        self._context_bar = ContextBar()
        self._context_bar.set_model(self._config.provider.model)
        layout.addWidget(self._context_bar)
        if self._composer is not None:
            self._composer.set_model(self._config.provider.model)
        self._apply_responsive_layout()
        QTimer.singleShot(0, self._load_binary_summary)
        QTimer.singleShot(0, self._detect_native_mcp)

        if self._ui_hooks_factory is not None:
            try:
                self._ui_hooks = self._ui_hooks_factory(lambda: self)
                if self._ui_hooks is not None:
                    self._ui_hooks.hook()
            except Exception as e:
                log_debug(f"UI hook setup failed: {e}")
                self._ui_hooks = None

        self._try_restore_session()

    def _build_tab_widget(self) -> None:
        """Create the tab widget with custom tab bar."""
        self._tab_widget = QTabWidget()
        self._tab_bar = _AddButtonTabBar()
        self._tab_widget.setTabBar(self._tab_bar)
        self._tab_widget.setDocumentMode(True)
        self._tab_widget.setTabsClosable(True)
        self._tab_widget.tabCloseRequested.connect(self._on_close_tab)
        self._tab_widget.currentChanged.connect(self._on_tab_changed)
        self._tab_bar.set_add_tab_callback(self._on_new_tab)
        self._tab_bar.set_export_tab_callback(self._on_export_tab)
        self._tab_bar.set_fork_tab_callback(self._on_fork_tab)
        self._tab_widget.setStyleSheet(
            maybe_host_stylesheet(
                "QTabWidget::pane { border: none; }"
                "QTabBar { background: #1e1e1e; border: none; }"
                "QTabBar::tab { background: #252526; color: #cccccc; padding: 2px 8px; "
                "border: none; border-right: 1px solid #3c3c3c; "
                "font-size: 11px; max-width: 140px; }"
                "QTabBar::tab:selected { background: #1e1e1e; color: #ffffff; }"
                "QTabBar::tab:hover { background: #2d2d2d; }"
                "QTabBar::close-button { image: none; border: none; padding: 1px; }"
                "QTabBar::close-button:hover { background: #c42b1c; border-radius: 2px; }"
            )
        )
        self._tab_bar.setExpanding(False)
        self._tab_bar.setVisible(False)  # hidden until 2+ tabs
        self._tab_widget.tabBar().hide()

    def _build_main_splitter(self, layout: QVBoxLayout) -> None:
        """Create the horizontal splitter (chat | mutation log) and add to layout."""
        self._main_splitter = QSplitter(Qt.Orientation.Horizontal)
        self._main_splitter.setHandleWidth(1)
        self._main_splitter.setStyleSheet(maybe_host_stylesheet("QSplitter::handle { background: #3c3c3c; }"))
        self._chat_sidebar = ChatThreadList()
        self._chat_sidebar.set_callbacks(
            select=self._select_chat,
            new=self._on_new_tab,
            delete=self._delete_chat,
            fork=self._fork_chat,
            export=self._export_chat,
        )
        self._main_splitter.addWidget(self._chat_sidebar)
        # Re-apply with the now-established parent palette (IDA may have set it after __init__)
        self._chat_sidebar.refresh_theme()

        self._chat_sidebar.set_close_callback(self._close_drawer)

        chat_column = QWidget()
        self._chat_column = chat_column
        chat_column_layout = QVBoxLayout(chat_column)
        chat_column_layout.setContentsMargins(0, 0, 0, 0)
        chat_column_layout.setSpacing(0)
        # Page 0 = the tab widget (chats); page 1 = the "no chat open" placeholder
        # shown when auto-load is disabled and nothing has been opened yet.
        self._chat_area_stack = QStackedWidget()
        self._chat_area_stack.addWidget(self._tab_widget)
        self._chat_area_stack.addWidget(self._build_placeholder())
        chat_column_layout.addWidget(self._chat_area_stack, 1)
        chat_column_layout.addWidget(self._build_input_section())
        self._main_splitter.addWidget(chat_column)

        # Overlay scrim for the drawer; a child of the chat column so it never
        # covers the header or the composer's own column.
        self._scrim = _DrawerScrim(chat_column)
        self._scrim.set_dismiss_callback(self._close_drawer)
        self._scrim.setVisible(False)

        self._mutation_panel = MutationLogPanel()
        self._mutation_panel.undo_requested.connect(self._on_undo_requested)
        self._mutation_panel.setVisible(False)
        self._main_splitter.addWidget(self._mutation_panel)

        self._main_splitter.setStretchFactor(0, 0)
        self._main_splitter.setStretchFactor(1, 3)
        self._main_splitter.setStretchFactor(2, 1)

        layout.addWidget(self._main_splitter, 1)

    def _build_placeholder(self) -> QWidget:
        """Build the 'no chat open' screen shown when auto-load is disabled.

        It is the same welcome screen an empty chat shows; picking a starting
        point fills the composer and submitting it opens a fresh chat.
        """
        placeholder = WelcomeView()
        placeholder.set_activated_callback(self._on_suggestion_activated)
        self._welcome_views["__placeholder__"] = placeholder
        self._refresh_welcome(placeholder)
        return placeholder

    def _make_welcome(self, tab_id: str) -> WelcomeView:
        """Create the empty-chat welcome for a tab."""
        welcome = WelcomeView()
        welcome.set_activated_callback(self._on_suggestion_activated)
        self._refresh_welcome(welcome)
        self._welcome_views[tab_id] = welcome
        return welcome

    def _refresh_welcome(self, welcome: WelcomeView) -> None:
        """Push the current binary card and starting points into one welcome."""
        welcome.set_binary_summary(self._binary_summary)
        welcome.set_suggestions(build_suggestions(self._ctrl.skill_slugs, get_current_address()))

    def _refresh_all_welcomes(self) -> None:
        for welcome in list(self._welcome_views.values()):
            try:
                self._refresh_welcome(welcome)
            except RuntimeError as e:  # widget already destroyed with its tab
                log_debug(f"Welcome refresh skipped: {e}")

    def _on_suggestion_activated(self, prompt: str) -> None:
        """Fill the composer from a welcome suggestion, leaving send to the user."""
        if self._is_shutdown or self._composer is None:
            return
        self._composer.set_text(prompt)

    def _load_binary_summary(self) -> None:
        """Collect the welcome card's binary facts once the runtime is ready."""
        if self._binary_summary_loaded or self._is_shutdown:
            return
        if not getattr(self._ctrl, "runtime_ready", False):
            self._binary_summary_attempts += 1
            if self._binary_summary_attempts <= _BINARY_SUMMARY_MAX_ATTEMPTS:
                QTimer.singleShot(500, self._load_binary_summary)
            return
        self._binary_summary_loaded = True
        self._binary_queue = queue.Queue()
        threading.Thread(
            target=self._collect_binary_summary,
            daemon=True,
            name="rikugan-binary-summary",
        ).start()
        self._ensure_binary_poll_timer()

    def _collect_binary_summary(self) -> None:
        """Background worker: run the two info tools and post a summary.

        Both handlers are marshalled to the main thread by ``@idasync`` in IDA
        and are thread-safe in Binary Ninja, so the string probe (which can be
        slow on a large database) never blocks first paint.
        """
        raw_info = ""
        string_count: int | None = None
        try:
            registry = self._ctrl.get_tool_registry()
            info_defn = registry.get("get_binary_info")
            if info_defn is not None and info_defn.handler is not None:
                raw_info = info_defn.handler() or ""
            strings_defn = registry.get("list_strings")
            if strings_defn is not None and strings_defn.handler is not None:
                string_count = parse_total_count(strings_defn.handler(offset=0, limit=1) or "")
        except Exception as e:  # defensive: a missing tool must not kill the thread
            log_debug(f"Binary summary collection failed: {e}")
        if self._binary_queue is not None:
            self._binary_queue.put(build_binary_summary(raw_info, string_count))

    def _ensure_binary_poll_timer(self) -> None:
        if self._binary_timer is not None:
            return
        self._binary_timer = QTimer(self)
        self._binary_timer.setInterval(100)
        self._binary_timer.timeout.connect(self._poll_binary_summary)
        self._binary_timer.start()

    def _stop_binary_poll_timer(self) -> None:
        if self._binary_timer is None:
            return
        self._binary_timer.stop()
        try:
            self._binary_timer.timeout.disconnect(self._poll_binary_summary)
        except (RuntimeError, TypeError) as e:
            log_debug(f"binary summary timer disconnect failed: {e}")
        self._binary_timer.deleteLater()
        self._binary_timer = None

    def _poll_binary_summary(self) -> None:
        if self._is_shutdown or self._binary_queue is None:
            self._stop_binary_poll_timer()
            self._stop_native_mcp_timer()
            return
        try:
            summary = self._binary_queue.get_nowait()
        except queue.Empty:
            return
        self._stop_binary_poll_timer()
        self._binary_queue = None
        self._binary_summary = summary
        self._refresh_all_welcomes()

    # --- Binary Ninja's own MCP server ----------------------------------

    def _detect_native_mcp(self, user_initiated: bool = False) -> None:
        """Look for the host's MCP server, off the UI thread.

        Re-runnable: the server is a plugin the user can enable at any time, so
        a single probe at startup would miss one turned on afterwards.
        """
        if self._is_shutdown or self._ctrl.host_name != "Binary Ninja":
            return
        if self._native_mcp_probe is not None:
            return  # a probe is already in flight
        if not getattr(self._ctrl, "runtime_ready", False):
            if not user_initiated:
                QTimer.singleShot(500, self._detect_native_mcp)
                return

        from ..binja import native_mcp

        result_queue: queue.Queue = queue.Queue()
        self._native_mcp_probe = result_queue
        self._native_mcp_user_initiated = user_initiated
        url = self._config.binja_mcp_url or native_mcp.DEFAULT_URL

        threading.Thread(
            target=lambda: result_queue.put(native_mcp.probe(url)),
            daemon=True,
            name="rikugan-native-mcp-probe",
        ).start()

        if user_initiated and self._panel_header is not None:
            self._panel_header.set_native_mcp_busy(True)

        self._native_mcp_timer = QTimer(self)
        self._native_mcp_timer.setInterval(200)
        self._native_mcp_timer.timeout.connect(self._poll_native_mcp)
        self._native_mcp_timer.start()

    def _stop_native_mcp_timer(self) -> None:
        if self._native_mcp_timer is None:
            return
        self._native_mcp_timer.stop()
        try:
            self._native_mcp_timer.timeout.disconnect(self._poll_native_mcp)
        except (RuntimeError, TypeError) as e:
            log_debug(f"native MCP timer disconnect failed: {e}")
        self._native_mcp_timer.deleteLater()
        self._native_mcp_timer = None

    def _poll_native_mcp(self) -> None:
        if self._is_shutdown or self._native_mcp_probe is None:
            self._stop_native_mcp_timer()
            return
        try:
            result = self._native_mcp_probe.get_nowait()
        except queue.Empty:
            return
        self._stop_native_mcp_timer()
        self._native_mcp_probe = None
        asked = self._native_mcp_user_initiated
        self._native_mcp_user_initiated = False
        if self._panel_header is not None:
            # Whatever the answer, the control stops claiming to be mid-switch.
            self._panel_header.set_native_mcp_busy(False)

        if not result.available:
            self._native_mcp_available = False
            if asked:
                self._report_native_mcp_missing(result)
            return

        self._native_mcp_available = True
        if asked:
            # Clicking the toggle is the consent.
            self._remember_native_mcp_consent(True)
            self._start_native_mcp()
            return

        consent = self._config.binja_mcp_consent.get(self._native_mcp_key())
        if consent is None:
            consent = self._ask_native_mcp_consent(result)
            self._remember_native_mcp_consent(consent)
        if consent:
            self._start_native_mcp()

    def _report_native_mcp_missing(self, result) -> None:
        """Say why an explicit request found nothing, instead of doing nothing."""
        from ..binja import native_mcp

        QMessageBox.information(
            self,
            "Binary Ninja MCP server",
            native_mcp.missing_server_message(result.url),
        )

    def _native_mcp_key(self) -> str:
        """Consent is remembered per binary, not per session."""
        return self._ctrl._db_instance_id or self._ctrl._idb_path or "default"

    def _remember_native_mcp_consent(self, allowed: bool) -> None:
        self._config.binja_mcp_consent[self._native_mcp_key()] = allowed
        try:
            self._config.save()
        except Exception as e:
            log_error(f"Failed to save Binary Ninja MCP consent: {e}")

    def _ask_native_mcp_consent(self, result) -> bool:
        reply = QMessageBox.question(
            self,
            "Binary Ninja MCP server",
            f"Binary Ninja is exposing {result.tool_count} tools over MCP.\n\n"
            "Let Rikugan use them for this binary? They read the same analysis "
            "database you are looking at, so the agent will prefer them over its "
            "own equivalents.",
            qt_flags(QMessageBox.StandardButton.Yes, QMessageBox.StandardButton.No),
            QMessageBox.StandardButton.Yes,
        )
        return reply == QMessageBox.StandardButton.Yes

    def _start_native_mcp(self) -> None:
        """Connect to the host's MCP server and expose its tools to the agent."""
        if self._native_mcp_active or self._is_shutdown:
            return
        from ..binja import native_mcp

        url = self._config.binja_mcp_url or native_mcp.DEFAULT_URL
        registry = self._ctrl.get_tool_registry()
        self._ctrl._mcp_manager.start_server(
            native_mcp.server_config(url, max_tools=self._config.binja_mcp_max_tools),
            registry,
            on_complete=lambda name, count: self._on_native_mcp_ready(count),
        )
        self._native_mcp_active = True
        if self._panel_header is not None:
            self._panel_header.set_native_mcp_active(True)

    def _on_native_mcp_ready(self, count: int) -> None:
        """Hand the host's tools the jobs ours were doing.

        Declaring both sets sends two ways to do each job on every request,
        which is what the model provider was refusing.
        """
        from ..binja import native_mcp
        from ..mcp.bridge import describe_payload

        registry = self._ctrl.get_tool_registry()
        if count > 0 and self._config.binja_mcp_replaces_builtins:
            superseded = native_mcp.superseded_builtins(registry)
            registry.set_shadowed(superseded)
            log_info(f"Binary Ninja MCP: {count} tools available, {len(superseded)} Rikugan tools switched off")
        else:
            log_info(f"Binary Ninja MCP: {count} tools available")
        log_info(f"Binary Ninja MCP: {describe_payload(registry)}")

    def _stop_native_mcp(self) -> None:
        if not self._native_mcp_active:
            return
        from ..binja import native_mcp

        registry = self._ctrl.get_tool_registry()
        self._ctrl._mcp_manager.stop_server(
            native_mcp.SERVER_NAME,
            registry,
            native_mcp.tool_prefix(),
        )
        registry.clear_shadowed()
        self._native_mcp_active = False
        if self._panel_header is not None:
            self._panel_header.set_native_mcp_active(False)

    def _toggle_native_mcp(self) -> None:
        """Turn the host's MCP tools on or off, remembering the choice."""
        if self._native_mcp_active:
            self._stop_native_mcp()
            self._remember_native_mcp_consent(False)
            return
        if self._native_mcp_available:
            self._start_native_mcp()
            self._remember_native_mcp_consent(True)
            return
        # Nothing found yet — the server may have been enabled since startup.
        self._detect_native_mcp(user_initiated=True)

    def _chat_group(self, tab_id: str) -> str:
        """Group label for a chat: the binary it belongs to."""
        session = self._ctrl.get_session(tab_id)
        path = getattr(session, "idb_path", "") if session is not None else ""
        path = path or self._ctrl._idb_path
        if not path or not isinstance(path, str):
            return ""
        return os.path.basename(path)

    def _sync_header_title(self, tab_id: str | None = None) -> None:
        """Show the active chat's label on the header switcher."""
        if self._panel_header is None:
            return
        tab_id = tab_id or self._ctrl.active_tab_id
        self._panel_header.set_chat_title(self._ctrl.tab_label(tab_id))

    def _show_placeholder(self) -> None:
        """Show the 'no chat open' placeholder in the chat area."""
        if self._chat_area_stack is not None:
            self._chat_area_stack.setCurrentIndex(1)

    def _show_chat_area(self) -> None:
        """Show the tabbed chat area (hides the placeholder)."""
        if self._chat_area_stack is not None:
            self._chat_area_stack.setCurrentIndex(0)

    def _build_input_section(self) -> QWidget:
        """Build the composer: input frame with an inline control row."""
        self._composer = Composer()
        self._composer.set_submit_callback(self._on_submit)
        self._composer.set_cancel_callback(self._on_cancel)
        self._composer.set_send_callback(self._on_send_clicked)
        self._composer.set_skill_slugs(self._ctrl.skill_slugs)
        # Alias kept so the rest of the panel can keep talking to the editor.
        self._input_area = self._composer.input_area
        self._mutations_btn = None
        self._tools_btn = None
        self._ensure_skills_refresh_timer()
        return self._composer

    # --- responsive chat list -------------------------------------------

    def _apply_responsive_layout(self) -> None:
        """Move the chat list between splitter column and overlay drawer.

        Widening the panel must not reveal the list on its own: whether it is
        open is the user's choice (``_sidebar_open``), and only the width
        decides *how* it is shown.
        """
        if self._chat_sidebar is None or self._chat_column is None:
            return
        drawer = should_use_drawer(self.width())
        if drawer == self._drawer_mode:
            if drawer and self._sidebar_open:
                self._layout_drawer()
            return
        self._drawer_mode = drawer
        try:
            if drawer:
                self._chat_sidebar.setParent(self._chat_column)
            else:
                self._main_splitter.insertWidget(0, self._chat_sidebar)
                self._main_splitter.setStretchFactor(0, 0)
            self._chat_sidebar.set_drawer_mode(drawer)
            self._apply_sidebar_visibility()
        except RuntimeError as e:
            log_debug(f"Responsive layout switch failed: {e}")

    def _apply_sidebar_visibility(self) -> None:
        """Show or hide the chat list to match the user's choice."""
        if self._chat_sidebar is None:
            return
        want = self._sidebar_open
        if self._drawer_mode and want:
            self._layout_drawer()
        self._chat_sidebar.setVisible(want)
        if self._drawer_mode and want:
            self._chat_sidebar.raise_()
        if self._scrim is not None:
            # The scrim only belongs under a floating drawer, never under the
            # docked column.
            self._scrim.setVisible(want and self._drawer_mode)
            if want and self._drawer_mode:
                self._scrim.raise_()
                self._chat_sidebar.raise_()
        if self._panel_header is not None:
            self._panel_header.set_sidebar_open(want)

    def _layout_drawer(self) -> None:
        """Size the floating drawer and its scrim to the chat column."""
        if self._chat_sidebar is None or self._chat_column is None:
            return
        height = self._chat_column.height()
        width = min(_DRAWER_MAX_WIDTH, max(200, int(self._chat_column.width() * _DRAWER_WIDTH_RATIO)))
        self._chat_sidebar.setGeometry(0, 0, width, height)
        if self._scrim is not None:
            self._scrim.setGeometry(0, 0, self._chat_column.width(), height)

    def _toggle_drawer(self) -> None:
        """Open or close the chat list, in whichever form it currently takes."""
        if self._chat_sidebar is None:
            return
        self._sidebar_open = not self._sidebar_open
        self._apply_sidebar_visibility()

    def _open_drawer(self) -> None:
        if self._chat_sidebar is None:
            return
        self._sidebar_open = True
        self._apply_sidebar_visibility()

    def _close_drawer(self) -> None:
        if self._chat_sidebar is None:
            return
        self._sidebar_open = False
        self._apply_sidebar_visibility()

    def resizeEvent(self, event) -> None:
        super().resizeEvent(event)
        self._apply_responsive_layout()

    def showEvent(self, event) -> None:
        # A panel docked straight at sidebar width may never get a resize event,
        # so settle the layout the first time it is shown as well.
        super().showEvent(event)
        self._apply_responsive_layout()

    # --- Tab management ---

    def _update_tab_bar_visibility(self) -> None:
        """Show the tab bar only when there are 2+ tabs."""
        self._tab_bar.setVisible(False)

    def _create_tab(self, tab_id: str, label: str, add_to_sidebar: bool = True, select: bool = True) -> ChatView:
        """Create a new ChatView and add it as a tab.

        ``add_to_sidebar`` is False when materializing a ChatView for a chat that
        already has a sidebar entry (lazy open in don't-auto-load mode).
        """
        chat_view = ChatView()
        chat_view.setProperty("tab_id", tab_id)  # O(1) lookup in _tab_id_at_index
        chat_view.set_tool_approval_callback(self._on_tool_approval)
        chat_view.set_user_answer_callback(self._on_user_answer_submitted)
        chat_view.set_welcome_widget(self._make_welcome(tab_id))
        self._chat_views[tab_id] = chat_view
        index = self._tab_widget.addTab(chat_view, label)
        # Re-apply with the now-established parent palette
        chat_view.setStyleSheet(build_chat_view_stylesheet(chat_view))
        if select:
            self._tab_widget.setCurrentIndex(index)
            self._show_chat_area()
        if self._chat_sidebar is not None:
            if add_to_sidebar:
                self._chat_sidebar.add_chat(tab_id, label, self._chat_detail(tab_id), self._chat_group(tab_id))
            if select:
                self._chat_sidebar.select_chat(tab_id)
        if select:
            self._sync_header_title(tab_id)
        self._update_tab_bar_visibility()
        return chat_view

    def _add_unloaded_chat(self, tab_id: str, session) -> None:
        """List a restored chat in the sidebar without building its ChatView.

        Used in don't-auto-load mode: the session is registered in the
        controller and its messages are stashed for replay; the (expensive)
        ChatView is created only when the user opens the chat.
        """
        self._pending_restore_messages[tab_id] = session.messages
        if self._chat_sidebar is not None:
            self._chat_sidebar.add_chat(
                tab_id,
                self._ctrl.tab_label(tab_id),
                self._chat_detail(tab_id),
                self._chat_group(tab_id),
            )

    def _on_new_tab(self) -> None:
        """Create a fresh independent chat tab."""
        if self._is_shutdown:
            return
        tab_id = self._ctrl.create_tab()
        self._ctrl.switch_tab(tab_id)
        self._create_tab(tab_id, "Untitled")
        self._update_token_display(0)
        self._set_running(False, tab_id=tab_id)

    def _on_fork_current(self) -> None:
        """Fork the active chat (header overflow menu)."""
        self._fork_chat(self._ctrl.active_tab_id)

    def _on_delete_current(self) -> None:
        """Delete the active chat (header overflow menu)."""
        self._delete_chat(self._ctrl.active_tab_id)

    def _on_fork_tab(self, index: int) -> None:
        """Fork (duplicate) a session into a new tab."""
        source_tab_id = self._tab_id_at_index(index)
        if source_tab_id is None:
            return
        self._fork_chat(source_tab_id)

    def _fork_chat(self, source_tab_id: str) -> None:
        """Fork (duplicate) a session into a new chat."""
        new_tab_id = self._ctrl.fork_session(source_tab_id)
        if new_tab_id is None:
            return
        label = self._ctrl.tab_label(new_tab_id)
        chat_view = self._create_tab(new_tab_id, f"{label} (fork)")
        # Restore messages into the forked chat view
        source_session = self._ctrl.get_session(new_tab_id)
        if source_session and source_session.messages:
            chat_view.restore_from_messages(source_session.messages)
        self._ctrl.switch_tab(new_tab_id)
        log_info(f"Forked tab {source_tab_id} → {new_tab_id}")

    def _select_chat(self, tab_id: str) -> None:
        if tab_id not in self._chat_views:
            # Lazy open (don't-auto-load mode): the chat is listed but its
            # ChatView hasn't been built yet. Materialize it now; messages are
            # replayed by _restore_messages_if_needed below.
            if self._ctrl.get_session(tab_id) is None:
                return
            self._create_tab(tab_id, self._ctrl.tab_label(tab_id), add_to_sidebar=False, select=False)
        cv = self._chat_views[tab_id]
        for i in range(self._tab_widget.count()):
            if self._tab_widget.widget(i) is cv:
                self._tab_widget.setCurrentIndex(i)
                break
        self._show_chat_area()
        self._ctrl.switch_tab(tab_id)
        self._restore_messages_if_needed(tab_id)
        self._update_token_display()
        self._set_running(self._ctrl.is_tab_running(tab_id), tab_id=tab_id)
        if self._chat_sidebar is not None:
            self._chat_sidebar.select_chat(tab_id)
        self._sync_header_title(tab_id)
        self._close_drawer()

    def _delete_chat(self, tab_id: str) -> None:
        if self._is_shutdown:
            return
        session = self._ctrl.get_session(tab_id)
        label = self._ctrl.tab_label(tab_id)
        if session and session.messages:
            reply = QMessageBox.question(
                self,
                "Delete Chat",
                f"Delete '{label}' and remove it from saved history?",
                qt_flags(QMessageBox.StandardButton.Yes, QMessageBox.StandardButton.No),
                QMessageBox.StandardButton.No,
            )
            if reply != QMessageBox.StandardButton.Yes:
                return
        self._welcome_views.pop(tab_id, None)
        cv = self._chat_views.pop(tab_id, None)
        if cv is not None:
            for i in range(self._tab_widget.count()):
                if self._tab_widget.widget(i) is cv:
                    self._tab_widget.removeTab(i)
                    break
            cv.shutdown()
            cv.deleteLater()
        if self._chat_sidebar is not None:
            self._chat_sidebar.remove_chat(tab_id)
        self._sidebar_rows.pop(tab_id, None)
        self._pending_restore_messages.pop(tab_id, None)
        self._ctrl.delete_tab(tab_id)
        active = self._ctrl.active_tab_id
        if active in self._chat_views:
            self._select_chat(active)
        elif self._config.dont_auto_load_chats:
            # Don't force a chat open over the placeholder; the rest stay listed.
            self._show_placeholder()
        else:
            self._create_tab(active, "Untitled")
            self._select_chat(active)
        self._refresh_chat_sidebar()

    def _export_chat(self, tab_id: str) -> None:
        for i in range(self._tab_widget.count()):
            if self._tab_id_at_index(i) == tab_id:
                self._on_export_tab(i)
                return

    def _on_close_tab(self, index: int) -> None:
        """Close a tab. Prevents closing the last tab."""
        if self._tab_widget.count() <= 1:
            return  # Don't close the last tab
        tab_id = self._tab_id_at_index(index)
        if tab_id is None:
            return
        self._ctrl.close_tab(tab_id)
        self._welcome_views.pop(tab_id, None)
        chat_view = self._chat_views.pop(tab_id, None)
        self._tab_widget.removeTab(index)
        if self._chat_sidebar is not None:
            self._chat_sidebar.remove_chat(tab_id)
        self._sidebar_rows.pop(tab_id, None)
        if chat_view:
            chat_view.shutdown()
            chat_view.deleteLater()
        self._update_tab_bar_visibility()
        self._refresh_chat_sidebar()

    def _on_export_tab(self, index: int) -> None:
        """Export a tab's chat to a Markdown file."""
        tab_id = self._tab_id_at_index(index)
        if tab_id is None:
            return
        session = self._ctrl.get_session(tab_id)
        if session is None or not session.messages:
            return

        # Show export options dialog if there are subagent logs
        include_subagents = False
        if session.subagent_logs:
            dlg = QDialog(self)
            dlg.setWindowTitle("Export Options")
            dlg.setStyleSheet(
                maybe_host_stylesheet(
                    "QDialog { background: #1e1e1e; }"
                    "QLabel { color: #d4d4d4; font-size: 12px; }"
                    "QCheckBox { color: #d4d4d4; font-size: 12px; }"
                )
            )
            layout = QVBoxLayout(dlg)
            cb = QCheckBox(f"Include subagent logs ({len(session.subagent_logs)} subagent runs)")
            cb.setChecked(True)
            layout.addWidget(cb)
            buttons = QDialogButtonBox(
                qt_flags(
                    QDialogButtonBox.StandardButton.Ok,
                    QDialogButtonBox.StandardButton.Cancel,
                )
            )
            buttons.accepted.connect(dlg.accept)
            buttons.rejected.connect(dlg.reject)
            layout.addWidget(buttons)
            if not qt_run(dlg):
                return
            include_subagents = cb.isChecked()

        label = self._ctrl.tab_label(tab_id).replace("/", "-").replace("\\", "-")
        default_name = f"rikugan-{label}-{time.strftime('%Y%m%d-%H%M%S')}.md"
        path, _ = QFileDialog.getSaveFileName(
            self,
            "Export Chat",
            default_name,
            "Markdown (*.md);;Text (*.txt);;All Files (*)",
        )
        if not path:
            return
        try:
            self._export_session_to_file(session, path, include_subagents=include_subagents)
            log_info(f"Exported chat to {path}")
        except Exception as e:
            log_error(f"Failed to export chat: {e}")

    @staticmethod
    def _export_session_to_file(
        session,
        path: str,
        include_subagents: bool = False,
    ) -> None:
        """Write session messages to a Markdown file."""
        lines = ["# Rikugan Chat Export\n"]
        lines.append(f"- **Model**: {session.model_name or 'unknown'}")
        lines.append(f"- **Exported**: {time.strftime('%Y-%m-%d %H:%M:%S')}")
        if session.idb_path:
            lines.append(f"- **File**: `{os.path.basename(session.idb_path)}`")
        lines.append("")
        lines.append("---\n")

        subagent_logs = session.subagent_logs if include_subagents else {}

        for msg in session.messages:
            if msg.role == Role.USER:
                lines.append(f"## You\n\n{msg.content}\n")
            elif msg.role == Role.ASSISTANT:
                if msg.content:
                    lines.append(f"## Rikugan\n\n{msg.content}\n")
                for tc in msg.tool_calls:
                    lines.append(f"**Tool call**: `{tc.name}`\n")
                    lines.append(_export_format_tool_args(tc))
                    lines.append("")
            elif msg.role == Role.TOOL:
                for tr in msg.tool_results:
                    status = "Error" if tr.is_error else "Result"
                    lines.append(f"**{status}** (`{tr.name}`):\n")
                    lines.append(_export_format_tool_result(tr))
                    lines.append("")
                    # Insert subagent log after the spawn_subagent result
                    if tr.name == "spawn_subagent" and tr.tool_call_id in subagent_logs:
                        lines.append(
                            _export_format_subagent_log(
                                subagent_logs[tr.tool_call_id],
                            )
                        )

        # Append exploration subagent logs that aren't tied to a tool_call_id
        if include_subagents:
            for key, msgs in subagent_logs.items():
                if key.startswith("exploration_"):
                    lines.append("\n---\n\n### Exploration Subagent Log\n")
                    lines.append(_export_format_subagent_log(msgs))

        with open(path, "w", encoding="utf-8") as f:
            f.write("\n".join(lines))

    def _on_export_current(self) -> None:
        """Export the currently active tab's chat."""
        index = self._tab_widget.currentIndex()
        if index >= 0:
            self._on_export_tab(index)

    def _on_tab_changed(self, index: int) -> None:
        """Handle tab switch."""
        if index < 0 or self._is_shutdown:
            return
        tab_id = self._tab_id_at_index(index)
        if tab_id is None:
            return
        self._ctrl.switch_tab(tab_id)
        self._restore_messages_if_needed(tab_id)
        self._update_token_display()
        if self._chat_sidebar is not None:
            self._chat_sidebar.select_chat(tab_id)
        self._sync_header_title(tab_id)
        self._set_running(self._ctrl.is_tab_running(tab_id), tab_id=tab_id)

    def _tab_id_at_index(self, index: int) -> str | None:
        """Find the tab_id for a given tab index via the stored property (O(1))."""
        widget = self._tab_widget.widget(index)
        if widget is None:
            return None
        tid = widget.property("tab_id")
        if tid and tid in self._chat_views:
            return tid
        # Fallback for tabs created before property was set
        for tid, cv in self._chat_views.items():
            if cv is widget:
                return tid
        return None

    def _active_chat_view(self) -> ChatView | None:
        """Return the ChatView for the currently active tab."""
        return self._chat_views.get(self._ctrl.active_tab_id)

    def _restore_messages_if_needed(self, tab_id: str) -> None:
        """Replay deferred restored messages for a tab the first time it is shown."""
        messages = self._pending_restore_messages.pop(tab_id, None)
        if not messages:
            return
        chat_view = self._chat_views.get(tab_id)
        if chat_view is not None:
            chat_view.restore_from_messages(messages)

    def _update_token_display(self, token_count: int | None = None) -> None:
        """Update the context bar token display with context window percentage."""
        if self._context_bar is None:
            return
        if token_count is None:
            session = self._ctrl.session
            # Show current context size (last prompt), not cumulative total
            token_count = (
                session.last_prompt_tokens
                if session.last_prompt_tokens is not None
                else session.total_usage.total_tokens
            )
        ctx_window = self._ctrl.get_context_window()
        self._context_bar.set_tokens(token_count, ctx_window)

    def _update_tab_label(self, tab_id: str) -> None:
        """Update tab label from the first user message."""
        label = self._ctrl.tab_label(tab_id)
        cv = self._chat_views.get(tab_id)
        if cv is None:
            return
        for i in range(self._tab_widget.count()):
            if self._tab_widget.widget(i) is cv:
                self._tab_widget.setTabText(i, label)
                break
        if self._chat_sidebar is not None:
            self._chat_sidebar.update_chat(tab_id, label, self._chat_detail(tab_id))
        if tab_id == self._ctrl.active_tab_id:
            self._sync_header_title(tab_id)

    def _chat_detail(self, tab_id: str) -> str:
        session = self._ctrl.get_session(tab_id)
        if session is None:
            return ""
        threads = max(1, session.current_turn)
        changes = len(getattr(session, "mutation_log", []) or [])
        detail = f"{threads} thread" + ("" if threads == 1 else "s")
        if changes:
            detail += f" · {changes} changes"
        return detail

    def _refresh_chat_sidebar(self) -> None:
        """Push label/detail/status into the chat list, skipping unchanged rows.

        This runs on every 50ms poll tick. Rewriting each row unconditionally
        cost two full rebuilds per tab per tick — four labels, a sizeHint
        recompute and a whole-list filter pass each — 40 list relayouts a second
        of work that almost always changed nothing.
        """
        if self._chat_sidebar is None:
            return
        for tab_id in self._chat_views:
            pending = self._ctrl.tab_pending_count(tab_id)
            if tab_id in self._awaiting_approval_tabs:
                status, pending = "approval", 0
            elif self._ctrl.is_tab_running(tab_id):
                status = "queued" if pending else "running"
            else:
                status, pending = self._tab_status.get(tab_id, "idle"), 0
            row = (self._ctrl.tab_label(tab_id), self._chat_detail(tab_id), status, pending)
            if self._sidebar_rows.get(tab_id) == row:
                continue
            self._sidebar_rows[tab_id] = row
            self._chat_sidebar.update_chat(tab_id, row[0], row[1])
            self._chat_sidebar.set_status(tab_id, status, pending)

    # --- Public API ---

    def prefill_input(self, text: str, auto_submit: bool = False) -> None:
        if self._is_shutdown:
            return
        self._input_area.setPlainText(text)
        if auto_submit:
            self._input_area.clear()
            self._on_submit(text)
        else:
            self._input_area.setFocus()

    def shutdown(self) -> None:
        if self._is_shutdown:
            return
        self._is_shutdown = True
        try:
            tools_form = getattr(self, "_tools_form", None)
            tools_panel = getattr(self, "_tools_panel", None)
            self._stop_poll_timer()
            self._stop_skills_refresh_timer()
            self._stop_restore_poll_timer()
            self._stop_binary_poll_timer()
            _SharedSpinnerTimer.shutdown()
            if self._context_bar:
                self._context_bar.stop()
            for cv in self._chat_views.values():
                cv.shutdown()
            if self._ui_hooks:
                self._ui_hooks.unhook()
                self._ui_hooks = None
            if tools_form is not None:
                tools_form.hide()
                # In IDA mode, hide() orphans the tools widget via
                # OnClose -> setParent(None).  Schedule it for deletion
                # while Python is still alive to prevent crashes during
                # QApplication::~QApplication() exit cleanup.
                if tools_panel is not None:
                    tools_panel.deleteLater()
            elif tools_panel is not None:
                tools_panel.close()
            self._tools_panel = None
            self._ctrl.shutdown()
        except Exception as e:
            log_error(f"Panel teardown error: {e}")

    def on_database_changed(self, new_path: str) -> None:
        """Called when the user opens a different file."""
        if self._is_shutdown:
            return
        normalized = os.path.normcase(os.path.realpath(os.path.abspath(new_path))) if new_path else ""
        if normalized == self._ctrl._idb_path:
            return
        self._ctrl.reset_for_new_file(normalized)
        # Remove all existing tabs
        for cv in self._chat_views.values():
            cv.shutdown()
        while self._tab_widget.count():
            w = self._tab_widget.widget(0)
            self._tab_widget.removeTab(0)
            if w:
                w.deleteLater()
        self._chat_views.clear()
        self._pending_restore_messages.clear()
        placeholder = self._welcome_views.get("__placeholder__")
        self._welcome_views = {"__placeholder__": placeholder} if placeholder is not None else {}
        self._binary_summary = BinarySummary()
        self._binary_summary_loaded = False
        self._binary_summary_attempts = 0
        self._refresh_all_welcomes()
        QTimer.singleShot(0, self._load_binary_summary)
        self._sidebar_rows.clear()
        if self._chat_sidebar is not None:
            self._chat_sidebar.clear()
        # Show the default tab (or placeholder) and try to restore saved sessions
        if self._config.dont_auto_load_chats:
            self._show_placeholder()
        else:
            self._create_tab(self._ctrl.active_tab_id, "Untitled")
        self._try_restore_session()

    def _on_submit(self, text: str) -> None:
        if not text or self._is_shutdown:
            return
        chat_view = self._active_chat_view()
        if chat_view is None:
            # Placeholder mode (don't-auto-load): start a fresh chat to host it.
            self._on_new_tab()
            chat_view = self._active_chat_view()
            if chat_view is None:
                return
        tab_id = self._ctrl.active_tab_id
        # Block free-text when awaiting button-only approval (plan/save).
        if tab_id in self._awaiting_approval_tabs:
            log_debug(f"Ignoring text input while awaiting button approval: {text!r}")
            return
        if tab_id in self._pending_answer_tabs:
            self._pending_answer_tabs.discard(tab_id)
            chat_view.add_user_message(text)
            self._set_running(True, tab_id=tab_id)
            runner = self._ctrl.get_runner_for_tab(tab_id)
            if runner:
                runner.agent_loop.submit_user_answer(text)
            return
        # Queue while the agent is actively running.
        if self._ctrl.is_tab_running(tab_id):
            self._ctrl.queue_message(text, tab_id=tab_id)
            chat_view.add_queued_message(text)
            self._refresh_chat_sidebar()
            return
        self._start_agent(text, tab_id=tab_id)

    def _on_send_clicked(self) -> None:
        text = self._input_area.toPlainText().strip()
        if text:
            self._input_area.clear()
            self._on_submit(text)

    def _on_cancel(self) -> None:
        if self._is_shutdown:
            return
        self._pending_answer = False
        self._awaiting_button_approval = False
        tab_id = self._ctrl.active_tab_id
        self._pending_answer_tabs.discard(tab_id)
        self._awaiting_approval_tabs.discard(tab_id)
        self._tab_status[tab_id] = "cancelled"
        self._ctrl.cancel(tab_id)
        # Remove [queued] widgets from the active chat view
        chat_view = self._active_chat_view()
        if chat_view is not None:
            chat_view.remove_queued_messages()
        self._set_running(False, tab_id=tab_id)
        self._refresh_chat_sidebar()

    def _on_settings(self) -> None:
        try:
            from .settings_dialog import SettingsDialog

            dlg = SettingsDialog(
                self._config,
                registry=self._ctrl.provider_registry,
                tool_registry=self._ctrl.tool_registry,
            )
            result = qt_run(dlg)
            if result:
                self._config.save(password=dlg.encryption_password)
                self._ctrl.update_settings()
                self._ctrl.reload_mcp()
                if self._context_bar is not None:
                    self._context_bar.set_model(self._config.provider.model)
                if self._composer is not None:
                    self._composer.set_model(self._config.provider.model)
                log_info(f"Settings updated: {self._config.provider.name}/{self._config.provider.model}")
            dlg.setParent(None)
        except Exception as e:
            log_error(f"Settings dialog error: {e}")

    def _show_new_chat_dialog(self, context_pct: int) -> str:
        """Show a confirmation dialog with context usage. Returns 'yes', 'clear', or 'no'."""
        dlg = QMessageBox(self)
        dlg.setWindowTitle("New Chat")
        dlg.setText("Start a new chat? Current conversation will be saved.")
        dlg.setInformativeText(f"Context usage: {context_pct}%")
        dlg.setStyleSheet(
            maybe_host_stylesheet(
                "QMessageBox { background: #1e1e1e; color: #d4d4d4; }"
                "QLabel { color: #d4d4d4; font-size: 12px; }"
                "QPushButton { background: #2d2d2d; color: #d4d4d4; border: 1px solid #3c3c3c; "
                "border-radius: 4px; padding: 6px 16px; font-size: 11px; min-width: 80px; }"
                "QPushButton:hover { background: #3c3c3c; }"
            )
        )
        yes_btn = dlg.addButton("Yes", QMessageBox.ButtonRole.AcceptRole)
        clear_btn = dlg.addButton(
            f"Yes, clear context ({context_pct}% used)",
            QMessageBox.ButtonRole.AcceptRole,
        )
        no_btn = dlg.addButton("No", QMessageBox.ButtonRole.RejectRole)
        dlg.setDefaultButton(no_btn)
        qt_run(dlg)
        clicked = dlg.clickedButton()
        if clicked is clear_btn:
            return "clear"
        if clicked is yes_btn:
            return "yes"
        return "no"

    def _start_agent(self, user_message: str, tab_id: str | None = None, display_user: bool = True) -> None:
        tab_id = tab_id or self._ctrl.active_tab_id
        chat_view = self._chat_views.get(tab_id)
        if chat_view is None:
            return
        if display_user:
            chat_view.add_user_message(user_message)
        self._tab_status[tab_id] = "running"
        self._set_running(True, tab_id=tab_id)

        # Update tab label after first user message
        self._update_tab_label(tab_id)

        error = self._ctrl.start_agent(user_message, tab_id=tab_id)
        if error:
            chat_view.add_error_message(error)
            self._tab_status[tab_id] = "error"
            self._set_running(False, tab_id=tab_id)
            return

        self._ensure_poll_timer()
        assert self._poll_timer is not None
        self._poll_timer.start(50)

    def _ensure_poll_timer(self) -> None:
        if self._poll_timer is not None:
            return
        self._poll_timer = QTimer(self)
        self._poll_timer.timeout.connect(self._poll_events)

    def _stop_poll_timer(self) -> None:
        if self._poll_timer is not None:
            self._poll_timer.stop()
            try:
                self._poll_timer.timeout.disconnect(self._poll_events)
            except (RuntimeError, TypeError) as e:
                log_debug(f"panel_core timer disconnect failed: {e}")
            self._poll_timer.deleteLater()
            self._poll_timer = None

    def _poll_events(self) -> None:
        if self._polling or self._is_shutdown:
            return
        self._polling = True
        try:
            containers = []
            # Defer layout/paint passes until the whole batch is processed.
            # When 3 tools complete between ticks, each TOOL_RESULT makes a hidden
            # widget visible which triggers an O(n-widgets) layout cascade on the
            # chat container.  Batching those into one final pass cuts this from
            # O(k·n) to O(n) per tick.
            for tab_id, chat_view in self._chat_views.items():
                if self._ctrl.get_runner_for_tab(tab_id) is None:
                    continue
                container = chat_view._container
                containers.append(container)
                container.setUpdatesEnabled(False)
            try:
                for tab_id, event in self._ctrl.poll_events():
                    self._on_event(tab_id, event)
            finally:
                for container in containers:
                    container.setUpdatesEnabled(True)
            for tab_id in list(self._chat_views):
                runner = self._ctrl.get_runner_for_tab(tab_id)
                if runner is None or runner.agent_loop.is_running:
                    continue
                # The loop stops "running" before its thread has flushed the
                # last events, so only retire the runner once it is drained —
                # otherwise the tail of the final answer dies with the queue.
                self._drain_runner(tab_id, runner)
                if runner.is_drained():
                    self._on_agent_finished(tab_id)
            if not self._ctrl.has_runners:
                self._stop_poll_timer()
            self._refresh_chat_sidebar()
        finally:
            self._polling = False

    def _drain_runner(self, tab_id: str, runner) -> None:
        """Dispatch whatever a finished runner still has queued."""
        for _ in range(_MAX_DRAIN_EVENTS):
            event = runner.get_event(timeout=0)
            if event is None:
                return
            self._on_event(tab_id, event)

    def _on_event(self, tab_id: str, event: TurnEvent) -> None:
        if self._is_shutdown:
            return
        chat_view = self._chat_views.get(tab_id)
        if chat_view is None:
            return
        chat_view.handle_event(event)
        if event.usage and tab_id == self._ctrl.active_tab_id:
            # Use prompt_tokens from the event directly — session hasn't
            # been updated yet during streaming, so session.last_prompt_tokens
            # would be stale.  prompt_tokens reflects current context size.
            token_count = event.usage.context_tokens if event.usage.context_tokens > 0 else event.usage.total_tokens
            if token_count > 0:
                self._update_token_display(token_count)
        if event.type in (
            TurnEventType.USER_QUESTION,
            TurnEventType.SAVE_APPROVAL_REQUEST,
            TurnEventType.PLAN_GENERATED,
        ):
            self._pending_answer_tabs.add(tab_id)
            # Plan approvals, save approvals, and any question with
            # predefined options MUST be answered via buttons only.
            # Disable text input so free-text ("continue", "redo", etc.)
            # cannot bypass the approval gate.
            has_options = bool(event.metadata.get("options")) if event.metadata else False
            allow_text = bool(event.metadata.get("allow_text")) if event.metadata else False
            needs_button = event.type in (
                TurnEventType.PLAN_GENERATED,
                TurnEventType.SAVE_APPROVAL_REQUEST,
            ) or (has_options and not allow_text)
            if needs_button:
                self._awaiting_approval_tabs.add(tab_id)
            self._set_running(False, tab_id=tab_id)
        elif event.type == TurnEventType.ERROR:
            self._tab_status[tab_id] = "error"
        elif event.type == TurnEventType.CANCELLED:
            self._tab_status[tab_id] = "cancelled"
        elif event.type == TurnEventType.NOTICE:
            pass  # informational — the turn is still healthy
        else:
            self._tab_status[tab_id] = "running"
        if event.type == TurnEventType.MUTATION_RECORDED:
            self._on_mutation_recorded(event)

    def _on_tool_approval(self, tool_call_id: str, decision: str) -> None:
        """Forward tool approval to the agent loop."""
        runner = self._ctrl.get_runner_for_tab(self._ctrl.active_tab_id)
        if runner:
            runner.agent_loop.submit_tool_approval(decision)

    def _on_user_answer_submitted(self, answer: str) -> None:
        """Handle a button click from UserQuestionWidget (plan/save/ask_user)."""
        tab_id = self._ctrl.active_tab_id
        if tab_id not in self._pending_answer_tabs:
            return
        self._pending_answer_tabs.discard(tab_id)
        self._awaiting_approval_tabs.discard(tab_id)
        chat_view = self._active_chat_view()
        if chat_view is not None:
            chat_view.add_user_message(answer)
        self._set_running(True, tab_id=tab_id)
        runner = self._ctrl.get_runner_for_tab(tab_id)
        if runner:
            runner.agent_loop.submit_user_answer(answer)

    def _on_agent_finished(self, tab_id: str | None = None) -> None:
        if self._is_shutdown:
            return
        tab_id = tab_id or self._ctrl.active_tab_id

        # Clear approval state — if the agent crashed mid-approval the
        # buttons are stale and free-text input must be restored.
        self._pending_answer = False
        self._awaiting_button_approval = False

        next_message = self._ctrl.on_agent_finished(tab_id)
        # Remove any [queued] widgets since the queue was cleared.
        chat_view = self._chat_views.get(tab_id)
        if chat_view is not None:
            if next_message:
                chat_view.pop_first_queued_message()
            else:
                chat_view.remove_queued_messages()
        if next_message:
            self._start_agent(next_message, tab_id=tab_id)
            return
        if self._tab_status.get(tab_id) == "running":
            self._tab_status[tab_id] = "idle"
        self._set_running(False, tab_id=tab_id)

    def _try_restore_session(self) -> None:
        """Kick off session restore without blocking first paint.

        The heavy work — reading and parsing every saved chat from disk — runs
        on a background thread.  When it finishes, a poll timer builds the tabs
        back on the UI thread (Qt widgets must not be created off-thread).
        """
        if not self._config.restore_sessions_on_start:
            return
        # Cancel any in-flight restore (e.g. a rapid database switch).
        self._stop_restore_poll_timer()
        result_queue: queue.Queue = queue.Queue()
        self._restore_queue = result_queue

        def _load() -> None:
            try:
                sessions = self._ctrl.load_restorable_sessions()
            except Exception as e:  # defensive: never let the thread die silently
                log_error(f"Session restore load failed: {e}")
                sessions = []
            result_queue.put(sessions)

        threading.Thread(target=_load, daemon=True, name="rikugan-session-restore").start()
        self._ensure_restore_poll_timer()

    def _ensure_restore_poll_timer(self) -> None:
        if self._restore_timer is not None:
            return
        self._restore_timer = QTimer(self)
        self._restore_timer.setInterval(40)
        self._restore_timer.timeout.connect(self._poll_restore)
        self._restore_timer.start()

    def _stop_restore_poll_timer(self) -> None:
        if self._restore_timer is None:
            return
        self._restore_timer.stop()
        try:
            self._restore_timer.timeout.disconnect(self._poll_restore)
        except (RuntimeError, TypeError) as e:
            log_debug(f"restore timer disconnect failed: {e}")
        self._restore_timer.deleteLater()
        self._restore_timer = None

    def _poll_restore(self) -> None:
        if self._is_shutdown:
            self._stop_restore_poll_timer()
            return
        if self._restore_queue is None:
            self._stop_restore_poll_timer()
            return
        try:
            sessions = self._restore_queue.get_nowait()
        except queue.Empty:
            return
        self._stop_restore_poll_timer()
        self._restore_queue = None
        self._apply_restored_sessions(sessions)

    def _apply_restored_sessions(self, sessions: list) -> None:
        """Register pre-loaded sessions and build their tabs (UI thread)."""
        if self._is_shutdown:
            return
        restored = self._ctrl.register_restored_sessions(sessions)
        if restored and self._config.dont_auto_load_chats:
            # List the chats in the sidebar but don't open any — stay on the
            # placeholder. ChatViews are built lazily when a chat is selected.
            for tab_id, session in restored:
                self._add_unloaded_chat(tab_id, session)
            self._show_placeholder()
            return
        if restored:
            # Remove the default empty tab if registration dropped it.
            for tid, cv in list(self._chat_views.items()):
                if tid not in self._ctrl.tab_ids:
                    for i in range(self._tab_widget.count()):
                        if self._tab_widget.widget(i) is cv:
                            self._tab_widget.removeTab(i)
                            break
                    cv.shutdown()
                    cv.deleteLater()
                    del self._chat_views[tid]

            for tab_id, session in restored:
                label = self._ctrl.tab_label(tab_id)
                self._pending_restore_messages[tab_id] = session.messages
                self._create_tab(tab_id, label)

            # Align the visible tab with the controller's active tab (which
            # only moved if the default empty tab was dropped).
            active = self._ctrl.active_tab_id
            active_cv = self._chat_views.get(active)
            if active_cv is not None:
                for i in range(self._tab_widget.count()):
                    if self._tab_widget.widget(i) is active_cv:
                        self._tab_widget.setCurrentIndex(i)
                        break
                self._restore_messages_if_needed(active)
            self._update_token_display()
        else:
            # No multi-session history — try legacy single-session restore
            # (at most one session, cheap enough on the UI thread).
            session = self._ctrl.restore_session()
            if session:
                legacy_cv = self._active_chat_view()
                if legacy_cv:
                    legacy_cv.restore_from_messages(session.messages)
                self._update_token_display()

    # --- Mutation log integration ---

    def _on_mutation_recorded(self, event: TurnEvent) -> None:
        """Handle a MUTATION_RECORDED event by adding it to the mutation log panel."""
        if self._mutation_panel is None:
            return
        meta = event.metadata
        record = MutationRecord(
            tool_name=event.tool_name,
            arguments={},
            reverse_tool=meta.get("reverse_tool", ""),
            reverse_arguments=meta.get("reverse_args", {}),
            description=event.text,
            reversible=meta.get("reversible", False),
        )
        self._mutation_panel.add_mutation(record)
        if self._mutations_btn is not None:
            self._mutations_btn.setVisible(True)

    def _on_toggle_mutation_log(self) -> None:
        """Toggle visibility of the mutation log panel."""
        if self._mutation_panel is None:
            return
        visible = not self._mutation_panel.isVisible()
        self._mutation_panel.setVisible(visible)
        if self._mutations_btn is not None:
            self._mutations_btn.setChecked(visible)

    def _on_mode_changed(self, index: int) -> None:
        """Handle the Chat / Tools mode bar switch."""
        self._mode_stack.setCurrentIndex(index)
        if index == 1:
            self._ensure_tools_initialized()
            if self._tools_btn is not None:
                self._tools_btn.setChecked(True)
        else:
            if self._tools_btn is not None:
                self._tools_btn.setChecked(False)

    def _on_toggle_tools(self) -> None:
        """Toggle the Tools view (IDA-docked or embedded mode tab)."""
        if self._tools_panel is None:
            return
        self._ensure_tools_initialized()

        if self._tools_form is not None:
            # IDA dockable form
            if self._tools_form.is_visible:
                self._tools_form.hide()
                if self._tools_btn is not None:
                    self._tools_btn.setChecked(False)
            else:
                self._tools_form.show()
                if self._tools_btn is not None:
                    self._tools_btn.setChecked(True)
        else:
            # Toggle mode bar between Chat (0) and Tools (1)
            current = self._mode_bar.currentIndex()
            self._mode_bar.setCurrentIndex(1 if current == 0 else 0)

    def show_tools_panel(self, tab_index: int = 0) -> None:
        """Show the tools view and switch to the given tab.

        Public API used by IDA actions (Open Tools, Send to Bulk Rename).
        """
        if self._tools_panel is None:
            return
        self._ensure_tools_initialized()

        if self._tools_form is not None:
            self._tools_form.show()
            self._tools_form.set_tab(tab_index)
        else:
            self._mode_bar.setCurrentIndex(1)
            if hasattr(self._tools_panel, "_tabs"):
                self._tools_panel._tabs.setCurrentIndex(tab_index)
        if self._tools_btn is not None:
            self._tools_btn.setChecked(True)

    def show_tools_with_renamer(self, address: int | None = None) -> None:
        """Show the tools panel on the Renamer tab.

        If *address* is given, filter and check that function.
        Called from the IDA "Send to Bulk Rename" right-click action.
        """
        self.show_tools_panel(tab_index=0)
        if address is not None and hasattr(self, "_bulk_renamer"):
            self._bulk_renamer.select_and_filter_address(address)

    def _ensure_tools_initialized(self) -> None:
        """Lazily initialize tools panel contents on first open."""
        if getattr(self, "_tools_initialized", False):
            return
        if self._tools_panel is None:
            return
        self._tools_initialized = True

        from .agent_tree import AgentTreeWidget
        from .bulk_renamer import BulkRenamerWidget

        # Agent tree
        self._agent_tree = AgentTreeWidget()
        self._agent_tree.cancel_requested.connect(self._on_cancel_agent)
        self._agent_tree.inject_summary_requested.connect(self._on_inject_summary)
        self._tools_panel.set_agents_widget(self._agent_tree)

        # Bulk renamer
        self._bulk_renamer = BulkRenamerWidget()
        self._bulk_renamer.start_requested.connect(self._on_renamer_start)
        self._bulk_renamer.pause_requested.connect(self._on_renamer_pause)
        self._bulk_renamer.cancel_requested.connect(self._on_renamer_cancel)
        self._bulk_renamer.undo_requested.connect(self._on_renamer_undo)
        self._bulk_renamer.seek_requested.connect(lambda addr: self._on_renamer_seek(addr))
        self._bulk_renamer.load_more_requested.connect(self._on_renamer_load_more)
        self._bulk_renamer.search_requested.connect(self._on_renamer_search)
        self._tools_panel.set_renamer_widget(self._bulk_renamer)

        # Create IDA dockable form wrapper if factory is available
        if self._tools_form_factory is not None and self._tools_form is None:
            self._tools_form = self._tools_form_factory(self._tools_panel)

        # Populate bulk renamer with functions from the binary.
        # Defer to next event-loop tick so the panel paints first.
        QTimer.singleShot(0, self._load_renamer_functions)

        # Start tools polling timer
        self._tools_poll_timer = QTimer(self)
        self._tools_poll_timer.setInterval(100)
        self._tools_poll_timer.timeout.connect(self._poll_tools_events)
        self._tools_poll_timer.start()

    def _get_or_create_subagent_manager(self):
        """Lazily create the SubagentManager."""
        if hasattr(self, "_subagent_manager"):
            return self._subagent_manager

        from ..agent.subagent_manager import SubagentManager

        provider = self._ctrl.get_provider()
        if provider is None:
            return None
        self._subagent_manager = SubagentManager(
            provider=provider,
            tool_registry=self._ctrl.get_tool_registry(),
            config=self._config,
            host_name=self._ctrl.host_name,
            skill_registry=getattr(self._ctrl, "_skill_registry", None),
        )
        return self._subagent_manager

    def _get_or_create_renamer_engine(self, batch_size: int, max_workers: int):
        """Create a BulkRenamerEngine for the current session."""
        from ..agent.bulk_renamer import BulkRenamerEngine

        provider = self._ctrl.get_provider()
        if provider is None:
            return None
        return BulkRenamerEngine(
            provider=provider,
            tool_registry=self._ctrl.get_tool_registry(),
            config=self._config,
            host_name=self._ctrl.host_name,
            skill_registry=getattr(self._ctrl, "_skill_registry", None),
            batch_size=batch_size,
            max_workers=max_workers,
            subagent_manager=self._get_or_create_subagent_manager(),
        )

    def _load_renamer_functions(self) -> None:
        """Populate the bulk renamer widget with functions from the binary.

        Fetches pages of functions one at a time via QTimer so the UI thread
        stays responsive between pages (avoids blocking on large binaries).
        """
        if not hasattr(self, "_bulk_renamer"):
            return

        tool_registry = self._ctrl.get_tool_registry()
        defn = tool_registry.get("list_functions")
        if defn is None or defn.handler is None:
            log_info("list_functions tool not available — renamer table will be empty")
            return

        # State for the incremental page fetcher. The widget requests more
        # pages as needed; do not walk the whole function list up front.
        self._renamer_load_offset = 0
        self._renamer_load_batch = 250
        self._renamer_load_defn = defn
        self._renamer_load_total: int | str | None = None
        self._renamer_loading_page = False
        self._bulk_renamer.begin_function_load()
        QTimer.singleShot(0, self._fetch_renamer_page)

    def _on_renamer_load_more(self) -> None:
        """Fetch the next function page when the renamer asks for it."""
        if getattr(self, "_renamer_loading_page", False):
            return
        if getattr(self, "_renamer_load_defn", None) is None:
            return
        QTimer.singleShot(0, self._fetch_renamer_page)

    def _fetch_renamer_page(self) -> None:
        """Fetch one function page and append it to the renamer table."""
        defn = self._renamer_load_defn
        offset = self._renamer_load_offset
        batch = self._renamer_load_batch
        self._renamer_loading_page = True
        self._bulk_renamer.set_function_page_loading(True)

        try:
            raw = defn.handler(offset=offset, limit=batch)
        except Exception as e:
            log_error(f"list_functions failed at offset {offset}: {e}")
            raw = None

        functions, total_hint = _parse_function_page(raw or "")
        if total_hint is not None:
            self._renamer_load_total = total_hint

        for i in range(len(functions) - 1):
            functions[i]["instruction_count"] = functions[i + 1]["address"] - functions[i]["address"]

        page_count = len(functions)
        next_offset = offset + page_count
        total = self._renamer_load_total
        has_more = page_count >= batch and (not isinstance(total, int) or next_offset < total)
        self._renamer_load_offset = next_offset
        self._renamer_loading_page = False
        self._bulk_renamer.append_function_page(
            functions,
            has_more=has_more,
            total_hint=total,
        )

        if functions:
            log_info(f"Loaded bulk renamer function page {offset}-{next_offset}")
        elif offset == 0:
            log_info("No functions found for bulk renamer")

        if not has_more:
            self._renamer_load_defn = None

    def _on_renamer_search(self, query: str) -> None:
        """Search unloaded functions for the current renamer filter."""
        if getattr(self, "_renamer_searching_query", None):
            return
        self._renamer_searching_query = query
        QTimer.singleShot(0, lambda: self._search_renamer_functions(query))

    def _search_renamer_functions(self, query: str) -> None:
        """Append functions found outside the currently loaded renamer pages."""
        if not hasattr(self, "_bulk_renamer"):
            return

        tool_registry = self._ctrl.get_tool_registry()
        functions: list[dict] = []
        query = query.strip()
        try:
            if _HEX_ADDRESS_QUERY_RE.fullmatch(query):
                defn = tool_registry.get("get_function_info")
                if defn is not None and defn.handler is not None:
                    raw = defn.handler(address=query)
                    parsed = _parse_function_info_result(raw or "")
                    if parsed is not None:
                        functions.append(parsed)
            else:
                defn = tool_registry.get("search_functions")
                if defn is not None and defn.handler is not None:
                    raw = defn.handler(query=query, limit=50)
                    functions, _total_hint = _parse_function_page(raw or "")
        except Exception as e:
            log_error(f"Renamer function search failed for {query!r}: {e}")
        finally:
            self._renamer_searching_query = ""

        if functions:
            self._bulk_renamer.append_discovered_functions(functions)
        self._bulk_renamer.set_search_loading(False)

    # --- Tools panel event handlers ---

    def _on_cancel_agent(self, agent_id: str) -> None:
        """Handle agent cancel request from AgentTreeWidget."""
        mgr = self._get_or_create_subagent_manager()
        if mgr is not None:
            mgr.cancel(agent_id)

    def _on_inject_summary(self, agent_id: str) -> None:
        """Inject a completed agent's summary into the active chat."""
        mgr = self._get_or_create_subagent_manager()
        if mgr is None:
            return
        info = mgr.get(agent_id)
        if info is None or not info.summary:
            return
        elapsed = (info.completed_at or info.created_at) - info.created_at
        text = (
            f"[Subagent \u201c{info.name}\u201d completed ({info.turn_count} turns, {elapsed:.0f}s)]\n\n{info.summary}"
        )
        self._start_agent(text)

    def _on_renamer_start(self, jobs, mode, batch_size, max_concurrent) -> None:
        """Handle bulk renamer start request."""
        from ..agent.bulk_renamer import RenameJob

        engine = self._get_or_create_renamer_engine(batch_size, max_concurrent)
        if engine is None:
            log_error("Cannot start renamer: LLM provider not available")
            return
        rename_jobs = [RenameJob(address=j["address"], current_name=j["current_name"]) for j in jobs]
        engine.enqueue(rename_jobs)
        self._renamer_engine = engine
        engine.start_renaming(deep=(mode == "deep"))

    def _on_renamer_pause(self) -> None:
        engine = getattr(self, "_renamer_engine", None)
        if engine is not None:
            if engine._paused.is_set():
                engine.pause()
            else:
                engine.resume()

    def _on_renamer_cancel(self) -> None:
        engine = getattr(self, "_renamer_engine", None)
        if engine is not None:
            engine.cancel_renaming()

    def _on_renamer_undo(self) -> None:
        engine = getattr(self, "_renamer_engine", None)
        if engine is None:
            return
        # undo_all calls tool_registry.execute which goes through
        # TPE + idasync — must run off the main thread to avoid deadlock.
        threading.Thread(target=engine.undo_all, daemon=True, name="rikugan-undo-renames").start()

    def _on_renamer_seek(self, address: int) -> None:
        """Navigate the host disassembly view to the given address."""
        from ..core.host import navigate_to

        navigate_to(address)

    def _poll_tools_events(self) -> None:
        """Poll all tools subsystems for events."""
        if self._is_shutdown:
            return

        # Poll subagent manager events
        mgr = getattr(self, "_subagent_manager", None)
        if mgr is not None:
            for _ in range(10):
                event = mgr.poll_event()
                if event is None:
                    break
                # Update agent tree
                if hasattr(self, "_agent_tree"):
                    from .agent_tree import AgentInfo

                    meta = event.metadata or {}
                    agent_id = meta.get("agent_id", "")
                    info = mgr.get(agent_id)
                    if info is not None:
                        elapsed = (info.completed_at or time.time()) - info.created_at
                        self._agent_tree.update_agent(
                            AgentInfo(
                                agent_id=info.id,
                                name=info.name,
                                agent_type=info.agent_type,
                                status=info.status.value.upper(),
                                turns=info.turn_count,
                                elapsed_seconds=elapsed,
                                summary=info.summary,
                                category=info.category,
                            )
                        )
                # Show in chat for spawned/completed/failed — but skip
                # bulk_rename agents to avoid polluting the conversation.
                if event.type in (
                    TurnEventType.SUBAGENT_SPAWNED,
                    TurnEventType.SUBAGENT_COMPLETED,
                    TurnEventType.SUBAGENT_FAILED,
                ):
                    is_bulk = info is not None and info.category == "bulk_rename"
                    if not is_bulk:
                        chat_view = self._active_chat_view()
                        if chat_view is not None:
                            chat_view.handle_event(event)

            # Refresh elapsed time for all RUNNING agents (~1 Hz, not every tick)
            now = time.time()
            last_sweep = getattr(self, "_last_agent_sweep", 0.0)
            if hasattr(self, "_agent_tree") and (now - last_sweep) >= 1.0:
                self._last_agent_sweep = now
                from .agent_tree import AgentInfo

                for info in mgr.list_all():
                    if info.status.value == "running":
                        elapsed = now - info.created_at
                        self._agent_tree.update_agent(
                            AgentInfo(
                                agent_id=info.id,
                                name=info.name,
                                agent_type=info.agent_type,
                                status=info.status.value.upper(),
                                turns=info.turn_count,
                                elapsed_seconds=elapsed,
                                summary=info.summary,
                                category=info.category,
                            )
                        )

        # Poll bulk renamer events
        engine = getattr(self, "_renamer_engine", None)
        if engine is not None:
            from ..agent.bulk_renamer import RenameEventType

            for _ in range(20):
                rename_event = engine.poll_event()
                if rename_event is None:
                    break
                if hasattr(self, "_bulk_renamer"):
                    _RENAME_STATUS_MAP = {
                        RenameEventType.JOB_STARTED: "analyzing",
                        RenameEventType.JOB_COMPLETED: "renamed",
                        RenameEventType.JOB_ERROR: "error",
                    }
                    if rename_event.type in _RENAME_STATUS_MAP:
                        status = _RENAME_STATUS_MAP[rename_event.type]
                        # Undo: JOB_COMPLETED with empty new_name means reverted
                        if rename_event.type == RenameEventType.JOB_COMPLETED and not rename_event.new_name:
                            status = "reverted"
                        self._bulk_renamer.update_job(
                            rename_event.address,
                            rename_event.new_name,
                            status,
                            rename_event.error,
                        )
                    if rename_event.type in (
                        RenameEventType.BATCH_PROGRESS,
                        RenameEventType.ALL_DONE,
                    ):
                        self._bulk_renamer.set_progress(
                            rename_event.completed,
                            rename_event.total,
                        )

    def _on_undo_requested(self, count: int) -> None:
        """Handle undo request from the mutation log panel."""
        if self._is_shutdown:
            return
        # Submit /undo command through the normal agent path
        self._start_agent(f"/undo {count}")

    def _set_running(self, running: bool, tab_id: str | None = None) -> None:
        tab_id = tab_id or self._ctrl.active_tab_id
        active_tab_id = self._ctrl.active_tab_id
        active_running = self._ctrl.is_tab_running(active_tab_id)
        if tab_id == active_tab_id:
            active_running = running
        self._pending_answer = active_tab_id in self._pending_answer_tabs
        self._awaiting_button_approval = active_tab_id in self._awaiting_approval_tabs
        # Keep input enabled so users can queue follow-up messages while
        # running — UNLESS we're waiting for a button-only approval.
        if self._composer is not None:
            # set_input_enabled resets the placeholder, so order matters here.
            self._composer.set_input_enabled(not self._awaiting_button_approval)
            if self._awaiting_button_approval:
                self._composer.set_placeholder(_PLACEHOLDER_APPROVAL)
            else:
                self._composer.set_placeholder(_PLACEHOLDER_RUNNING if active_running else _PLACEHOLDER_IDLE)
            self._composer.set_running(active_running)

        if self._context_bar is not None:
            if active_running:
                state = "running"
            elif self._tab_status.get(active_tab_id) == "error":
                state = "error"
            else:
                state = "idle"
            self._context_bar.set_state(state)
        self._refresh_chat_sidebar()

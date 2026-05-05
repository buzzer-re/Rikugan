"""Tool call, batch, group, and approval widgets."""

from __future__ import annotations

import json
import re as _re
from typing import ClassVar

from .qt_compat import (
    QColor,
    QFont,
    QFrame,
    QHBoxLayout,
    QLabel,
    QPlainTextEdit,
    QSyntaxHighlighter,
    Qt,
    QTextCharFormat,
    QTimer,
    QToolButton,
    QVBoxLayout,
    QWidget,
    qt_flags,
)
from .styles import host_stylesheet, use_native_host_theme

_MAX_ARGS_DISPLAY = 2000
_MAX_RESULT_DISPLAY = 3000
_TOOL_PREVIEW_LINES = 3
_MUTED_TEXT = "#808080"
_TOOL_BG = "#252526"
_TOOL_BORDER = "#3c3c3c"


def _tool_card_css(
    source=None,
    *,
    accent: str | None = None,
    background: str | None = None,
    radius: int = 6,
    object_name: str = "message_tool",
) -> str:
    del source
    border = accent or _TOOL_BORDER
    bg = background or _TOOL_BG
    return f"QFrame#{object_name} {{ background-color: {bg}; border: 1px solid {border}; border-radius: {radius}px; }}"


def _native_text_style(
    *,
    size: int | None = None,
    bold: bool = False,
    italic: bool = False,
    monospace: bool = False,
) -> str:
    parts: list[str] = []
    if size is not None:
        parts.append(f"font-size: {size}px;")
    if bold:
        parts.append("font-weight: bold;")
    if italic:
        parts.append("font-style: italic;")
    if monospace:
        parts.append('font-family: Consolas, "Courier New", monospace;')
    return " ".join(parts)


# ---------------------------------------------------------------------------
# MCP prefix stripping - works with any MCP server, not just a specific one
# ---------------------------------------------------------------------------


def _strip_mcp_prefix(name: str) -> str:
    """Strip MCP server prefix (``mcp__<server>__``) from tool names."""
    if name.startswith("mcp__"):
        rest = name[5:]  # after "mcp__"
        idx = rest.find("__")
        if idx >= 0:
            return rest[idx + 2 :]
    return name


# ---------------------------------------------------------------------------
# Tool-specific colors (by base name - MCP prefix is stripped before lookup)
# ---------------------------------------------------------------------------
_TOOL_COLORS: dict[str, str] = {}

# Analysis (read-only) -> teal/cyan
for _t in (
    "decompile_function",
    "read_disassembly",
    "read_function_disassembly",
    "get_binary_info",
    "list_imports",
    "list_exports",
    "list_functions",
    "search_functions",
    "list_strings",
    "search_strings",
    "get_string_at",
    "list_segments",
    "xrefs_to",
    "xrefs_from",
    "function_xrefs",
    "get_microcode",
    "get_cursor_position",
    "get_current_function",
    "fetch_disassembly",
    "list_methods",
    "list_strings_filter",
    "list_sections",
    "get_xrefs_to",
    "get_xrefs_to_field",
    "get_xrefs_to_struct",
    "get_xrefs_to_type",
    "get_xrefs_to_enum",
    "get_xrefs_to_union",
    "get_il",
    "get_binary_status",
    "hexdump_address",
    "hexdump_data",
    "get_data_decl",
    "search_functions_by_name",
    "function_at",
    "get_entry_points",
    "list_classes",
    "list_namespaces",
    "list_data_items",
    "list_all_strings",
    "list_local_types",
    "search_types",
    "get_type_info",
    "get_user_defined_type",
    "get_comment",
    "get_function_comment",
    "list_binaries",
    "select_binary",
    "list_platforms",
    "convert_number",
    "format_value",
):
    _TOOL_COLORS[_t] = "#4ec9b0"  # teal/cyan

# Modification -> magenta/purple
for _t in (
    "rename_function",
    "rename_variable",
    "rename_address",
    "set_type",
    "set_function_prototype",
    "set_comment",
    "set_function_comment",
    "create_struct",
    "create_enum",
    "nop_microcode",
    "install_microcode_optimizer",
    "redecompile_function",
    "apply_struct_to_address",
    "rename_single_variable",
    "rename_multi_variables",
    "retype_variable",
    "define_types",
    "declare_c_type",
    "rename_data",
    "set_local_variable_type",
    "make_function_at",
    "delete_comment",
    "delete_function_comment",
):
    _TOOL_COLORS[_t] = "#c586c0"  # magenta/purple

# Exploration -> gold/amber
for _t in ("exploration_report", "phase_transition"):
    _TOOL_COLORS[_t] = "#d7ba7d"

# Scripting -> green
for _t in ("execute_python",):
    _TOOL_COLORS[_t] = "#6a9955"

_DEFAULT_TOOL_COLOR = "#569cd6"  # blue

_TOOL_GROUP_LABELS: dict[str, tuple[str, str]] = {
    "decompile_function": ("Decompiled", "function"),
    "read_disassembly": ("Disassembled", "function"),
    "read_function_disassembly": ("Disassembled", "function"),
    "fetch_disassembly": ("Disassembled", "function"),
    "search_strings": ("Searched", "string"),
    "list_strings_filter": ("Searched", "string"),
    "search_functions": ("Searched", "function"),
    "search_functions_by_name": ("Searched", "function"),
    "read_file": ("Read", "file"),
}


def _tool_color(name: str) -> str:
    """Look up tool color by base name (MCP prefix stripped)."""
    return _TOOL_COLORS.get(_strip_mcp_prefix(name), _DEFAULT_TOOL_COLOR)


def _format_tool_group_label(tool_names: list[str]) -> str:
    """Human-friendly group label for collapsed tool-call runs."""
    count = len(tool_names)
    if count <= 0:
        return "0 tools called"

    base_names = [_strip_mcp_prefix(name) for name in tool_names]
    unique_names = {name for name in base_names if name}

    if len(unique_names) == 1:
        only_name = base_names[0]
        template = _TOOL_GROUP_LABELS.get(only_name)
        if template:
            action, noun = template
            suffix = "" if count == 1 else "s"
            return f"{action} {count} {noun}{suffix}"

    return f"{count} tool called" if count == 1 else f"{count} tools called"


# ---------------------------------------------------------------------------
# Smart tool parameter summaries
# ---------------------------------------------------------------------------


def _format_tool_summary(tool_name: str, args_text: str) -> str:
    """Extract the most relevant parameter for a compact one-line summary."""
    try:
        args = json.loads(args_text) if args_text else {}
    except (json.JSONDecodeError, TypeError):
        return ""

    if not isinstance(args, dict):
        return ""

    def _get(*keys: str) -> str:
        for k in keys:
            v = args.get(k)
            if v is not None:
                return str(v)
        return ""

    # Strip MCP prefix for matching (works with any MCP server)
    short_name = _strip_mcp_prefix(tool_name)

    summary = ""

    if short_name in ("decompile_function",):
        target = _get("address", "ea", "name", "target", "func_id")
        if target:
            summary = target

    elif short_name in ("rename_function",):
        old = _get("old_name", "current_name", "ea")
        new = _get("new_name")
        if old and new:
            summary = f"{old} \u2192 {new}"

    elif short_name in ("rename_single_variable", "rename_variable"):
        func = _get("function_name", "function", "ea")
        old = _get("variable_name", "var_name", "old_name")
        new = _get("new_name")
        if old and new:
            summary = f"{func}: {old} \u2192 {new}" if func else f"{old} \u2192 {new}"

    elif short_name in ("rename_multi_variables",):
        func = _get("function_identifier", "function_name", "ea")
        summary = func if func else ""

    elif short_name in ("set_comment", "set_function_comment"):
        addr = _get("address", "ea", "function_name")
        comment = _get("comment", "text")
        if comment and len(comment) > 50:
            comment = comment[:47] + "..."
        if addr and comment:
            summary = f"{addr}: {comment}"
        elif comment:
            summary = comment

    elif short_name in (
        "set_type",
        "set_function_prototype",
        "retype_variable",
        "set_local_variable_type",
    ):
        target = _get("ea", "address", "name", "name_or_address", "variable_name")
        type_str = _get("type_str", "prototype", "new_type", "type")
        if target and type_str:
            summary = f"{target}: {type_str}"

    elif short_name in (
        "xrefs_to",
        "xrefs_from",
        "function_xrefs",
        "get_xrefs_to",
        "get_xrefs_to_field",
    ):
        target = _get("address", "ea", "name", "struct_name")
        if target:
            summary = target

    elif short_name in (
        "search_strings",
        "search_functions",
        "search_functions_by_name",
        "list_strings_filter",
    ):
        query = _get("pattern", "query", "filter", "name")
        if query:
            summary = f'"{query}"'

    elif short_name in ("define_types", "declare_c_type"):
        code = _get("c_code", "c_declaration", "types")
        if code and len(code) > 60:
            code = code[:57] + "..."
        summary = code or ""

    elif short_name in ("create_struct",):
        name = _get("name", "struct_name")
        summary = name

    elif short_name in ("execute_python",):
        code = _get("code", "script")
        if code:
            first_line = code.strip().split("\n")[0]
            if len(first_line) > 60:
                first_line = first_line[:57] + "..."
            summary = first_line

    elif short_name in (
        "fetch_disassembly",
        "read_disassembly",
        "read_function_disassembly",
    ):
        target = _get("name", "ea", "address", "start")
        if target:
            summary = target

    elif short_name in ("get_il",):
        target = _get("name_or_address")
        view = _get("view")
        if target:
            summary = f"{target}" + (f" ({view})" if view else "")

    elif short_name in ("hexdump_address", "hexdump_data", "get_data_decl"):
        target = _get("address", "name_or_address")
        if target:
            summary = target

    elif short_name == "exploration_report":
        cat = _get("category")
        sm = _get("summary")
        if sm and len(sm) > 50:
            sm = sm[:47] + "..."
        summary = f"[{cat}] {sm}" if cat and sm else sm or cat or ""

    elif short_name == "phase_transition":
        phase = _get("to_phase")
        reason = _get("reason")
        if reason and len(reason) > 40:
            reason = reason[:37] + "..."
        summary = f"\u2192 {phase}" + (f": {reason}" if reason else "")

    else:
        # Generic: try common parameter names
        for key in (
            "target",
            "address",
            "ea",
            "name",
            "path",
            "query",
            "pattern",
            "command",
            "name_or_address",
        ):
            val = _get(key)
            if val:
                summary = val
                break

    # Truncate
    if len(summary) > 80:
        summary = summary[:77] + "..."
    return summary


def _truncate_preview(text: str, max_lines: int = _TOOL_PREVIEW_LINES) -> str:
    """Return first N lines with a '... +M lines' indicator if truncated."""
    lines = text.split("\n")
    if len(lines) <= max_lines:
        return text
    preview = "\n".join(lines[:max_lines])
    remaining = len(lines) - max_lines
    return f"{preview}\n\u2026 +{remaining} lines"


def _make_preview_label(source=None) -> QLabel:
    """Create a collapsed-state preview QLabel (shared by ToolCallWidget/ToolBatchWidget)."""
    del source
    lbl = QLabel()
    lbl.setObjectName("tool_content")
    lbl.setWordWrap(True)
    lbl.setStyleSheet(
        host_stylesheet(
            "color: #6a6a7a; font-family: monospace; font-size: 10px; margin-left: 28px;",
            f"color: {_MUTED_TEXT}; {_native_text_style(size=10, monospace=True)}",
        )
    )
    lbl.setVisible(False)
    return lbl


# ---------------------------------------------------------------------------
# Shared spinner timer
# ---------------------------------------------------------------------------


class _SharedSpinnerTimer:
    """Single QTimer shared across all ToolCallWidget instances.

    Instead of N timers (one per widget), one 100ms timer ticks all
    active spinners.  Starts automatically when the first widget
    registers and stops when the last one unregisters.
    """

    _instance: _SharedSpinnerTimer | None = None

    def __init__(self) -> None:
        self._timer = QTimer()
        self._timer.setInterval(100)
        self._timer.timeout.connect(self._tick)
        self._widgets: set[ToolCallWidget] = set()

    @classmethod
    def get(cls) -> _SharedSpinnerTimer:
        if cls._instance is None:
            cls._instance = cls()
        return cls._instance

    def register(self, widget: ToolCallWidget) -> None:
        self._widgets.add(widget)
        # NOTE: Do NOT connect to widget.destroyed here.  PySide6/Shiboken
        # under IDA may GC the lambda slot, leaving a dangling C++ pointer
        # in Qt's connection list.  When *any* QWidget is later destroyed,
        # the stale slot pointer causes a SIGBUS (0xaaaa freed-memory read).
        # Stale widgets are already handled safely in _tick() via RuntimeError.
        if not self._timer.isActive():
            self._timer.start()

    def unregister(self, widget: ToolCallWidget) -> None:
        self._widgets.discard(widget)
        if not self._widgets and self._timer.isActive():
            self._timer.stop()

    def _tick(self) -> None:
        stale: set = set()
        for w in list(self._widgets):
            try:
                w._spin_tick()
            except RuntimeError:
                # Shiboken raises RuntimeError when the C++ object was already deleted.
                stale.add(w)
        if stale:
            self._widgets -= stale
            if not self._widgets and self._timer.isActive():
                self._timer.stop()

    @classmethod
    def shutdown(cls) -> None:
        """Stop and discard the singleton so the QTimer doesn't outlive QApplication."""
        inst = cls._instance
        if inst is not None:
            inst._widgets.clear()
            inst._timer.stop()
            inst._timer.deleteLater()
            cls._instance = None


class ToolCallWidget(QFrame):
    """Compact tool call display.

    Shows:  [dot] tool_name  summary_text
    With a collapsible detail section for args and result.
    """

    _SPINNER_FRAMES = (
        "\u280b",
        "\u2819",
        "\u2839",
        "\u2838",
        "\u283c",
        "\u2834",
        "\u2826",
        "\u2827",
        "\u2807",
        "\u280f",
    )

    def __init__(self, tool_name: str, tool_call_id: str, parent: QWidget = None):
        super().__init__(parent)
        self.setObjectName("message_tool")
        self.setStyleSheet(_tool_card_css(object_name="message_tool"))
        self._tool_name = tool_name
        self._tool_call_id = tool_call_id
        self._args_text = ""
        self._result_text = ""
        self._is_error = False
        self._expanded = False
        self._spin_idx = 0

        layout = QVBoxLayout(self)
        layout.setContentsMargins(6, 3, 6, 3)
        layout.setSpacing(2)

        layout.addLayout(self._build_header(tool_name))
        _SharedSpinnerTimer.get().register(self)
        layout.addWidget(self._build_preview())
        layout.addWidget(self._build_detail_section())

    def _build_header(self, tool_name: str) -> QHBoxLayout:
        """Build the compact header row: toggle bullet name summary status."""
        display_name = _strip_mcp_prefix(tool_name)
        color = _tool_color(tool_name)

        header_layout = QHBoxLayout()
        header_layout.setContentsMargins(0, 0, 0, 0)
        header_layout.setSpacing(0)

        self._toggle_btn = QToolButton()
        self._toggle_btn.setObjectName("collapse_button")
        self._toggle_btn.setText("\u25b6")
        self._toggle_btn.setFixedSize(14, 14)
        self._toggle_btn.clicked.connect(self._toggle)
        header_layout.addWidget(self._toggle_btn)

        self._bullet = QLabel("\u25cf")
        self._bullet.setStyleSheet(
            host_stylesheet(
                f"color: {color}; font-size: 10px;",
                f"color: {color}; {_native_text_style(size=10)}",
            )
        )
        self._bullet.setFixedWidth(14)
        header_layout.addWidget(self._bullet)

        self._name_label = QLabel(display_name)
        self._name_label.setStyleSheet(
            host_stylesheet(
                f"color: {color}; font-weight: bold; font-size: 11px;",
                f"color: {color}; {_native_text_style(size=11, bold=True)}",
            )
        )
        header_layout.addWidget(self._name_label)

        self._summary_label = QLabel("")
        self._summary_label.setStyleSheet(
            host_stylesheet(
                "color: #808080; font-size: 11px; margin-left: 6px;",
                f"color: {_MUTED_TEXT}; {_native_text_style(size=11)}",
            )
        )
        header_layout.addWidget(self._summary_label, 1)

        self._status_label = QLabel(self._SPINNER_FRAMES[0])
        self._status_label.setStyleSheet(
            host_stylesheet(
                "color: #dcdcaa; font-size: 10px;",
                f"color: #dcdcaa; {_native_text_style(size=10, bold=True)}",
            )
        )
        header_layout.addWidget(self._status_label)

        return header_layout

    def _build_preview(self) -> QLabel:
        """Build the preview label (truncated args, shown when collapsed)."""
        self._preview_label = _make_preview_label(self)
        return self._preview_label

    def _build_detail_section(self) -> QWidget:
        """Build the expandable detail area (args + result)."""
        self._detail_widget = QWidget()
        self._detail_layout = QVBoxLayout(self._detail_widget)
        self._detail_layout.setContentsMargins(28, 2, 0, 2)
        self._detail_layout.setSpacing(2)

        self._args_label = QLabel()
        self._args_label.setObjectName("tool_content")
        self._args_label.setWordWrap(True)
        self._args_label.setTextFormat(Qt.TextFormat.PlainText)
        self._args_label.setTextInteractionFlags(
            qt_flags(
                Qt.TextInteractionFlag.TextSelectableByMouse,
                Qt.TextInteractionFlag.TextSelectableByKeyboard,
            )
        )
        self._detail_layout.addWidget(self._args_label)

        self._result_header = QLabel("Result:")
        self._result_header.setStyleSheet(
            host_stylesheet(
                "color: #808080; font-size: 10px; font-weight: bold;",
                f"color: {_MUTED_TEXT}; {_native_text_style(size=10, bold=True)}",
            )
        )
        self._result_header.setVisible(False)
        self._detail_layout.addWidget(self._result_header)

        self._result_label = QLabel()
        self._result_label.setObjectName("tool_content")
        self._result_label.setWordWrap(True)
        self._result_label.setTextFormat(Qt.TextFormat.PlainText)
        self._result_label.setTextInteractionFlags(
            qt_flags(
                Qt.TextInteractionFlag.TextSelectableByMouse,
                Qt.TextInteractionFlag.TextSelectableByKeyboard,
            )
        )
        self._result_label.setVisible(False)
        self._detail_layout.addWidget(self._result_label)

        self._detail_widget.setVisible(False)
        return self._detail_widget

    def _spin_tick(self) -> None:
        """Advance the spinner animation frame."""
        self._spin_idx = (self._spin_idx + 1) % len(self._SPINNER_FRAMES)
        self._status_label.setText(self._SPINNER_FRAMES[self._spin_idx])

    def _stop_spinner(self) -> None:
        """Unregister from the shared spinner timer."""
        _SharedSpinnerTimer.get().unregister(self)

    def _toggle(self) -> None:
        self.setUpdatesEnabled(False)
        self._expanded = not self._expanded
        self._detail_widget.setVisible(self._expanded)
        self._preview_label.setVisible(not self._expanded and bool(self._args_text))
        self._toggle_btn.setText("\u25bc" if self._expanded else "\u25b6")
        self.setUpdatesEnabled(True)

    def set_arguments(self, args_text: str) -> None:
        self._args_text = args_text
        # Update summary
        summary = _format_tool_summary(self._tool_name, args_text)
        if summary:
            self._summary_label.setText(summary)
        # Preview (truncated)
        if args_text.strip():
            self._preview_label.setText(_truncate_preview(args_text.strip()))
            self._preview_label.setVisible(not self._expanded)
        # Full args in detail area
        display = args_text[:_MAX_ARGS_DISPLAY] + "..." if len(args_text) > _MAX_ARGS_DISPLAY else args_text
        self._args_label.setText(display)

    def append_args_delta(self, delta: str) -> None:
        self._args_text += delta
        # Don't update preview during streaming - wait for set_arguments

    def set_result(self, result: str, is_error: bool = False) -> None:
        self._stop_spinner()
        self._result_text = result
        self._is_error = is_error
        display = result[:_MAX_RESULT_DISPLAY] + "\n... (truncated)" if len(result) > _MAX_RESULT_DISPLAY else result
        self._result_label.setText(display)
        self._result_label.setVisible(True)
        self._result_header.setVisible(True)
        if is_error:
            self._result_label.setStyleSheet(
                host_stylesheet(
                    "color: #f44747; font-family: monospace; font-size: 11px;",
                    f"color: #f44747; {_native_text_style(size=11, monospace=True)}",
                )
            )
            self._status_label.setText("\u2717")
            self._status_label.setStyleSheet(
                host_stylesheet(
                    "color: #f44747; font-size: 10px;",
                    f"color: #f44747; {_native_text_style(size=10, bold=True)}",
                )
            )
            self._bullet.setStyleSheet(
                host_stylesheet(
                    "color: #f44747; font-size: 10px;",
                    "color: #f44747; font-size: 10px;",
                )
            )
            # Auto-expand on error
            self._expanded = True
            self._detail_widget.setVisible(True)
            self._preview_label.setVisible(False)
            self._toggle_btn.setText("\u25bc")
        else:
            self._status_label.setText("\u2713")
            self._status_label.setStyleSheet(
                host_stylesheet(
                    "color: #4ec9b0; font-size: 10px;",
                    f"color: #4ec9b0; {_native_text_style(size=10, bold=True)}",
                )
            )

    def mark_done(self) -> None:
        self._stop_spinner()
        if self._status_label.text() not in ("\u2713", "\u2717"):
            self._status_label.setText("\u2713")
            self._status_label.setStyleSheet(
                host_stylesheet(
                    "color: #4ec9b0; font-size: 10px;",
                    f"color: #4ec9b0; {_native_text_style(size=10, bold=True)}",
                )
            )

    def hide_preview(self) -> None:
        """Hide the args preview (used when preview budget exhausted)."""
        self._preview_label.setVisible(False)


# ---------------------------------------------------------------------------
# Tool batch widget - groups N consecutive calls to the same tool
# ---------------------------------------------------------------------------


class ToolBatchWidget(QFrame):
    """Groups consecutive calls to the same tool.

    Shows:  [dot] tool_name  (N calls)
    With preview of the first call's args.
    """

    def __init__(self, tool_name: str, parent: QWidget = None):
        super().__init__(parent)
        self.setObjectName("message_tool")
        self.setStyleSheet(_tool_card_css(object_name="message_tool"))
        self._tool_name = tool_name
        self._count = 0
        self._expanded = False
        self._first_args: str = ""
        self._tool_call_ids: list[str] = []
        self._results: dict[str, str] = {}
        self._errors: dict[str, str] = {}
        self._entry_labels: list[QLabel] = []

        layout = QVBoxLayout(self)
        layout.setContentsMargins(6, 3, 6, 3)
        layout.setSpacing(2)

        layout.addLayout(self._build_header(tool_name))
        layout.addWidget(self._build_preview())
        layout.addWidget(self._build_detail_section())

    def _build_header(self, tool_name: str) -> QHBoxLayout:
        """Build the compact header row: toggle bullet name count status."""
        display_name = _strip_mcp_prefix(tool_name)
        color = _tool_color(tool_name)

        header_layout = QHBoxLayout()
        header_layout.setContentsMargins(0, 0, 0, 0)
        header_layout.setSpacing(0)

        self._toggle_btn = QToolButton()
        self._toggle_btn.setObjectName("collapse_button")
        self._toggle_btn.setText("\u25b6")
        self._toggle_btn.setFixedSize(14, 14)
        self._toggle_btn.clicked.connect(self._toggle)
        header_layout.addWidget(self._toggle_btn)

        self._bullet = QLabel("\u25cf")
        self._bullet.setStyleSheet(
            host_stylesheet(
                f"color: {color}; font-size: 10px;",
                f"color: {color}; {_native_text_style(size=10)}",
            )
        )
        self._bullet.setFixedWidth(14)
        header_layout.addWidget(self._bullet)

        self._name_label = QLabel(display_name)
        self._name_label.setStyleSheet(
            host_stylesheet(
                f"color: {color}; font-weight: bold; font-size: 11px;",
                f"color: {color}; {_native_text_style(size=11, bold=True)}",
            )
        )
        header_layout.addWidget(self._name_label)

        self._count_label = QLabel("")
        self._count_label.setStyleSheet(
            host_stylesheet(
                "color: #808080; font-size: 11px; margin-left: 6px;",
                f"color: {_MUTED_TEXT}; {_native_text_style(size=11)}",
            )
        )
        header_layout.addWidget(self._count_label, 1)

        self._status_label = QLabel("\u2026")
        self._status_label.setStyleSheet(
            host_stylesheet(
                "color: #dcdcaa; font-size: 10px;",
                f"color: #dcdcaa; {_native_text_style(size=10, bold=True)}",
            )
        )
        header_layout.addWidget(self._status_label)

        return header_layout

    def _build_preview(self) -> QLabel:
        """Build the preview label for the first call's args."""
        self._preview_label = _make_preview_label(self)
        return self._preview_label

    def _build_detail_section(self) -> QWidget:
        """Build the expandable detail area for all calls."""
        self._detail_widget = QWidget()
        self._detail_layout = QVBoxLayout(self._detail_widget)
        self._detail_layout.setContentsMargins(28, 2, 0, 2)
        self._detail_layout.setSpacing(4)
        self._detail_widget.setVisible(False)
        return self._detail_widget

    def add_call(self, tool_call_id: str, args_text: str = "") -> None:
        """Add another call to this batch."""
        self._count += 1
        self._tool_call_ids.append(tool_call_id)
        self._count_label.setText(f"({self._count} calls)")

        if self._count == 1 and args_text.strip():
            self._first_args = args_text
            summary = _format_tool_summary(self._tool_name, args_text)
            # For first call, show summary alongside count
            preview = _truncate_preview(args_text.strip())
            self._preview_label.setText(preview)
            self._preview_label.setVisible(not self._expanded)

        # Add entry in detail area
        summary = _format_tool_summary(self._tool_name, args_text) if args_text else ""
        entry = QLabel(f"#{self._count}: {summary}" if summary else f"#{self._count}")
        entry.setStyleSheet(
            host_stylesheet(
                "color: #808080; font-family: monospace; font-size: 10px;",
                f"color: {_MUTED_TEXT}; {_native_text_style(size=10, monospace=True)}",
            )
        )
        entry.setWordWrap(True)
        self._entry_labels.append(entry)  # prevent Shiboken GC
        self._detail_layout.addWidget(entry)

    def set_args_for_call(self, tool_call_id: str, args_text: str) -> None:
        """Update args for a specific call (used when streaming completes)."""
        idx = -1
        for i, tid in enumerate(self._tool_call_ids):
            if tid == tool_call_id:
                idx = i
                break
        if idx < 0:
            return

        if idx == 0 and not self._first_args:
            self._first_args = args_text
            preview = _truncate_preview(args_text.strip())
            self._preview_label.setText(preview)
            self._preview_label.setVisible(not self._expanded)

        # Update detail entry
        summary = _format_tool_summary(self._tool_name, args_text)
        item = self._detail_layout.itemAt(idx)
        if item and item.widget():
            label_text = f"#{idx + 1}: {summary}" if summary else f"#{idx + 1}"
            item.widget().setText(label_text)

    def set_result_for_call(self, tool_call_id: str, result: str, is_error: bool) -> None:
        """Record a result for one call in the batch."""
        if is_error:
            self._errors[tool_call_id] = result
        else:
            self._results[tool_call_id] = result
        self._update_status()

    def _update_status(self) -> None:
        done = len(self._results) + len(self._errors)
        if done >= self._count:
            if self._errors:
                self._status_label.setText(f"\u2713{len(self._results)} \u2717{len(self._errors)}")
                self._status_label.setStyleSheet(
                    host_stylesheet(
                        "color: #f44747; font-size: 10px;",
                        f"color: #f44747; {_native_text_style(size=10, bold=True)}",
                    )
                )
            else:
                self._status_label.setText("\u2713")
                self._status_label.setStyleSheet(
                    host_stylesheet(
                        "color: #4ec9b0; font-size: 10px;",
                        f"color: #4ec9b0; {_native_text_style(size=10, bold=True)}",
                    )
                )
        else:
            self._status_label.setText(f"{done}/{self._count}")

    def _toggle(self) -> None:
        self.setUpdatesEnabled(False)
        self._expanded = not self._expanded
        self._detail_widget.setVisible(self._expanded)
        self._preview_label.setVisible(not self._expanded and bool(self._first_args))
        self._toggle_btn.setText("\u25bc" if self._expanded else "\u25b6")
        self.setUpdatesEnabled(True)

    @property
    def tool_name(self) -> str:
        return self._tool_name

    @property
    def count(self) -> int:
        return self._count


# ---------------------------------------------------------------------------
# Tool group widget - collapsible container for runs of tool calls
# ---------------------------------------------------------------------------


class ToolGroupWidget(QFrame):
    """Collapsible group for a consecutive run of tool calls."""

    def __init__(self, parent: QWidget = None):
        super().__init__(parent)
        self.setObjectName("message_tool")
        self.setStyleSheet(_tool_card_css(object_name="message_tool"))
        self._expanded = False
        self._count = 0
        self._done = 0
        self._errors = 0
        self._tool_names: list[str] = []

        layout = QVBoxLayout(self)
        layout.setContentsMargins(6, 3, 6, 3)
        layout.setSpacing(0)

        # Header: > <summary label> ok
        header_layout = QHBoxLayout()
        header_layout.setContentsMargins(0, 0, 0, 0)
        header_layout.setSpacing(6)

        self._toggle_btn = QToolButton()
        self._toggle_btn.setObjectName("collapse_button")
        self._toggle_btn.setText("\u25b6")
        self._toggle_btn.setFixedSize(14, 14)
        self._toggle_btn.clicked.connect(self._toggle)
        header_layout.addWidget(self._toggle_btn)

        self._label = QLabel("0 tools called")
        self._label.setStyleSheet(
            host_stylesheet(
                "color: #808080; font-size: 11px; font-weight: bold;",
                f"color: {_MUTED_TEXT}; {_native_text_style(size=11, bold=True)}",
            )
        )
        header_layout.addWidget(self._label, 1)

        self._status_label = QLabel("")
        self._status_label.setStyleSheet(
            host_stylesheet(
                "color: #dcdcaa; font-size: 10px;",
                f"color: #dcdcaa; {_native_text_style(size=10, bold=True)}",
            )
        )
        header_layout.addWidget(self._status_label)

        layout.addLayout(header_layout)

        # Container for child widgets (hidden by default)
        self._body = QWidget()
        self._body_layout = QVBoxLayout(self._body)
        self._body_layout.setContentsMargins(8, 2, 0, 2)
        self._body_layout.setSpacing(2)
        self._body.setVisible(False)
        layout.addWidget(self._body)

    def add_widget(self, widget: QWidget, tool_name: str = "") -> None:
        """Add a tool widget into this group."""
        self._count += 1
        self._tool_names.append(tool_name)
        self._body_layout.addWidget(widget)
        self._update_label()

    def notify_result(self, is_error: bool = False) -> None:
        """Called when a tool inside this group finishes."""
        self._done += 1
        if is_error:
            self._errors += 1
        self._update_status()

    def _update_label(self) -> None:
        self._label.setText(_format_tool_group_label(self._tool_names))

    def _update_status(self) -> None:
        if self._done >= self._count:
            if self._errors:
                ok = self._done - self._errors
                self._status_label.setText(f"\u2713{ok} \u2717{self._errors}")
                self._status_label.setStyleSheet(
                    host_stylesheet(
                        "color: #f44747; font-size: 10px;",
                        f"color: #f44747; {_native_text_style(size=10, bold=True)}",
                    )
                )
            else:
                self._status_label.setText("\u2713")
                self._status_label.setStyleSheet(
                    host_stylesheet(
                        "color: #4ec9b0; font-size: 10px;",
                        f"color: #4ec9b0; {_native_text_style(size=10, bold=True)}",
                    )
                )
        else:
            self._status_label.setText(f"{self._done}/{self._count}")
            self._status_label.setStyleSheet(
                host_stylesheet(
                    "color: #dcdcaa; font-size: 10px;",
                    f"color: #dcdcaa; {_native_text_style(size=10, bold=True)}",
                )
            )

    def _toggle(self) -> None:
        self.setUpdatesEnabled(False)
        self._expanded = not self._expanded
        self._body.setVisible(self._expanded)
        self._toggle_btn.setText("\u25bc" if self._expanded else "\u25b6")
        self.setUpdatesEnabled(True)

    @property
    def count(self) -> int:
        return self._count


# ---------------------------------------------------------------------------
# Python syntax highlighter (used by ToolApprovalWidget)
# ---------------------------------------------------------------------------


class _PythonHighlighter(QSyntaxHighlighter):
    """Minimal VS Code-dark-style Python syntax highlighter."""

    _RULES: ClassVar[list] = []  # built once in __init_subclass__ - see below

    def __init__(self, parent=None):
        super().__init__(parent)
        if not _PythonHighlighter._RULES:
            _PythonHighlighter._RULES = self._build_rules()

    @staticmethod
    def _fmt(color: str, bold: bool = False, italic: bool = False) -> QTextCharFormat:
        f = QTextCharFormat()
        f.setForeground(QColor(color))
        if bold:
            f.setFontWeight(QFont.Weight.Bold)
        if italic:
            f.setFontItalic(True)
        return f

    @staticmethod
    def _build_rules():
        rules = []
        kw_fmt = _PythonHighlighter._fmt("#c586c0", bold=True)
        for kw in (
            "and",
            "as",
            "assert",
            "async",
            "await",
            "break",
            "class",
            "continue",
            "def",
            "del",
            "elif",
            "else",
            "except",
            "finally",
            "for",
            "from",
            "global",
            "if",
            "import",
            "in",
            "is",
            "lambda",
            "nonlocal",
            "not",
            "or",
            "pass",
            "raise",
            "return",
            "try",
            "while",
            "with",
            "yield",
        ):
            rules.append((_re.compile(rf"\b{kw}\b"), kw_fmt))
        # Built-ins
        bi_fmt = _PythonHighlighter._fmt("#dcdcaa")
        for bi in (
            "print",
            "len",
            "range",
            "int",
            "str",
            "bytes",
            "list",
            "dict",
            "set",
            "tuple",
            "hex",
            "ord",
            "chr",
            "type",
            "isinstance",
            "enumerate",
            "zip",
            "map",
            "filter",
            "sorted",
            "open",
            "True",
            "False",
            "None",
        ):
            rules.append((_re.compile(rf"\b{bi}\b"), bi_fmt))
        # Numbers
        rules.append((_re.compile(r"\b0[xX][0-9a-fA-F]+\b"), _PythonHighlighter._fmt("#b5cea8")))
        rules.append((_re.compile(r"\b\d+\.?\d*\b"), _PythonHighlighter._fmt("#b5cea8")))
        # Strings (single/double, including f/r/b prefixes)
        str_fmt = _PythonHighlighter._fmt("#ce9178")
        rules.append((_re.compile(r'[brfu]?""".*?"""', _re.DOTALL), str_fmt))
        rules.append((_re.compile(r"[brfu]?'''.*?'''", _re.DOTALL), str_fmt))
        rules.append((_re.compile(r'[brfu]?"[^"\n]*"'), str_fmt))
        rules.append((_re.compile(r"[brfu]?'[^'\n]*'"), str_fmt))
        # Comments
        rules.append((_re.compile(r"#[^\n]*"), _PythonHighlighter._fmt("#6a9955", italic=True)))
        # Decorators
        rules.append((_re.compile(r"@\w+"), _PythonHighlighter._fmt("#dcdcaa")))
        # self
        rules.append((_re.compile(r"\bself\b"), _PythonHighlighter._fmt("#9cdcfe", italic=True)))
        return rules

    def highlightBlock(self, text: str) -> None:
        for pattern, fmt in _PythonHighlighter._RULES:
            for m in pattern.finditer(text):
                self.setFormat(m.start(), m.end() - m.start(), fmt)


class ToolApprovalWidget(QFrame):
    """Displays a tool approval request with syntax-highlighted code preview."""

    def __init__(
        self,
        tool_call_id: str,
        tool_name: str,
        args_text: str,
        description: str,
        parent: QWidget = None,
    ):
        super().__init__(parent)
        self._approved_callback = None
        self.setObjectName("message_question")
        self.setStyleSheet(
            host_stylesheet(
                "QFrame#message_question { border: 1px solid #dcdcaa; border-radius: 6px; background: #2d2d1e; }",
                _tool_card_css(
                    parent or self,
                    accent="#dcdcaa",
                    background="#2d2d1e",
                    object_name="message_question",
                ),
            )
        )
        self._tool_call_id = tool_call_id

        layout = QVBoxLayout(self)
        layout.setContentsMargins(8, 6, 8, 6)

        self._header = QLabel("Approve execute_python?")
        self._header.setStyleSheet(
            host_stylesheet(
                "color: #dcdcaa; font-weight: bold; font-size: 11px;",
                f"color: #dcdcaa; {_native_text_style(size=11, bold=True)}",
            )
        )
        layout.addWidget(self._header)

        code = self._extract_code(args_text)
        code_lines = code.strip().splitlines() if code.strip() else []

        self._info = QLabel(f"Python code - {len(code_lines)} line{'s' if len(code_lines) != 1 else ''}")
        self._info.setStyleSheet(
            host_stylesheet(
                "color: #808080; font-size: 10px;",
                f"color: {_MUTED_TEXT}; {_native_text_style(size=10)}",
            )
        )
        layout.addWidget(self._info)

        editor = self._build_code_editor(code, code_lines)
        if editor is not None:
            layout.addWidget(editor)

        layout.addLayout(self._build_approval_buttons())

    def set_approved_callback(self, callback) -> None:
        self._approved_callback = callback

    @staticmethod
    def _extract_code(args_text: str) -> str:
        """Extract the code/script value from JSON args, falling back to raw text."""
        try:
            args = json.loads(args_text) if args_text.strip() else {}
            return args.get("code", args.get("script", ""))
        except (json.JSONDecodeError, TypeError, AttributeError):
            return args_text

    def _build_code_editor(self, code: str, lines: list) -> QPlainTextEdit | None:
        """Build a read-only syntax-highlighted code editor, or None if no code."""
        if not code.strip():
            return None
        self._code_edit = QPlainTextEdit()
        self._code_edit.setReadOnly(True)
        self._code_edit.setPlainText(code)
        self._code_edit.setStyleSheet(
            host_stylesheet(
                "QPlainTextEdit { "
                "  color: #d4d4d4; background: #1e1e2e; "
                "  font-family: 'Consolas', 'Monaco', 'Courier New', monospace; "
                "  font-size: 11px; border: 1px solid #3c3c3c; border-radius: 4px; "
                "  padding: 4px; "
                "}"
                "QScrollBar:vertical { width: 8px; background: #1e1e2e; }"
                "QScrollBar::handle:vertical { background: #3c3c3c; border-radius: 4px; }"
                "QScrollBar:horizontal { height: 8px; background: #1e1e2e; }"
                "QScrollBar::handle:horizontal { background: #3c3c3c; border-radius: 4px; }"
            )
        )
        self._code_edit.setLineWrapMode(QPlainTextEdit.LineWrapMode.NoWrap)
        visible_lines = min(len(lines), 15)
        line_height = self._code_edit.fontMetrics().lineSpacing()
        self._code_edit.setFixedHeight(line_height * visible_lines + 16)
        if not use_native_host_theme():
            self._highlighter = _PythonHighlighter(self._code_edit.document())
        return self._code_edit

    def _build_approval_buttons(self) -> QHBoxLayout:
        """Build the Allow / Always Allow / Deny button row."""
        btn_layout = QHBoxLayout()
        btn_layout.setSpacing(8)

        self._allow_btn = QToolButton()
        self._allow_btn.setText("  Allow  ")
        allow_css = (
            "QToolButton { background: #2ea043; color: #ffffff; border: none; "
            "border-radius: 4px; padding: 4px 16px; font-size: 11px; font-weight: bold; }"
            "QToolButton:hover { background: #3fb950; }"
        )
        self._allow_btn.setStyleSheet(host_stylesheet(allow_css, allow_css))
        self._allow_btn.clicked.connect(self._on_allow)
        btn_layout.addWidget(self._allow_btn)

        self._always_btn = QToolButton()
        self._always_btn.setText("  Always Allow  ")
        always_css = (
            "QToolButton { background: #1a5c2d; color: #ffffff; border: none; "
            "border-radius: 4px; padding: 4px 16px; font-size: 11px; font-weight: bold; }"
            "QToolButton:hover { background: #2ea043; }"
        )
        self._always_btn.setStyleSheet(host_stylesheet(always_css, always_css))
        self._always_btn.clicked.connect(self._on_always_allow)
        btn_layout.addWidget(self._always_btn)

        self._deny_btn = QToolButton()
        self._deny_btn.setText("  Deny  ")
        deny_css = (
            "QToolButton { background: #c42b1c; color: #ffffff; border: none; "
            "border-radius: 4px; padding: 4px 16px; font-size: 11px; font-weight: bold; }"
            "QToolButton:hover { background: #e04030; }"
        )
        self._deny_btn.setStyleSheet(host_stylesheet(deny_css, deny_css))
        self._deny_btn.clicked.connect(self._on_deny)
        btn_layout.addWidget(self._deny_btn)

        btn_layout.addStretch()
        return btn_layout

    def _disable_buttons(self):
        self._allow_btn.setEnabled(False)
        self._always_btn.setEnabled(False)
        self._deny_btn.setEnabled(False)

    def _on_allow(self):
        self._disable_buttons()
        self._allow_btn.setText("  Allowed  ")
        allowed_css = (
            "QToolButton { background: #1a5c2d; color: #808080; border: none; "
            "border-radius: 4px; padding: 4px 16px; font-size: 11px; }"
        )
        self._allow_btn.setStyleSheet(host_stylesheet(allowed_css, allowed_css))
        if self._approved_callback is not None:
            self._approved_callback(self._tool_call_id, "allow")

    def _on_always_allow(self):
        self._disable_buttons()
        self._always_btn.setText("  Always Allowed  ")
        always_allowed_css = (
            "QToolButton { background: #1a5c2d; color: #808080; border: none; "
            "border-radius: 4px; padding: 4px 16px; font-size: 11px; }"
        )
        self._always_btn.setStyleSheet(host_stylesheet(always_allowed_css, always_allowed_css))
        if self._approved_callback is not None:
            self._approved_callback(self._tool_call_id, "allow_all")

    def _on_deny(self):
        self._disable_buttons()
        self._deny_btn.setText("  Denied  ")
        denied_css = (
            "QToolButton { background: #6e1a12; color: #808080; border: none; "
            "border-radius: 4px; padding: 4px 16px; font-size: 11px; }"
        )
        self._deny_btn.setStyleSheet(host_stylesheet(denied_css, denied_css))
        if self._approved_callback is not None:
            self._approved_callback(self._tool_call_id, "deny")

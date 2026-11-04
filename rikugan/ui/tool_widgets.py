"""Tool call, batch, group, and approval widgets."""

from __future__ import annotations

import json
import re as _re

from ..core.sanitize import unwrap_tool_result
from .code_view import LANGUAGE_JSON, CodeView
from .qt_compat import (
    QFrame,
    QHBoxLayout,
    QLabel,
    Qt,
    QTimer,
    QToolButton,
    QVBoxLayout,
    QWidget,
    qt_flags,
)
from .styles import (
    blend_theme_color,
    ensure_contrast,
    get_chat_color_tokens,
    host_stylesheet,
)
from .tool_review import ToolReview, ToolReviewView, build_review, result_language

_CODE_RESULT_TOOLS = frozenset({"read_file", "edit_file", "search_files", "run_shell", "execute_python"})
_MAX_ARGS_DISPLAY = 2000
_MAX_RESULT_DISPLAY = 3000
_TOOL_PREVIEW_LINES = 3
# Fallbacks only: the live host palette is used whenever one is available, so a
# tool row is not a black bar in IDA's light theme.
_MUTED_TEXT = "#a8a8a8"
_TOOL_BG = "#252526"
_TOOL_BORDER = "#3c3c3c"


def _muted_text(source=None) -> str:
    """Secondary text color for the tool card, from the host palette."""
    try:
        return get_chat_color_tokens(source)["muted"]
    except Exception:
        return _MUTED_TEXT


# Status hues tuned for a dark card; on a light host theme they are walked
# darker until they read on the card, keeping their identity.
_TOOL_INK_BASE = {"running": "#dcdcaa", "ok": "#4ec9b0", "error": "#f44747"}


def _tool_ink(role: str, source=None) -> str:
    """Text color for a tool card, resolved against the card's own fill.

    The IDA arm used to hard-code dark-theme greys, so on IDA's light theme a
    group header was pale grey on pale grey.
    """
    try:
        colors = get_chat_color_tokens(source)
    except Exception:
        return _TOOL_INK_BASE.get(role, _MUTED_TEXT)
    card = colors["tool_bg"]
    if role == "text":
        return ensure_contrast(colors["text"], card, 7.0)
    if role == "muted":
        return ensure_contrast(colors["muted"], card, 4.5)
    return ensure_contrast(_TOOL_INK_BASE[role], card, 4.5)


def _toggle_css(source=None) -> str:
    """Expand arrow color: IDA's own tool-button ink is too faint on our cards."""
    return host_stylesheet(
        "",
        "QToolButton#collapse_button { border: none; background: transparent; padding: 0px; "
        f"color: {_tool_ink('muted', source)}; }}"
        f"QToolButton#collapse_button:hover {{ color: {_tool_ink('text', source)}; }}",
    )


def _tool_card_css(
    source=None,
    *,
    accent: str | None = None,
    background: str | None = None,
    radius: int = 6,
    object_name: str = "message_tool",
) -> str:
    try:
        colors = get_chat_color_tokens(source)
        default_border, default_bg = colors["border"], colors["tool_bg"]
    except Exception:
        default_border, default_bg = _TOOL_BORDER, _TOOL_BG
    border = accent or default_border
    bg = background or default_bg
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
_MUTATING_TOOLS = frozenset(
    (
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
    )
)
for _t in _MUTATING_TOOLS:
    _TOOL_COLORS[_t] = "#c586c0"  # magenta/purple

# Files and the shell act on the machine -> same family as other changes
for _t in ("write_file", "edit_file", "run_shell"):
    _TOOL_COLORS[_t] = "#c586c0"
for _t in ("read_file", "search_files"):
    _TOOL_COLORS[_t] = "#4ec9b0"

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


_PAGE_TOTAL_RE = _re.compile(r"^([A-Za-z][A-Za-z ]*?)\s+\d+[-\u2013]\d+\s+of\s+(\d+)\s*:")
_PAGE_NOUNS = {
    "functions": "fns",
    "xrefs": "refs",
    "cross references": "refs",
    "imports": "imports",
    "exports": "exports",
    "strings": "strings",
    "segments": "segs",
    "sections": "sects",
}
_LINE_COUNT_TOOLS = frozenset(
    (
        "decompile_function",
        "read_disassembly",
        "read_function_disassembly",
        "fetch_disassembly",
        "get_microcode",
        "get_il",
        "get_il_block",
        "hexdump_address",
    )
)
_CHECK = "\u2713"
_CROSS = "\u2717"


def _file_tool_chip(tool_name: str, text: str) -> str:
    """The shape of a file tool's result. A failed command must not read as a tick."""
    if tool_name == "run_shell":
        exit_code = _re.match(r"Exit code (-?\d+)", text)
        if exit_code:
            return _CHECK if exit_code.group(1) == "0" else f"exit {exit_code.group(1)}"
        return "stopped" if text.startswith("The command ") else ""
    if tool_name == "read_file":
        title = _re.match(r"=== .* \(([\d,]+) lines\) ===", text)
        return f"{int(title.group(1).replace(',', '')):,} lines" if title else ""
    if tool_name == "search_files":
        if "No matches." in text[:300]:
            return "0 rows"
        rows = _re.search(r"Rows \d+-\d+ of (\d+):", text[:600])
        return f"{int(rows.group(1)):,} rows" if rows else ""
    return ""


def _chip_means_failure(tool_name: str, chip: str) -> bool:
    return tool_name == "run_shell" and chip != _CHECK


def _format_result_chip(tool_name: str, result: str, is_error: bool = False) -> str:
    """Summarize a tool result as a short status chip for the call row.

    Keeps the row one line tall: the shape of the result ("3 refs",
    "148 lines") is what the reader needs; the payload stays behind the
    expander.
    """
    if is_error:
        return _CROSS

    short_name = _strip_mcp_prefix(tool_name)
    text = result or ""

    file_chip = _file_tool_chip(short_name, text)
    if file_chip:
        return file_chip

    page = _PAGE_TOTAL_RE.match(text.lstrip())
    if page:
        title = page.group(1).strip().lower()
        try:
            total = int(page.group(2))
        except ValueError:
            total = 0
        noun = _PAGE_NOUNS.get(title, title)
        return f"{total:,} {noun}"

    if short_name in _MUTATING_TOOLS:
        return f"{_CHECK} undoable"

    if short_name in _LINE_COUNT_TOOLS and text.strip():
        lines = text.count("\n") + 1
        return f"{lines:,} lines"

    return _CHECK


def _tool_color(name: str, source=None) -> str:
    """Category color for a tool, darkened for light host themes.

    The palette is chosen for a dark background. On IDA's light default the
    same teal or gold washes out against the card, so it is blended toward the
    text color — keeping the category legible without changing its identity.
    """
    base = _TOOL_COLORS.get(_strip_mcp_prefix(name), _DEFAULT_TOOL_COLOR)
    try:
        colors = get_chat_color_tokens(source)
    except Exception:
        return base
    if _is_light_surface(colors["tool_bg"]):
        return blend_theme_color(base, "#000000", 0.45)
    return base


def _is_light_surface(color: str) -> bool:
    value = color.lstrip("#")
    if len(value) != 6:
        return False
    r, g, b = (int(value[i : i + 2], 16) / 255.0 for i in (0, 2, 4))
    return (0.2126 * r + 0.7152 * g + 0.0722 * b) >= 0.5


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

    elif short_name in ("read_file", "write_file", "edit_file"):
        summary = _get("file_path")

    elif short_name == "run_shell":
        first_line = _get("command").strip().split("\n")[0]
        summary = first_line

    elif short_name == "search_files":
        pattern = _get("pattern")
        summary = f'"{pattern}"' if pattern else _get("glob", "path")

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
    lbl = QLabel()
    lbl.setObjectName("tool_content")
    lbl.setWordWrap(True)
    lbl.setStyleSheet(
        host_stylesheet(
            "color: #9a9a9a; font-family: monospace; font-size: 10px; margin-left: 28px;",
            f"color: {_tool_ink('muted', source)}; {_native_text_style(size=10, monospace=True)}",
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
        self._result_done = False
        self._review: ToolReview | None = None
        self._review_view: ToolReviewView | None = None
        self._code_result = ""
        self._result_view: CodeView | None = None

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
        color = _tool_color(tool_name, self)

        header_layout = QHBoxLayout()
        header_layout.setContentsMargins(0, 0, 0, 0)
        header_layout.setSpacing(0)

        self._toggle_btn = QToolButton()
        self._toggle_btn.setObjectName("collapse_button")
        self._toggle_btn.setText("\u25b6")
        self._toggle_btn.setFixedSize(14, 14)
        self._toggle_btn.setStyleSheet(_toggle_css(self))
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
                f"color: {_muted_text(self)}; font-size: 11px; margin-left: 6px;",
                f"color: {_muted_text(self)}; {_native_text_style(size=11)}",
            )
        )
        header_layout.addWidget(self._summary_label, 1)

        self._status_label = QLabel(self._SPINNER_FRAMES[0])
        self._status_label.setStyleSheet(
            host_stylesheet(
                "color: #dcdcaa; font-size: 10px;",
                f"color: {_tool_ink('running', self)}; {_native_text_style(size=10, bold=True)}",
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
                f"color: {_muted_text(self)}; font-size: 10px; font-weight: bold;",
                f"color: {_muted_text(self)}; {_native_text_style(size=10, bold=True)}",
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
        if self._expanded:
            self._render_details()
        self._detail_widget.setVisible(self._expanded)
        has_preview = bool(self._preview_label.text())
        self._preview_label.setVisible(not self._expanded and has_preview)
        self._toggle_btn.setText("\u25bc" if self._expanded else "\u25b6")
        self.setUpdatesEnabled(True)

    def _render_details(self) -> None:
        """Build the heavy views when first shown. Most cards are never opened."""
        if self._review is not None and self._review_view is None:
            self._args_label.setVisible(False)
            self._review_view = ToolReviewView(self._review, parent=self._detail_widget)
            self._detail_layout.insertWidget(0, self._review_view)
        if self._code_result and self._result_view is None:
            self._result_label.setVisible(False)
            language = result_language(self._tool_name, self._args_text)
            self._result_view = CodeView(self._code_result, language, line_numbers=False, parent=self._detail_widget)
            self._detail_layout.addWidget(self._result_view)

    def set_arguments(self, args_text: str) -> None:
        self._args_text = args_text
        self._review = build_review(self._tool_name, args_text)
        if self._expanded:
            self._render_details()
        # Update summary
        summary = _format_tool_summary(self._tool_name, args_text)
        if summary:
            self._summary_label.setText(summary)
        # Preview (truncated) — only for tools whose args have no one-line
        # summary, otherwise the row repeats itself over three lines.
        if args_text.strip() and not summary:
            self._preview_label.setText(_truncate_preview(args_text.strip()))
            self._preview_label.setVisible(not self._expanded)
        else:
            self._preview_label.setVisible(False)
        # Full args in detail area
        display = args_text[:_MAX_ARGS_DISPLAY] + "..." if len(args_text) > _MAX_ARGS_DISPLAY else args_text
        self._args_label.setText(display)

    def append_args_delta(self, delta: str) -> None:
        self._args_text += delta
        # Don't update preview during streaming - wait for set_arguments

    def set_result(self, result: str, is_error: bool = False) -> None:
        self._stop_spinner()
        result = unwrap_tool_result(result)
        self._result_text = result
        self._is_error = is_error
        if not is_error and result and self._tool_name in _CODE_RESULT_TOOLS:
            self._code_result = result
            if self._expanded:
                self._render_details()
        display = result[:_MAX_RESULT_DISPLAY] + "\n... (truncated)" if len(result) > _MAX_RESULT_DISPLAY else result
        self._result_label.setText(display)
        self._result_label.setVisible(True)
        self._result_header.setVisible(True)
        if is_error:
            self._result_label.setStyleSheet(
                host_stylesheet(
                    "color: #f44747; font-family: monospace; font-size: 11px;",
                    f"color: {_tool_ink('error', self)}; {_native_text_style(size=11, monospace=True)}",
                )
            )
            self._status_label.setText(_format_result_chip(self._tool_name, result, True))
            self._status_label.setStyleSheet(
                host_stylesheet(
                    "color: #f44747; font-size: 10px;",
                    f"color: {_tool_ink('error', self)}; {_native_text_style(size=10, bold=True)}",
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
            self._render_details()
            self._detail_widget.setVisible(True)
            self._preview_label.setVisible(False)
            self._toggle_btn.setText("\u25bc")
        else:
            chip = _format_result_chip(self._tool_name, result, False)
            self._status_label.setText(chip)
            # A command can fail without the tool failing; it must not look fine.
            failed = _chip_means_failure(self._tool_name, chip)
            self._status_label.setStyleSheet(
                host_stylesheet(
                    f"color: {'#f44747' if failed else '#4ec9b0'}; font-size: 10px;",
                    f"color: {_tool_ink('error' if failed else 'ok', self)}; {_native_text_style(size=10, bold=True)}",
                )
            )
        self._result_done = True

    def mark_done(self) -> None:
        self._stop_spinner()
        if not self._result_done:
            self._result_done = True
            self._status_label.setText(_CHECK)
            self._status_label.setStyleSheet(
                host_stylesheet(
                    "color: #4ec9b0; font-size: 10px;",
                    f"color: {_tool_ink('ok', self)}; {_native_text_style(size=10, bold=True)}",
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
        color = _tool_color(tool_name, self)

        header_layout = QHBoxLayout()
        header_layout.setContentsMargins(0, 0, 0, 0)
        header_layout.setSpacing(0)

        self._toggle_btn = QToolButton()
        self._toggle_btn.setObjectName("collapse_button")
        self._toggle_btn.setText("\u25b6")
        self._toggle_btn.setFixedSize(14, 14)
        self._toggle_btn.setStyleSheet(_toggle_css(self))
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
                f"color: {_muted_text(self)}; font-size: 11px; margin-left: 6px;",
                f"color: {_muted_text(self)}; {_native_text_style(size=11)}",
            )
        )
        header_layout.addWidget(self._count_label, 1)

        self._status_label = QLabel("\u2026")
        self._status_label.setStyleSheet(
            host_stylesheet(
                "color: #dcdcaa; font-size: 10px;",
                f"color: {_tool_ink('running', self)}; {_native_text_style(size=10, bold=True)}",
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
                "color: #a8a8a8; font-family: monospace; font-size: 10px;",
                f"color: {_tool_ink('muted', self)}; {_native_text_style(size=10, monospace=True)}",
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
                        f"color: {_tool_ink('error', self)}; {_native_text_style(size=10, bold=True)}",
                    )
                )
            else:
                self._status_label.setText("\u2713")
                self._status_label.setStyleSheet(
                    host_stylesheet(
                        "color: #4ec9b0; font-size: 10px;",
                        f"color: {_tool_ink('ok', self)}; {_native_text_style(size=10, bold=True)}",
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
        self._spin_idx = 0
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
        self._toggle_btn.setStyleSheet(_toggle_css(self))
        self._toggle_btn.clicked.connect(self._toggle)
        header_layout.addWidget(self._toggle_btn)

        self._label = QLabel("0 tools called")
        self._label.setStyleSheet(
            host_stylesheet(
                "color: #c8c8c8; font-size: 11px; font-weight: bold;",
                f"color: {_tool_ink('text', self)}; {_native_text_style(size=11, bold=True)}",
            )
        )
        header_layout.addWidget(self._label, 1)

        self._status_label = QLabel("")
        self._status_label.setStyleSheet(
            host_stylesheet(
                "color: #dcdcaa; font-size: 10px;",
                f"color: {_tool_ink('running', self)}; {_native_text_style(size=10, bold=True)}",
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
        # The group is collapsed by default, which hides every child's spinner.
        # Without its own the panel showed no sign of life for the whole
        # multi-tool phase — often the longest part of a turn.
        _SharedSpinnerTimer.get().register(self)
        self._update_status()

    def notify_result(self, is_error: bool = False) -> None:
        """Called when a tool inside this group finishes."""
        self._done += 1
        if is_error:
            self._errors += 1
        if self._done >= self._count:
            _SharedSpinnerTimer.get().unregister(self)
        self._update_status()

    def _spin_tick(self) -> None:
        """Animate the header while calls are still in flight."""
        if self._done >= self._count:
            return
        self._spin_idx = (self._spin_idx + 1) % len(ToolCallWidget._SPINNER_FRAMES)
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
                        f"color: {_tool_ink('error', self)}; {_native_text_style(size=10, bold=True)}",
                    )
                )
            else:
                self._status_label.setText("\u2713")
                self._status_label.setStyleSheet(
                    host_stylesheet(
                        "color: #4ec9b0; font-size: 10px;",
                        f"color: {_tool_ink('ok', self)}; {_native_text_style(size=10, bold=True)}",
                    )
                )
        else:
            frame = ToolCallWidget._SPINNER_FRAMES[self._spin_idx]
            self._status_label.setText(f"{frame} {self._done}/{self._count}")
            self._status_label.setStyleSheet(
                host_stylesheet(
                    "color: #dcdcaa; font-size: 10px;",
                    f"color: {_tool_ink('running', self)}; {_native_text_style(size=10, bold=True)}",
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


def _approval_card_text(tool_name: str, args_text: str, description: str) -> tuple[str, str, str]:
    """Return the (title, info line, preview) an approval card shows.

    A script is previewed as code. Anything else shows its description with the
    raw arguments beneath, because the arguments are what actually runs and a
    description only ever summarizes them.
    """
    title = f"Approve {tool_name}?"
    code = ToolApprovalWidget._extract_code(args_text)
    if code.strip():
        count = len(code.strip().splitlines())
        return title, f"Python code - {count} line{'s' if count != 1 else ''}", code
    return title, description, "" if args_text.strip() in ("", "{}") else args_text


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

        title, info, preview = _approval_card_text(tool_name, args_text, description)

        self._header = QLabel(title)
        self._header.setStyleSheet(
            host_stylesheet(
                "color: #dcdcaa; font-weight: bold; font-size: 11px;",
                f"color: {_tool_ink('running', self)}; {_native_text_style(size=11, bold=True)}",
            )
        )
        layout.addWidget(self._header)

        self._info = QLabel(info)
        # The description quotes arguments, which can carry text from the binary.
        # Rendered as rich text, that could restyle what the user is approving.
        self._info.setTextFormat(Qt.TextFormat.PlainText)
        self._info.setWordWrap(True)
        self._info.setStyleSheet(
            host_stylesheet(
                "color: #a8a8a8; font-size: 10px;",
                f"color: {_tool_ink('muted', self)}; {_native_text_style(size=10)}",
            )
        )
        layout.addWidget(self._info)

        review = build_review(tool_name, args_text)
        if review is not None:
            # The card's fill differs by host, so its labels can't assume the tool card's.
            ink = host_stylesheet("#d4d4d4", _tool_ink("text", self))
            muted = host_stylesheet("#a8a8a8", _tool_ink("muted", self))
            layout.addWidget(ToolReviewView(review, parent=self, ink=ink, muted=muted))
        elif preview.strip():
            layout.addWidget(CodeView(preview, LANGUAGE_JSON, line_numbers=False, parent=self))

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
            "QToolButton { background: #1a5c2d; color: #d4d4d4; border: none; "
            "border-radius: 4px; padding: 4px 16px; font-size: 11px; }"
        )
        self._allow_btn.setStyleSheet(host_stylesheet(allowed_css, allowed_css))
        if self._approved_callback is not None:
            self._approved_callback(self._tool_call_id, "allow")

    def _on_always_allow(self):
        self._disable_buttons()
        self._always_btn.setText("  Always Allowed  ")
        always_allowed_css = (
            "QToolButton { background: #1a5c2d; color: #d4d4d4; border: none; "
            "border-radius: 4px; padding: 4px 16px; font-size: 11px; }"
        )
        self._always_btn.setStyleSheet(host_stylesheet(always_allowed_css, always_allowed_css))
        if self._approved_callback is not None:
            self._approved_callback(self._tool_call_id, "allow_all")

    def _on_deny(self):
        self._disable_buttons()
        self._deny_btn.setText("  Denied  ")
        denied_css = (
            "QToolButton { background: #6e1a12; color: #d4d4d4; border: none; "
            "border-radius: 4px; padding: 4px 16px; font-size: 11px; }"
        )
        self._deny_btn.setStyleSheet(host_stylesheet(denied_css, denied_css))
        if self._approved_callback is not None:
            self._approved_callback(self._tool_call_id, "deny")

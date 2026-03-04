"""Message display widgets for the chat view."""

from __future__ import annotations

import random

import re as _re

from .qt_compat import (
    QFrame, QVBoxLayout, QHBoxLayout, QLabel, QToolButton, QPushButton,
    QWidget, Qt, Signal, QTimer,
)
from .markdown import md_to_html

_THINKING_PHRASES = [
    "analyzing binary structure...",
    "examining control flow...",
    "tracing cross-references...",
    "inspecting disassembly...",
    "reading function signatures...",
    "correlating data references...",
    "mapping call graph...",
    "evaluating type patterns...",
    "scanning string references...",
    "deobfuscating logic...",
    "checking import table...",
    "inferring variable types...",
    "analyzing stack layout...",
    "tracing data flow...",
    "examining vtable references...",
    "decoding encoded values...",
]

# Re-export tool widgets so existing consumers that import from this module
# continue to work without changes.
from .tool_widgets import (  # noqa: E402
    _SharedSpinnerTimer, ToolCallWidget, ToolBatchWidget,
    ToolGroupWidget, ToolApprovalWidget,
)


# ---------------------------------------------------------------------------
# Collapsible section (unchanged, used internally)
# ---------------------------------------------------------------------------

class CollapsibleSection(QFrame):
    """A widget with a clickable header that shows/hides content."""

    def __init__(self, title: str, parent: QWidget = None):
        super().__init__(parent)
        self._expanded = False

        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(2)

        # Header
        header = QHBoxLayout()
        self._toggle_btn = QToolButton()
        self._toggle_btn.setObjectName("collapse_button")
        self._toggle_btn.setText("▶")
        self._toggle_btn.setFixedSize(16, 16)
        self._toggle_btn.clicked.connect(self.toggle)

        self._title_label = QLabel(title)
        self._title_label.setObjectName("tool_header")
        header.addWidget(self._toggle_btn)
        header.addWidget(self._title_label, 1)
        layout.addLayout(header)

        # Content area
        self._content = QWidget()
        self._content_layout = QVBoxLayout(self._content)
        self._content_layout.setContentsMargins(20, 0, 0, 0)
        self._content.setVisible(False)
        layout.addWidget(self._content)

    def toggle(self) -> None:
        self._expanded = not self._expanded
        self._content.setVisible(self._expanded)
        self._toggle_btn.setText("▼" if self._expanded else "▶")

    def set_expanded(self, expanded: bool) -> None:
        self._expanded = expanded
        self._content.setVisible(expanded)
        self._toggle_btn.setText("▼" if expanded else "▶")

    def content_layout(self) -> QVBoxLayout:
        return self._content_layout


# ---------------------------------------------------------------------------
# User message
# ---------------------------------------------------------------------------

class UserMessageWidget(QFrame):
    """Displays a user message."""

    def __init__(self, text: str, parent: QWidget = None):
        super().__init__(parent)
        self.setObjectName("message_user")

        layout = QVBoxLayout(self)
        layout.setContentsMargins(8, 6, 8, 6)

        role_label = QLabel("You")
        role_label.setStyleSheet("color: #4ec9b0; font-weight: bold; font-size: 11px;")
        layout.addWidget(role_label)

        content = QLabel(text)
        content.setWordWrap(True)
        content.setTextInteractionFlags(Qt.TextInteractionFlag.TextSelectableByMouse | Qt.TextInteractionFlag.TextSelectableByKeyboard)
        content.setStyleSheet("color: #d4d4d4; font-size: 13px;")
        layout.addWidget(content)


# ---------------------------------------------------------------------------
# Thinking content parser
# ---------------------------------------------------------------------------

_THINK_RE = _re.compile(r'<think>(.*?)</think>', _re.DOTALL)


def _split_thinking(text: str):
    """Split text into (thinking_content, visible_content).

    Handles:
    - One or more complete ``<think>...</think>`` blocks
    - An unclosed ``<think>`` during streaming
    """
    thinking_parts: list = []

    # Extract all complete <think>...</think> blocks
    last_end = 0
    visible_parts: list = []
    for m in _THINK_RE.finditer(text):
        visible_parts.append(text[last_end:m.start()])
        thinking_parts.append(m.group(1).strip())
        last_end = m.end()
    visible_parts.append(text[last_end:])
    remaining = "".join(visible_parts)

    # Check for unclosed <think> (still streaming)
    open_idx = remaining.rfind("<think>")
    if open_idx >= 0:
        partial = remaining[open_idx + 7:].strip()
        if partial:
            thinking_parts.append(partial)
        remaining = remaining[:open_idx]

    return "\n\n".join(thinking_parts), remaining.strip()


# ---------------------------------------------------------------------------
# Collapsible thinking block
# ---------------------------------------------------------------------------

class _ThinkingBlock(QFrame):
    """Collapsible block for model reasoning / chain-of-thought."""

    def __init__(self, parent: QWidget = None):
        super().__init__(parent)
        self.setObjectName("thinking_block")
        self.setStyleSheet(
            "#thinking_block { background: #1a1a2e; border: 1px solid #2a2a3e; "
            "border-radius: 6px; }"
        )

        layout = QVBoxLayout(self)
        layout.setContentsMargins(8, 4, 8, 4)
        layout.setSpacing(0)

        self._toggle = QToolButton()
        self._toggle.setObjectName("collapse_button")
        self._toggle.setText("\u25b6")  # ▶
        self._toggle.setFixedSize(14, 14)
        self._toggle.clicked.connect(self._on_toggle)

        header = QHBoxLayout()
        header.setContentsMargins(0, 0, 0, 0)
        header.setSpacing(4)
        header.addWidget(self._toggle)
        self._header_label = QLabel("Thinking")
        self._header_label.setStyleSheet(
            "color: #707090; font-size: 11px; font-style: italic;"
        )
        header.addWidget(self._header_label, 1)
        layout.addLayout(header)

        self._content = QLabel()
        self._content.setWordWrap(True)
        self._content.setTextFormat(Qt.TextFormat.RichText)
        self._content.setTextInteractionFlags(
            Qt.TextInteractionFlag.TextSelectableByMouse
            | Qt.TextInteractionFlag.TextSelectableByKeyboard
        )
        self._content.setStyleSheet("color: #606078; font-size: 12px;")
        self._content.hide()
        layout.addWidget(self._content)

        self._expanded = False
        self.hide()

    def _on_toggle(self) -> None:
        self._expanded = not self._expanded
        self._content.setVisible(self._expanded)
        self._toggle.setText("\u25bc" if self._expanded else "\u25b6")

    def set_thinking(self, text: str, in_progress: bool = False) -> None:
        self._content.setText(md_to_html(text))
        label = "Thinking\u2026" if in_progress else "Thinking"
        self._header_label.setText(label)
        self.show()


# ---------------------------------------------------------------------------
# Assistant message (with streaming + Markdown)
# ---------------------------------------------------------------------------

class AssistantMessageWidget(QFrame):
    """Displays an assistant message with streaming support and Markdown rendering."""

    _RENDER_BATCH = 40

    def __init__(self, parent: QWidget = None):
        super().__init__(parent)
        self.setObjectName("message_assistant")
        self._full_text = ""
        self._pending_delta = 0

        layout = QVBoxLayout(self)
        layout.setContentsMargins(8, 6, 8, 6)

        role_label = QLabel("Rikugan")
        role_label.setStyleSheet("color: #569cd6; font-weight: bold; font-size: 11px;")
        layout.addWidget(role_label)

        self._thinking_block = _ThinkingBlock()
        layout.addWidget(self._thinking_block)

        self._content = QLabel()
        self._content.setWordWrap(True)
        self._content.setTextFormat(Qt.TextFormat.RichText)
        self._content.setTextInteractionFlags(
            Qt.TextInteractionFlag.TextSelectableByMouse
            | Qt.TextInteractionFlag.TextSelectableByKeyboard
            | Qt.TextInteractionFlag.LinksAccessibleByMouse
        )
        self._content.setOpenExternalLinks(True)
        self._content.setStyleSheet("color: #d4d4d4; font-size: 13px;")
        layout.addWidget(self._content)

    def _render(self) -> None:
        thinking, visible = _split_thinking(self._full_text)
        if thinking:
            in_progress = "<think>" in self._full_text and "</think>" not in self._full_text
            self._thinking_block.set_thinking(thinking, in_progress=in_progress)
        else:
            self._thinking_block.hide()
        self._content.setText(md_to_html(visible))
        self._pending_delta = 0

    def append_text(self, delta: str) -> None:
        self._full_text += delta
        self._pending_delta += len(delta)
        if self._pending_delta >= self._RENDER_BATCH:
            self._render()

    def set_text(self, text: str) -> None:
        self._full_text = text
        self._render()

    def full_text(self) -> str:
        return self._full_text


# ---------------------------------------------------------------------------
# Thinking indicator
# ---------------------------------------------------------------------------

class ThinkingWidget(QFrame):
    """Animated thinking indicator shown while the LLM is processing."""

    _STAR_FRAMES = ["✳", "✴", "✵", "✶"]

    def __init__(self, parent: QWidget = None):
        super().__init__(parent)
        self.setObjectName("message_thinking")
        self._phrase_idx = random.randint(0, len(_THINKING_PHRASES) - 1)
        self._star_idx = 0

        layout = QHBoxLayout(self)
        layout.setContentsMargins(8, 4, 8, 4)
        layout.setSpacing(6)

        self._star_label = QLabel(self._STAR_FRAMES[0])
        self._star_label.setStyleSheet("color: #dcdcaa; font-size: 14px;")
        self._star_label.setFixedWidth(18)
        layout.addWidget(self._star_label)

        self._phrase_label = QLabel(_THINKING_PHRASES[self._phrase_idx])
        self._phrase_label.setStyleSheet("color: #808080; font-style: italic; font-size: 12px;")
        layout.addWidget(self._phrase_label, 1)

        self._stopped = False

        self._timer = QTimer(self)
        self._timer.timeout.connect(self._tick)
        self._timer.start(900)

    def _tick(self) -> None:
        if self._stopped:
            return
        self._star_idx = (self._star_idx + 1) % len(self._STAR_FRAMES)
        self._star_label.setText(self._STAR_FRAMES[self._star_idx])

        if self._star_idx == 0:
            self._phrase_idx = (self._phrase_idx + 1) % len(_THINKING_PHRASES)
            self._phrase_label.setText(_THINKING_PHRASES[self._phrase_idx])

    def stop(self) -> None:
        self._stopped = True
        try:
            self._timer.stop()
            self._timer.timeout.disconnect(self._tick)
        except (RuntimeError, TypeError):
            pass


# ---------------------------------------------------------------------------
# Other message widgets
# ---------------------------------------------------------------------------

class QueuedMessageWidget(QFrame):
    """Displays a queued user message with dashed border."""

    def __init__(self, text: str, parent: QWidget = None):
        super().__init__(parent)
        self.setObjectName("message_queued")
        self.setStyleSheet(
            "QFrame#message_queued { border: 1px dashed #007acc; "
            "border-radius: 6px; background: #1e1e2e; }"
        )

        layout = QHBoxLayout(self)
        layout.setContentsMargins(8, 6, 8, 6)

        content_layout = QVBoxLayout()

        role_label = QLabel("You")
        role_label.setStyleSheet("color: #4ec9b0; font-weight: bold; font-size: 11px;")
        content_layout.addWidget(role_label)

        content = QLabel(text)
        content.setWordWrap(True)
        content.setTextInteractionFlags(Qt.TextInteractionFlag.TextSelectableByMouse | Qt.TextInteractionFlag.TextSelectableByKeyboard)
        content.setStyleSheet("color: #d4d4d4; font-size: 13px;")
        content_layout.addWidget(content)

        layout.addLayout(content_layout, 1)

        badge = QLabel("[queued]")
        badge.setStyleSheet("color: #808080; font-size: 10px; font-style: italic;")
        badge.setAlignment(Qt.AlignmentFlag.AlignTop)
        layout.addWidget(badge)


class UserQuestionWidget(QFrame):
    """Displays a question from the agent to the user with clickable option buttons."""

    option_selected = Signal(str)  # emitted with the chosen option text

    def __init__(self, question: str, options: list = None, parent: QWidget = None):
        super().__init__(parent)
        self.setObjectName("message_question")
        self.setStyleSheet(
            "QFrame#message_question { border: 1px solid #dcdcaa; "
            "border-radius: 6px; background: #2d2d1e; }"
        )

        layout = QVBoxLayout(self)
        layout.setContentsMargins(8, 6, 8, 6)
        layout.setSpacing(6)

        header = QLabel("Rikugan asks:")
        header.setStyleSheet("color: #dcdcaa; font-weight: bold; font-size: 11px;")
        layout.addWidget(header)

        q_label = QLabel(question)
        q_label.setWordWrap(True)
        q_label.setTextInteractionFlags(Qt.TextInteractionFlag.TextSelectableByMouse | Qt.TextInteractionFlag.TextSelectableByKeyboard)
        q_label.setStyleSheet("color: #d4d4d4; font-size: 13px;")
        layout.addWidget(q_label)

        if options:
            btn_layout = QHBoxLayout()
            btn_layout.setContentsMargins(0, 4, 0, 0)
            btn_layout.setSpacing(8)
            for opt in options:
                btn = QPushButton(opt)
                btn.setStyleSheet(
                    "QPushButton { background: #2d4a6e; color: #9cdcfe; border: 1px solid #4a7ab5; "
                    "border-radius: 4px; padding: 4px 14px; font-size: 12px; }"
                    "QPushButton:hover { background: #3a5a8a; }"
                    "QPushButton:pressed { background: #1a3a5e; }"
                    "QPushButton:disabled { color: #808080; background: #1e2a3a; border-color: #444; }"
                )
                btn.clicked.connect(lambda checked, o=opt: self._on_option(o))
                btn_layout.addWidget(btn)
            btn_layout.addStretch()
            layout.addLayout(btn_layout)
            self._buttons = btn_layout

    def _on_option(self, option: str) -> None:
        # Disable all buttons after selection
        for i in range(self._buttons.count()):
            item = self._buttons.itemAt(i)
            if item and item.widget():
                item.widget().setEnabled(False)
        self.option_selected.emit(option)


class ExplorationPhaseWidget(QFrame):
    """Displays an exploration phase transition."""

    _PHASE_ICONS = {
        "explore": "\u25b6",   # play
        "plan": "\u270e",      # pencil
        "execute": "\u2699",   # gear
        "save": "\u2714",      # checkmark
    }

    def __init__(self, from_phase: str, to_phase: str, reason: str = "",
                 parent: QWidget = None):
        super().__init__(parent)
        self.setObjectName("message_tool")
        self.setStyleSheet(
            "QFrame#message_tool { border-color: #d7ba7d; background: #2d2a1f; }"
        )

        layout = QHBoxLayout(self)
        layout.setContentsMargins(8, 6, 8, 6)
        layout.setSpacing(6)

        icon = self._PHASE_ICONS.get(to_phase, "\u2192")
        phase_label = QLabel(f"{icon}  Phase: {to_phase.upper()}")
        phase_label.setStyleSheet(
            "color: #d7ba7d; font-weight: bold; font-size: 11px;"
        )
        layout.addWidget(phase_label)

        if reason:
            reason_label = QLabel(reason)
            reason_label.setWordWrap(True)
            reason_label.setStyleSheet("color: #b0a070; font-size: 11px;")
            layout.addWidget(reason_label, 1)


class ExplorationFindingWidget(QFrame):
    """Displays a single exploration finding."""

    _CATEGORY_COLORS = {
        "function_purpose": "#4ec9b0",
        "hypothesis": "#d7ba7d",
        "constant": "#b5cea8",
        "data_structure": "#c586c0",
        "string_ref": "#ce9178",
        "import_usage": "#569cd6",
        "patch_result": "#6a9955",
        "general": "#808080",
    }

    def __init__(self, category: str, summary: str, address: str = None,
                 relevance: str = "medium", parent: QWidget = None):
        super().__init__(parent)
        self.setObjectName("message_tool")
        color = self._CATEGORY_COLORS.get(category, "#808080")
        self.setStyleSheet(
            f"QFrame#message_tool {{ border-color: {color}; }}"
        )

        layout = QHBoxLayout(self)
        layout.setContentsMargins(8, 4, 8, 4)
        layout.setSpacing(6)

        cat_label = QLabel(f"[{category}]")
        cat_label.setStyleSheet(
            f"color: {color}; font-weight: bold; font-size: 10px;"
        )
        layout.addWidget(cat_label)

        if address:
            addr_label = QLabel(address)
            addr_label.setStyleSheet(
                "color: #808080; font-family: monospace; font-size: 10px;"
            )
            layout.addWidget(addr_label)

        summary_label = QLabel(summary)
        summary_label.setWordWrap(True)
        summary_label.setStyleSheet("color: #d4d4d4; font-size: 11px;")
        layout.addWidget(summary_label, 1)

        if relevance == "high":
            rel_label = QLabel("\u2605")
            rel_label.setStyleSheet("color: #d7ba7d; font-size: 12px;")
            rel_label.setToolTip("High relevance")
            layout.addWidget(rel_label)


class ErrorMessageWidget(QFrame):
    """Displays an error message."""

    def __init__(self, error_text: str, parent: QWidget = None):
        super().__init__(parent)
        self.setObjectName("message_tool")
        self.setStyleSheet("QFrame#message_tool { border-color: #f44747; }")

        layout = QVBoxLayout(self)
        layout.setContentsMargins(8, 6, 8, 6)

        header = QLabel("Error")
        header.setStyleSheet("color: #f44747; font-weight: bold; font-size: 11px;")
        layout.addWidget(header)

        content = QLabel(error_text)
        content.setWordWrap(True)
        content.setTextInteractionFlags(Qt.TextInteractionFlag.TextSelectableByMouse | Qt.TextInteractionFlag.TextSelectableByKeyboard)
        content.setStyleSheet("color: #f44747; font-size: 12px;")
        layout.addWidget(content)

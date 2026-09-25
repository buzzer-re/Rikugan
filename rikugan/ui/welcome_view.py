"""Welcome screen shown in an empty chat instead of a blank canvas.

States what is loaded (the binary card) and offers one-click starting points.
Clicking a suggestion fills the composer rather than submitting it, so the user
can edit the prompt before sending.
"""

from __future__ import annotations

from collections.abc import Callable
from dataclasses import dataclass

from .binary_summary import BinarySummary
from .qt_compat import (
    QFrame,
    QHBoxLayout,
    QLabel,
    Qt,
    QVBoxLayout,
    QWidget,
)
from .styles import build_welcome_stylesheet

_SUBTITLE = "Ask anything about the loaded binary, or pick a starting point below."
_HINT = "<b>/</b> for skills · <b>Enter</b> to send · <b>Shift+Enter</b> for a newline"
_CHEVRON = "\u203a"
_FILE_GLYPH = "▤"

# Chips are laid out by a character budget because Qt has no flow layout; at
# sidebar width two rows of ~40 characters is what fits without clipping.
_CHIP_ROW_BUDGET = 40


@dataclass(frozen=True)
class Suggestion:
    """One starting point offered on the welcome screen."""

    icon: str
    title: str
    command: str
    prompt: str


def build_suggestions(skill_slugs: list[str] | None = None, cursor_address: int | None = None) -> list[Suggestion]:
    """Build the starting points, dropping rows whose skill is unavailable.

    ``/explore`` and ``/modify`` are built-in commands and always offered; the
    deobfuscation row only appears when that skill is actually installed.
    """
    slugs = set(skill_slugs or ())
    suggestions = [
        Suggestion(
            icon="≡",
            title="Summarize what this binary does",
            command="",
            prompt="Summarize what this binary does",
        )
    ]
    if "deobfuscation" in slugs:
        suggestions.append(
            Suggestion(
                icon="✦",
                title="Hunt for crypto & obfuscation",
                command="/deobfuscation",
                prompt="/deobfuscation ",
            )
        )
    suggestions.append(
        Suggestion(
            icon="◎",
            title="Explore autonomously, read-only",
            command="/explore",
            prompt="/explore ",
        )
    )
    patch_command = "/modify"
    if cursor_address is not None:
        patch_command = f"/modify · 0x{int(cursor_address):x}"
    suggestions.append(
        Suggestion(
            icon="✎",
            title="Patch a check at the cursor",
            command=patch_command,
            prompt="/modify ",
        )
    )
    return suggestions


def wrap_chips(chips: list[str], budget: int = _CHIP_ROW_BUDGET) -> list[list[str]]:
    """Group metadata chips into rows that fit the panel width."""
    rows: list[list[str]] = []
    current: list[str] = []
    used = 0
    for chip in chips:
        cost = len(chip) + 3  # padding and inter-chip spacing, in characters
        if current and used + cost > budget:
            rows.append(current)
            current = []
            used = 0
        current.append(chip)
        used += cost
    if current:
        rows.append(current)
    return rows


class SuggestionRow(QFrame):
    """A clickable starting point. Uses a plain callback, not a Qt signal."""

    def __init__(self, suggestion: Suggestion, parent: QWidget = None):
        super().__init__(parent)
        self.setObjectName("suggestion_row")
        self.setAttribute(Qt.WidgetAttribute.WA_Hover, True)
        self.setCursor(Qt.CursorShape.PointingHandCursor)
        self._suggestion = suggestion
        self._callback: Callable[[str], None] | None = None

        layout = QHBoxLayout(self)
        layout.setContentsMargins(10, 8, 10, 8)
        layout.setSpacing(9)

        icon = QLabel(suggestion.icon, self)
        icon.setObjectName("suggestion_icon")
        icon.setFixedSize(20, 20)
        icon.setAlignment(Qt.AlignmentFlag.AlignCenter)
        layout.addWidget(icon)

        text_layout = QVBoxLayout()
        text_layout.setContentsMargins(0, 0, 0, 0)
        text_layout.setSpacing(2)
        title = QLabel(suggestion.title, self)
        title.setObjectName("suggestion_title")
        title.setWordWrap(True)
        text_layout.addWidget(title)
        if suggestion.command:
            command = QLabel(suggestion.command, self)
            command.setObjectName("suggestion_command")
            text_layout.addWidget(command)
        layout.addLayout(text_layout, 1)

        chevron = QLabel(_CHEVRON, self)
        chevron.setObjectName("suggestion_chevron")
        layout.addWidget(chevron)

    def set_activated_callback(self, callback: Callable[[str], None] | None) -> None:
        self._callback = callback

    def mousePressEvent(self, event) -> None:
        super().mousePressEvent(event)
        if self._callback is not None:
            self._callback(self._suggestion.prompt)


class WelcomeView(QWidget):
    """Empty-chat screen: identity, binary card, and starting points."""

    def __init__(self, parent: QWidget = None):
        super().__init__(parent)
        self.setObjectName("welcome_view")
        self.setStyleSheet(build_welcome_stylesheet(self))
        self._activated_callback: Callable[[str], None] | None = None
        self._summary = BinarySummary()

        layout = QVBoxLayout(self)
        layout.setContentsMargins(18, 16, 18, 8)
        layout.setSpacing(0)
        layout.addStretch()
        layout.addWidget(self._build_subtitle())
        layout.addSpacing(14)
        layout.addWidget(self._build_binary_card())
        layout.addSpacing(14)
        layout.addWidget(self._build_section_label())
        layout.addSpacing(8)
        self._suggestion_layout = QVBoxLayout()
        self._suggestion_layout.setContentsMargins(0, 0, 0, 0)
        self._suggestion_layout.setSpacing(6)
        layout.addLayout(self._suggestion_layout)
        layout.addSpacing(14)
        layout.addWidget(self._build_hint())
        layout.addStretch()

        self.set_suggestions(build_suggestions())

    def _build_subtitle(self) -> QLabel:
        subtitle = QLabel(_SUBTITLE, self)
        subtitle.setObjectName("welcome_subtitle")
        subtitle.setAlignment(Qt.AlignmentFlag.AlignCenter)
        subtitle.setWordWrap(True)
        return subtitle

    def _build_binary_card(self) -> QFrame:
        self._card = QFrame(self)
        self._card.setObjectName("binary_card")
        card_layout = QVBoxLayout(self._card)
        card_layout.setContentsMargins(11, 10, 11, 10)
        card_layout.setSpacing(7)

        name_row = QHBoxLayout()
        name_row.setContentsMargins(0, 0, 0, 0)
        name_row.setSpacing(8)
        file_glyph = QLabel(_FILE_GLYPH, self._card)
        file_glyph.setObjectName("suggestion_chevron")
        name_row.addWidget(file_glyph)
        self._name_label = QLabel("No binary loaded", self._card)
        self._name_label.setObjectName("binary_name")
        name_row.addWidget(self._name_label, 1)
        card_layout.addLayout(name_row)

        self._chip_layout = QVBoxLayout()
        self._chip_layout.setContentsMargins(0, 0, 0, 0)
        self._chip_layout.setSpacing(5)
        card_layout.addLayout(self._chip_layout)
        return self._card

    def _build_section_label(self) -> QLabel:
        label = QLabel("Start with", self)
        label.setObjectName("welcome_section")
        return label

    def _build_hint(self) -> QLabel:
        hint = QLabel(_HINT, self)
        hint.setObjectName("welcome_hint")
        hint.setAlignment(Qt.AlignmentFlag.AlignCenter)
        hint.setWordWrap(True)
        return hint

    def set_activated_callback(self, callback: Callable[[str], None] | None) -> None:
        """Set the callback invoked with the prompt text of a clicked row."""
        self._activated_callback = callback

    def set_suggestions(self, suggestions: list[Suggestion]) -> None:
        """Replace the starting-point rows."""
        while self._suggestion_layout.count():
            item = self._suggestion_layout.takeAt(0)
            widget = item.widget() if item else None
            if widget is not None:
                # setParent(None) removes it from the view now; deleteLater
                # alone would leave the old rows painted until the next tick.
                widget.setParent(None)
                widget.deleteLater()
        for suggestion in suggestions:
            row = SuggestionRow(suggestion, parent=self)
            row.set_activated_callback(self._on_row_activated)
            self._suggestion_layout.addWidget(row)

    def set_binary_summary(self, summary: BinarySummary) -> None:
        """Fill the binary card; an empty summary leaves the placeholder text."""
        self._summary = summary
        if summary.name:
            self._name_label.setText(summary.short_name)
            self._name_label.setToolTip(summary.name)
        while self._chip_layout.count():
            item = self._chip_layout.takeAt(0)
            row_layout = item.layout() if item else None
            if row_layout is None:
                continue
            while row_layout.count():
                sub = row_layout.takeAt(0)
                widget = sub.widget() if sub else None
                if widget is not None:
                    widget.setParent(None)
                    widget.deleteLater()
        for chips in wrap_chips(summary.chips):
            row = QHBoxLayout()
            row.setContentsMargins(0, 0, 0, 0)
            row.setSpacing(5)
            for chip in chips:
                label = QLabel(chip, self._card)
                label.setObjectName("binary_chip")
                row.addWidget(label)
            row.addStretch()
            self._chip_layout.addLayout(row)

    def _on_row_activated(self, prompt: str) -> None:
        if self._activated_callback is not None:
            self._activated_callback(prompt)

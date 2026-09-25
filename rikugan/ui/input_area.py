"""Multi-line input area with Enter/Shift+Enter handling and /skill autocomplete."""

from __future__ import annotations

from .qt_compat import (
    QEvent,
    QFrame,
    QLabel,
    QPlainTextEdit,
    QSizePolicy,
    Qt,
    QVBoxLayout,
    QWidget,
)
from .styles import build_input_area_stylesheet, host_stylesheet, use_native_host_theme

PLACEHOLDER_IDLE = "Ask about this binary\u2026"
PLACEHOLDER_BUSY = "Rikugan is thinking\u2026"

# The composer starts one line tall and grows to this before it scrolls.
_MIN_VISIBLE_LINES = 1
_MAX_VISIBLE_LINES = 5
# Vertical breathing room inside the editor, on top of the document margins.
_INPUT_PADDING = 8


class _SkillPopup(QFrame):
    """Lightweight autocomplete popup for /skill slugs.

    Uses QLabel items inside a QFrame — avoids QListWidget Shiboken surface.
    NO Signal definitions here — this class is defined during the Shiboken
    bypass window and adding Signal descriptors corrupts Shiboken's internal
    signal registry on Python 3.14 + Shiboken 6.8.2, causing SIGSEGV in
    checkQtSignal() on any later signal operation (e.g. QTimer.singleShot).
    """

    def __init__(self, parent: QWidget = None):
        super().__init__(parent)
        self.setObjectName("skill_popup")
        self.setWindowFlags(Qt.WindowType.ToolTip)
        self.setStyleSheet(
            host_stylesheet(
                "QFrame#skill_popup { background: #2d2d2d; border: 1px solid #555; "
                "border-radius: 4px; padding: 2px; }"
                "QLabel { color: #d4d4d4; padding: 3px 8px; }"
                'QLabel[selected="true"] { background: #094771; border-radius: 3px; }',
                'QLabel[selected="true"] { font-weight: bold; }',
            )
        )
        self._layout = QVBoxLayout(self)
        self._layout.setContentsMargins(2, 2, 2, 2)
        self._layout.setSpacing(0)
        self._labels: list[QLabel] = []
        self._slugs: list[str] = []
        self._selected_idx = 0

    def set_items(self, slugs: list[str]) -> None:
        """Replace popup contents with filtered slugs."""
        # Clear old labels
        for lbl in self._labels:
            self._layout.removeWidget(lbl)
            lbl.setParent(None)
        self._labels.clear()
        self._slugs = list(slugs)
        self._selected_idx = 0

        for slug in slugs:
            lbl = QLabel(f"/{slug}")
            self._labels.append(lbl)
            self._layout.addWidget(lbl)

        self._update_highlight()
        self.adjustSize()

    def _update_highlight(self) -> None:
        for i, lbl in enumerate(self._labels):
            lbl.setProperty("selected", "true" if i == self._selected_idx else "false")
            lbl.style().unpolish(lbl)
            lbl.style().polish(lbl)

    def move_selection(self, delta: int) -> None:
        if not self._slugs:
            return
        self._selected_idx = (self._selected_idx + delta) % len(self._slugs)
        self._update_highlight()

    def current_slug(self) -> str | None:
        if 0 <= self._selected_idx < len(self._slugs):
            return self._slugs[self._selected_idx]
        return None

    def is_empty(self) -> bool:
        return len(self._slugs) == 0


class InputArea(QPlainTextEdit):
    """Chat input area with keyboard shortcuts.

    - Enter: submit message
    - Shift+Enter: newline
    - Escape: cancel running agent
    - /: skill autocomplete popup

    Uses plain Python callbacks instead of PySide6 Signals to avoid
    Shiboken C++ dispatch crashes (UAF in checkQtSignal on Python 3.14).
    """

    # Do NOT define Signal() here — Shiboken signal dispatch causes
    # random SIGSEGV during emit() on Python 3.14 + PySide6 6.8.2.

    def __init__(self, parent: QWidget = None):
        super().__init__(parent)
        self.setObjectName("input_area")
        self.setPlaceholderText(PLACEHOLDER_IDLE)
        self.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Fixed)
        self.setVerticalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAsNeeded)
        # Grow with the text instead of always reserving the maximum. An empty
        # composer used to hold ~150px of a 420px-tall panel.
        self.document().contentsChanged.connect(self._sync_height)
        self._enabled = True
        self._skill_slugs: list[str] = []
        self._popup: _SkillPopup | None = None
        self._submit_callback = None  # Callable[[str], None]
        self._cancel_callback = None  # Callable[[], None]
        self._applying_theme = False
        self._theme_css = ""
        self._flat = False
        self._focus_callback = None  # Callable[[bool], None]
        self._apply_theme()
        self._sync_height()

    def _sync_height(self) -> None:
        """Size the editor to its content, between one and a few lines."""
        doc = self.document()
        # QPlainTextDocumentLayout reports its height in lines, not pixels.
        lines = max(_MIN_VISIBLE_LINES, int(doc.size().height()))
        lines = min(lines, _MAX_VISIBLE_LINES)
        frame = int(doc.documentMargin() * 2) + 2 * self.frameWidth() + _INPUT_PADDING
        self.setFixedHeight(lines * self.fontMetrics().lineSpacing() + frame)

    def resizeEvent(self, event) -> None:
        super().resizeEvent(event)
        # Wrapping changes the line count, which changes the height we need.
        self._sync_height()

    def set_submit_callback(self, callback) -> None:
        """Set the callback for submit (Enter key). Callback signature: (str) -> None."""
        self._submit_callback = callback

    def set_cancel_callback(self, callback) -> None:
        """Set the callback for cancel (Escape key). Callback signature: () -> None."""
        self._cancel_callback = callback

    def set_focus_callback(self, callback) -> None:
        """Set the focus-change callback. Signature: (bool) -> None."""
        self._focus_callback = callback

    def set_flat(self, flat: bool) -> None:
        """Drop the editor's own border so a parent frame can draw it."""
        self._flat = flat
        self._theme_css = ""
        self._apply_theme()

    def trigger_skill_autocomplete(self) -> None:
        """Start a ``/`` skill command and open the autocomplete popup."""
        if not self._enabled:
            return
        if not self.toPlainText().startswith("/"):
            self.setPlainText("/")
            cursor = self.textCursor()
            cursor.movePosition(cursor.MoveOperation.End)
            self.setTextCursor(cursor)
        self.setFocus()
        self._check_autocomplete()

    def set_skill_slugs(self, slugs: list[str]) -> None:
        """Set the list of available skill slugs for autocomplete.

        Automatically includes /plan, /modify, /explore, and /research as built-in commands.
        """
        combined = set(slugs)
        combined.update(("plan", "modify", "explore", "research"))
        self._skill_slugs = sorted(combined)

    def keyPressEvent(self, event) -> None:
        # Handle popup navigation when popup is visible
        if self._popup and self._popup.isVisible():
            if event.key() in (Qt.Key.Key_Return, Qt.Key.Key_Enter, Qt.Key.Key_Tab):
                slug = self._popup.current_slug()
                if slug:
                    self._accept_completion(slug)
                return
            elif event.key() == Qt.Key.Key_Escape:
                self._dismiss_popup()
                return
            elif event.key() == Qt.Key.Key_Down:
                self._popup.move_selection(1)
                return
            elif event.key() == Qt.Key.Key_Up:
                self._popup.move_selection(-1)
                return

        if event.key() in (Qt.Key.Key_Return, Qt.Key.Key_Enter):
            if event.modifiers() & Qt.KeyboardModifier.ShiftModifier:
                super().keyPressEvent(event)
            else:
                text = self.toPlainText().strip()
                if text and self._enabled:
                    # Plain Python callback — no Shiboken signal dispatch
                    if self._submit_callback:
                        self._submit_callback(text)
                    self.clear()
        elif event.key() == Qt.Key.Key_Escape:
            if self._cancel_callback:
                self._cancel_callback()
        else:
            # Let the base class process the key first (inserts character),
            # then check if we need to show/update/dismiss the autocomplete.
            super().keyPressEvent(event)
            self._check_autocomplete()

    def set_enabled(self, enabled: bool) -> None:
        self._enabled = enabled
        self.setReadOnly(not enabled)
        if enabled:
            self.setPlaceholderText(PLACEHOLDER_IDLE)
        else:
            self.setPlaceholderText(PLACEHOLDER_BUSY)

    def _apply_theme(self) -> None:
        """Apply host-aware styling to the text editor.

        Flat mode always applies its own stylesheet: the global dark theme
        styles ``QPlainTextEdit#input_area`` with a border that would double up
        with the composer frame's.
        """
        if self._applying_theme:
            return
        if not use_native_host_theme() and not self._flat:
            return
        css = build_input_area_stylesheet(self, flat=self._flat)
        if css == self._theme_css:
            return
        self._applying_theme = True
        try:
            self.setStyleSheet(css)
            self._theme_css = css
        finally:
            self._applying_theme = False

    def showEvent(self, event) -> None:
        super().showEvent(event)
        self._apply_theme()

    def changeEvent(self, event) -> None:
        super().changeEvent(event)
        if not use_native_host_theme() and not self._flat:
            return
        event_type = getattr(event, "type", lambda: None)()
        palette_change = getattr(QEvent, "PaletteChange", None)
        app_palette_change = getattr(QEvent, "ApplicationPaletteChange", None)
        parent_change = getattr(QEvent, "ParentChange", None)
        if event_type in {palette_change, app_palette_change, parent_change}:
            self._apply_theme()

    def focusInEvent(self, event) -> None:
        super().focusInEvent(event)
        if self._focus_callback is not None:
            self._focus_callback(True)

    def focusOutEvent(self, event) -> None:
        super().focusOutEvent(event)
        if self._focus_callback is not None:
            self._focus_callback(False)

    # ------------------------------------------------------------------
    # Autocomplete
    # ------------------------------------------------------------------

    def _check_autocomplete(self) -> None:
        """Check current text and show/hide the skill autocomplete popup."""
        text = self.toPlainText()
        if not text.startswith("/") or not self._skill_slugs:
            self._dismiss_popup()
            return

        # Extract partial slug (everything after / up to first space)
        parts = text[1:].split(None, 1)
        # If there's already a space, the slug is complete — dismiss
        if len(parts) > 1:
            self._dismiss_popup()
            return

        partial = parts[0] if parts else ""
        matches = [s for s in self._skill_slugs if s.startswith(partial)]

        if not matches:
            self._dismiss_popup()
            return

        self._show_popup(matches)

    def _show_popup(self, slugs: list[str]) -> None:
        if self._popup is None:
            self._popup = _SkillPopup()
        self._popup.set_items(slugs)

        # Position above the input area
        pos = self.mapToGlobal(self.rect().topLeft())
        popup_height = self._popup.sizeHint().height()
        self._popup.move(pos.x(), pos.y() - popup_height - 4)
        self._popup.show()

    def _dismiss_popup(self) -> None:
        if self._popup and self._popup.isVisible():
            self._popup.hide()

    def _accept_completion(self, slug: str) -> None:
        """Replace current text with /slug and a trailing space."""
        self._dismiss_popup()
        self.setPlainText(f"/{slug} ")
        # Move cursor to end
        cursor = self.textCursor()
        cursor.movePosition(cursor.MoveOperation.End)
        self.setTextCursor(cursor)

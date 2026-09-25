"""Composer: the input frame with an inline control row.

The old composer spent a fixed 64 px column on a "Send" button, which is most
of a sidebar's text width. Here the send button is a 28 px icon inside the
input frame, sharing a control row with the skill shortcuts and the model
pill, and Stop replaces Send in place so the layout never shifts mid-turn.
"""

from __future__ import annotations

from collections.abc import Callable

from .context_bar import shorten_model_name
from .input_area import InputArea
from .qt_compat import (
    QFrame,
    QHBoxLayout,
    QLabel,
    QToolButton,
    QVBoxLayout,
    QWidget,
)
from .styles import build_composer_stylesheet

_SEND_GLYPH = "↑"
_STOP_GLYPH = "■"


class Composer(QWidget):
    """Input frame plus control row. Plain callbacks, no Qt signals."""

    def __init__(self, parent: QWidget = None):
        super().__init__(parent)
        self.setObjectName("composer")
        self.setStyleSheet(build_composer_stylesheet(self))
        self._send_callback: Callable[[], None] | None = None
        self._cancel_callback: Callable[[], None] | None = None
        self._running = False

        outer = QVBoxLayout(self)
        outer.setContentsMargins(8, 6, 8, 6)
        outer.setSpacing(0)

        self._frame = QFrame(self)
        self._frame.setObjectName("composer_frame")
        self._frame.setProperty("focused", "false")
        frame_layout = QVBoxLayout(self._frame)
        frame_layout.setContentsMargins(6, 5, 6, 4)
        frame_layout.setSpacing(2)

        self.input_area = InputArea(self._frame)
        self.input_area.set_flat(True)
        self.input_area.set_focus_callback(self._on_focus_changed)
        frame_layout.addWidget(self.input_area)
        frame_layout.addLayout(self._build_control_row())

        outer.addWidget(self._frame)

    def _build_control_row(self) -> QHBoxLayout:
        row = QHBoxLayout()
        row.setContentsMargins(3, 0, 0, 0)
        row.setSpacing(4)

        self._skills_btn = self._make_chip(
            "Skills",
            "Insert a skill command (/)",
            self.input_area.trigger_skill_autocomplete,
        )
        row.addWidget(self._skills_btn)
        self._modify_btn = self._make_chip(
            "Modify",
            "Start a guided patch (/modify)",
            lambda: self.set_text("/modify "),
        )
        row.addWidget(self._modify_btn)
        row.addStretch()

        self._model_label = QLabel("", self._frame)
        self._model_label.setObjectName("composer_model")
        row.addWidget(self._model_label)

        self._send_btn = QToolButton(self._frame)
        self._send_btn.setObjectName("composer_send")
        self._send_btn.setText(_SEND_GLYPH)
        self._send_btn.setToolTip("Send (Enter)")
        self._send_btn.setFixedSize(28, 28)
        self._send_btn.clicked.connect(self._on_send)
        row.addWidget(self._send_btn)
        return row

    def _make_chip(self, text: str, tooltip: str, callback: Callable[[], None]) -> QToolButton:
        chip = QToolButton(self._frame)
        chip.setObjectName("composer_chip")
        chip.setText(text)
        chip.setToolTip(tooltip)
        chip.clicked.connect(callback)
        return chip

    # --- callbacks -----------------------------------------------------

    def set_send_callback(self, callback: Callable[[], None] | None) -> None:
        self._send_callback = callback

    def set_cancel_callback(self, callback: Callable[[], None] | None) -> None:
        self._cancel_callback = callback
        self.input_area.set_cancel_callback(callback)

    def set_submit_callback(self, callback: Callable[[str], None] | None) -> None:
        self.input_area.set_submit_callback(callback)

    def _on_send(self) -> None:
        if self._running:
            if self._cancel_callback is not None:
                self._cancel_callback()
            return
        if self._send_callback is not None:
            self._send_callback()

    def _on_focus_changed(self, focused: bool) -> None:
        self._frame.setProperty("focused", "true" if focused else "false")
        self._frame.style().unpolish(self._frame)
        self._frame.style().polish(self._frame)

    # --- state ---------------------------------------------------------

    def set_running(self, running: bool) -> None:
        """Swap Send for Stop in place while a turn is running."""
        self._running = running
        self._send_btn.setObjectName("composer_stop" if running else "composer_send")
        self._send_btn.setText(_STOP_GLYPH if running else _SEND_GLYPH)
        self._send_btn.setToolTip("Stop (Esc)" if running else "Send (Enter)")
        self._send_btn.style().unpolish(self._send_btn)
        self._send_btn.style().polish(self._send_btn)

    def set_input_enabled(self, enabled: bool) -> None:
        """Gate free-text input (button-only approval states)."""
        self.input_area.set_enabled(enabled)
        self._skills_btn.setEnabled(enabled)
        self._modify_btn.setEnabled(enabled)
        self._send_btn.setEnabled(enabled or self._running)

    def set_model(self, model: str) -> None:
        self._model_label.setText(shorten_model_name(model))
        self._model_label.setToolTip(model)

    def set_placeholder(self, text: str) -> None:
        self.input_area.setPlaceholderText(text)

    def set_skill_slugs(self, slugs: list[str]) -> None:
        self.input_area.set_skill_slugs(slugs)

    # --- text ----------------------------------------------------------

    def text(self) -> str:
        return self.input_area.toPlainText().strip()

    def set_text(self, text: str) -> None:
        self.input_area.setPlainText(text)
        cursor = self.input_area.textCursor()
        cursor.movePosition(cursor.MoveOperation.End)
        self.input_area.setTextCursor(cursor)
        self.input_area.setFocus()

    def clear(self) -> None:
        self.input_area.clear()

    def focus_input(self) -> None:
        self.input_area.setFocus()

"""Chat header: identity mark, chat switcher, new chat, and an overflow menu.

At sidebar width the chat list cannot afford a permanent column, so the header
carries the chat name as a switcher button and folds the per-chat actions
(fork / export / delete) plus settings into a single overflow menu. It carries
no product mark: the host already names the pane and shows the icon in its own
sidebar rail.
"""

from __future__ import annotations

from collections.abc import Callable

from .qt_compat import (
    QHBoxLayout,
    QMenu,
    QToolButton,
    QWidget,
    qt_run,
)
from .styles import build_panel_header_stylesheet

_CHEVRON = "▾"
_PLUS = "+"
_OVERFLOW = "\u2026"
_SIDEBAR = "\u2630"  # trigram for heaven, reads as a list/menu glyph
_MCP = "\u2b21"  # hexagon: the host's own MCP tools
_MAX_TITLE_CHARS = 26


def elide_title(title: str, limit: int = _MAX_TITLE_CHARS) -> str:
    """Trim a chat title to the width the switcher button can show."""
    text = (title or "").strip() or "Untitled"
    if len(text) <= limit:
        return text
    return text[: limit - 1].rstrip() + "…"


class PanelHeader(QWidget):
    """Header bar above the mode tabs. Plain callbacks, no Qt signals."""

    def __init__(self, parent: QWidget = None):
        super().__init__(parent)
        self.setObjectName("panel_header")
        self.setStyleSheet(build_panel_header_stylesheet(self))
        self._switcher_callback: Callable[[], None] | None = None
        self._new_callback: Callable[[], None] | None = None
        self._fork_callback: Callable[[], None] | None = None
        self._export_callback: Callable[[], None] | None = None
        self._delete_callback: Callable[[], None] | None = None
        self._settings_callback: Callable[[], None] | None = None
        self._mutations_callback: Callable[[], None] | None = None
        self._mcp_callback: Callable[[], None] | None = None

        layout = QHBoxLayout(self)
        layout.setContentsMargins(10, 7, 8, 7)
        layout.setSpacing(8)

        self._sidebar_btn = self._make_icon_button(_SIDEBAR, "Show chat list", self._on_switcher)
        layout.addWidget(self._sidebar_btn)

        self._switcher = QToolButton(self)
        self._switcher.setObjectName("chat_switcher")
        self._switcher.setToolTip("Switch chat")
        self._switcher.clicked.connect(self._on_switcher)
        # QToolButton ignores QSS text-align, so it hugs its text and a stretch
        # keeps it against the left edge instead of centring in the header.
        layout.addWidget(self._switcher)
        layout.addStretch()

        # Only shown once a host MCP server has actually answered a handshake.
        self._mcp_btn = self._make_icon_button(_MCP, "Binary Ninja MCP tools", self._on_mcp)
        self._mcp_btn.setCheckable(True)
        self._mcp_btn.setVisible(False)
        layout.addWidget(self._mcp_btn)

        self._new_btn = self._make_icon_button(_PLUS, "New chat", self._on_new)
        layout.addWidget(self._new_btn)
        self._overflow_btn = self._make_icon_button(_OVERFLOW, "More actions", self._on_overflow)
        layout.addWidget(self._overflow_btn)

        self.set_chat_title("Untitled")

    def _make_icon_button(self, text: str, tooltip: str, callback: Callable[[], None]) -> QToolButton:
        button = QToolButton(self)
        button.setObjectName("header_icon")
        button.setText(text)
        button.setToolTip(tooltip)
        button.setFixedSize(26, 26)
        button.clicked.connect(callback)
        return button

    def set_callbacks(
        self,
        switcher: Callable[[], None],
        new: Callable[[], None],
        fork: Callable[[], None],
        export: Callable[[], None],
        delete: Callable[[], None],
        settings: Callable[[], None],
        mutations: Callable[[], None],
    ) -> None:
        self._switcher_callback = switcher
        self._new_callback = new
        self._fork_callback = fork
        self._export_callback = export
        self._delete_callback = delete
        self._settings_callback = settings
        self._mutations_callback = mutations

    def set_chat_title(self, title: str) -> None:
        """Show the active chat's name on the switcher button."""
        self._switcher.setText(f"{elide_title(title)}  {_CHEVRON}")
        self._switcher.setToolTip(title or "Untitled")

    def set_native_mcp_callback(self, callback: Callable[[], None] | None) -> None:
        self._mcp_callback = callback

    def set_native_mcp_available(self, available: bool) -> None:
        """Show the toggle only when the host actually offers MCP tools."""
        self._mcp_btn.setVisible(available)

    def set_native_mcp_active(self, active: bool) -> None:
        self._mcp_btn.setChecked(active)
        self._mcp_btn.setToolTip(
            "Binary Ninja MCP tools: on (click to disable)" if active else "Binary Ninja MCP tools: off (click to use)"
        )

    def _on_mcp(self) -> None:
        if self._mcp_callback is not None:
            self._mcp_callback()

    def set_sidebar_open(self, is_open: bool) -> None:
        """Reflect whether the chat list is showing."""
        self._sidebar_btn.setToolTip("Hide chat list" if is_open else "Show chat list")
        self._sidebar_btn.setDown(is_open)

    def set_switcher_enabled(self, enabled: bool) -> None:
        """Hide the switcher chevron affordance when there is nothing to switch to."""
        self._switcher.setEnabled(enabled)

    def _on_switcher(self) -> None:
        if self._switcher_callback is not None:
            self._switcher_callback()

    def _on_new(self) -> None:
        if self._new_callback is not None:
            self._new_callback()

    def _on_overflow(self) -> None:
        menu = QMenu(self)
        fork_action = menu.addAction("Fork Chat")
        export_action = menu.addAction("Export Chat")
        delete_action = menu.addAction("Delete Chat")
        menu.addSeparator()
        mutations_action = menu.addAction("Mutation Log")
        settings_action = menu.addAction("Settings")
        below = self._overflow_btn.mapToGlobal(self._overflow_btn.rect().bottomLeft())
        action = qt_run(menu, below)
        handlers = {
            fork_action: self._fork_callback,
            export_action: self._export_callback,
            delete_action: self._delete_callback,
            mutations_action: self._mutations_callback,
            settings_action: self._settings_callback,
        }
        handler = handlers.get(action)
        if handler is not None:
            handler()

"""Tools panel: container for bulk renamer and agent tree.

Can be shown as an independent window (QDialog) or embedded in a layout.
"""

from __future__ import annotations

from .qt_compat import (
    QFrame,
    QHBoxLayout,
    QLabel,
    QTabWidget,
    QVBoxLayout,
    QWidget,
)
from .styles import (
    PRODUCT_ACCENT,
    PRODUCT_BORDER,
    PRODUCT_MUTED,
    PRODUCT_PANEL,
    PRODUCT_SURFACE,
    PRODUCT_SURFACE_HI,
    PRODUCT_TEXT,
    get_host_font_tokens,
    maybe_host_stylesheet,
)

# Tab indices are referenced by the panel and by host actions.
RENAMER_TAB_INDEX = 0
AGENTS_TAB_INDEX = 1

_HEADER_STYLE = f"color: {PRODUCT_TEXT}; font-weight: bold;"
_PLACEHOLDER_STYLE = f"color: {PRODUCT_MUTED}; padding: 20px;"


def _panel_stylesheet(source=None) -> str:
    """Tools chrome, in the shared product colors and the host's type size."""
    fonts = get_host_font_tokens(source)
    return (
        f"QWidget#tools_panel {{ background: {PRODUCT_PANEL}; }}"
        f"QTabWidget::pane {{ border: none; background: {PRODUCT_PANEL}; }}"
        f"QTabBar::tab {{ background: {PRODUCT_SURFACE}; color: {PRODUCT_MUTED}; "
        f"border: 1px solid {PRODUCT_BORDER}; border-bottom: none; padding: 5px 14px; "
        f"font-size: {fonts['font_base']}; min-width: 60px; }}"
        f"QTabBar::tab:selected {{ background: {PRODUCT_PANEL}; color: {PRODUCT_TEXT}; "
        f"border-bottom: 2px solid {PRODUCT_ACCENT}; }}"
        f"QTabBar::tab:hover:!selected {{ background: {PRODUCT_SURFACE_HI}; color: {PRODUCT_TEXT}; }}"
    )


_BTN_STYLE = (
    "QPushButton { background: #2d2d2d; color: #d4d4d4; border: 1px solid #3c3c3c; "
    "border-radius: 4px; padding: 2px 8px; font-size: 11px; }"
    "QPushButton:hover { background: #3c3c3c; }"
)


class ToolsPanel(QWidget):
    """Standalone tools window containing tabs: Renamer, Agents."""

    def __init__(self, parent: QWidget = None):
        super().__init__(parent)
        self.setObjectName("tools_panel")
        self.setWindowTitle("Rikugan Tools")
        self.setStyleSheet(maybe_host_stylesheet(_panel_stylesheet(self)))
        # No minimum size — this widget is embedded in IDA dockable forms
        # and Binary Ninja sidebars, which can be any size.

        main_layout = QVBoxLayout(self)
        main_layout.setContentsMargins(0, 0, 0, 0)
        main_layout.setSpacing(0)

        # Header bar with title (hidden when docked in IDA)
        self._header = QFrame()
        self._header.setObjectName("tools_panel_header")
        header_layout = QHBoxLayout(self._header)
        header_layout.setContentsMargins(12, 8, 12, 8)

        title = QLabel("Tools")
        title.setStyleSheet(maybe_host_stylesheet(_HEADER_STYLE))
        header_layout.addWidget(title)
        header_layout.addStretch()

        main_layout.addWidget(self._header)

        # Tab widget
        self._tabs = QTabWidget()
        self._tabs.setObjectName("tools_tabs")

        # Placeholder tabs
        self._renamer_placeholder = QLabel("Not loaded")
        self._renamer_placeholder.setStyleSheet(maybe_host_stylesheet(_PLACEHOLDER_STYLE))
        self._renamer_placeholder.setWordWrap(True)
        self._tabs.addTab(self._renamer_placeholder, "Renamer")

        self._agents_placeholder = QLabel("Not loaded")
        self._agents_placeholder.setStyleSheet(maybe_host_stylesheet(_PLACEHOLDER_STYLE))
        self._agents_placeholder.setWordWrap(True)
        self._tabs.addTab(self._agents_placeholder, "Agents")
        self._tabs.setCurrentIndex(RENAMER_TAB_INDEX)

        main_layout.addWidget(self._tabs)

    def _replace_tab(self, index: int, widget: QWidget, label: str) -> None:
        """Replace the widget at the given tab index, keeping the selection.

        ``removeTab`` on the *current* tab makes Qt select its neighbour, so
        swapping the placeholder for the real Renamer used to leave the panel
        sitting on Agents.
        """
        was_current = self._tabs.currentIndex() == index
        old = self._tabs.widget(index)
        self._tabs.removeTab(index)
        self._tabs.insertTab(index, widget, label)
        if was_current:
            self._tabs.setCurrentIndex(index)
        if old is not None:
            old.deleteLater()

    def set_renamer_widget(self, widget: QWidget) -> None:
        """Replace the Renamer tab content."""
        self._replace_tab(RENAMER_TAB_INDEX, widget, "Renamer")

    def set_agents_widget(self, widget: QWidget) -> None:
        """Replace the Agents tab content."""
        self._replace_tab(AGENTS_TAB_INDEX, widget, "Agents")

    def hide_header(self) -> None:
        """Hide the title bar (used when embedded in a dockable form)."""
        self._header.setVisible(False)

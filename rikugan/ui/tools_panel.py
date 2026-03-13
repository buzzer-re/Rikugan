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

_HEADER_STYLE = "color: #d4d4d4; font-weight: bold; font-size: 12px;"

_PANEL_STYLE = """
    QWidget#tools_panel {
        background: #1e1e1e;
    }
    QTabWidget::pane {
        border: none;
        background: #1e1e1e;
    }
    QTabBar::tab {
        background: #2d2d2d;
        color: #808080;
        border: 1px solid #3c3c3c;
        border-bottom: none;
        padding: 5px 14px;
        font-size: 11px;
        min-width: 60px;
    }
    QTabBar::tab:selected {
        background: #1e1e1e;
        color: #d4d4d4;
        border-bottom: 2px solid #4ec9b0;
    }
    QTabBar::tab:hover:!selected {
        background: #353535;
        color: #d4d4d4;
    }
"""

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
        self.setStyleSheet(_PANEL_STYLE)
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
        title.setStyleSheet(_HEADER_STYLE)
        header_layout.addWidget(title)
        header_layout.addStretch()

        main_layout.addWidget(self._header)

        # Tab widget
        self._tabs = QTabWidget()
        self._tabs.setObjectName("tools_tabs")

        # Placeholder tabs
        self._renamer_placeholder = QLabel("Not loaded")
        self._renamer_placeholder.setStyleSheet("color: #808080; padding: 20px;")
        self._renamer_placeholder.setWordWrap(True)
        self._tabs.addTab(self._renamer_placeholder, "Renamer")

        self._agents_placeholder = QLabel("Not loaded")
        self._agents_placeholder.setStyleSheet("color: #808080; padding: 20px;")
        self._agents_placeholder.setWordWrap(True)
        self._tabs.addTab(self._agents_placeholder, "Agents")

        main_layout.addWidget(self._tabs)

    def _replace_tab(self, index: int, widget: QWidget, label: str) -> None:
        """Replace the widget at the given tab index."""
        old = self._tabs.widget(index)
        self._tabs.removeTab(index)
        self._tabs.insertTab(index, widget, label)
        if old is not None:
            old.deleteLater()

    def set_renamer_widget(self, widget: QWidget) -> None:
        """Replace the Renamer tab content."""
        self._replace_tab(0, widget, "Renamer")

    def set_agents_widget(self, widget: QWidget) -> None:
        """Replace the Agents tab content."""
        self._replace_tab(1, widget, "Agents")

    def hide_header(self) -> None:
        """Hide the title bar (used when embedded in a dockable form)."""
        self._header.setVisible(False)

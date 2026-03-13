"""IDA PluginForm wrapper for the Rikugan Tools panel (dockable view)."""

from __future__ import annotations

import importlib
from typing import Any

from rikugan.ui.qt_compat import QTimer, QVBoxLayout, QWidget
from rikugan.ui.tools_panel import ToolsPanel

idaapi = importlib.import_module("idaapi")


def _widget_alive(w: QWidget) -> bool:
    """Return True if the underlying C++ QWidget has not been deleted."""
    try:
        from shiboken6 import isValid

        return isValid(w)
    except Exception:
        try:
            # Fallback: any attribute access on a dead wrapper raises
            w.objectName()
            return True
        except RuntimeError:
            return False


class RikuganToolsForm(idaapi.PluginForm):
    """IDA dockable form embedding the shared ToolsPanel widget."""

    def __init__(self, tools_widget: ToolsPanel):
        super().__init__()
        self._tools_widget = tools_widget
        self._form_widget: QWidget | None = None
        self._shown = False

    def OnCreate(self, form: Any) -> None:
        try:
            self._form_widget = self.FormToPyQtWidget(form)
        except Exception:
            self._form_widget = self.FormToPySideWidget(form)

        layout = QVBoxLayout(self._form_widget)
        layout.setContentsMargins(0, 0, 0, 0)

        if _widget_alive(self._tools_widget):
            self._tools_widget.hide_header()
            self._tools_widget.setVisible(True)
            layout.addWidget(self._tools_widget)
        else:
            # Widget was destroyed by a prior close — create a placeholder.
            # panel_core will re-set the real widget on next init.
            from rikugan.ui.qt_compat import QLabel

            placeholder = QLabel("Tools panel is reloading...")
            placeholder.setStyleSheet("color: #808080; padding: 20px;")
            layout.addWidget(placeholder)

        # IDA's docking system doesn't always trigger a paint after the
        # initial Show().  Schedule a deferred update so the form contents
        # become visible without requiring the user to switch away and back.
        QTimer.singleShot(50, self._force_update)

    def _force_update(self) -> None:
        """Force the form and tools widget to repaint after IDA docking."""
        if self._form_widget is not None:
            self._form_widget.update()
        if _widget_alive(self._tools_widget):
            self._tools_widget.update()
            self._tools_widget.updateGeometry()

    def OnClose(self, form: Any) -> None:
        # Reparent the tools widget out of the doomed form so it is not
        # destroyed together with the form's QWidget tree.
        if _widget_alive(self._tools_widget):
            self._tools_widget.setParent(None)
            self._tools_widget.setVisible(False)
        self._shown = False

    def show(self) -> None:
        if not self._shown:
            self.Show(
                "Rikugan Tools",
                options=(idaapi.PluginForm.WOPN_TAB | idaapi.PluginForm.WOPN_PERSIST),
            )
            self._shown = True
        else:
            widget = idaapi.find_widget("Rikugan Tools")
            if widget:
                idaapi.activate_widget(widget, True)

    def hide(self) -> None:
        widget = idaapi.find_widget("Rikugan Tools")
        if widget:
            idaapi.close_widget(widget, 0)
        self._shown = False

    @property
    def is_visible(self) -> bool:
        widget = idaapi.find_widget("Rikugan Tools")
        return widget is not None and self._shown

    def set_tab(self, index: int) -> None:
        if _widget_alive(self._tools_widget) and hasattr(self._tools_widget, "_tabs"):
            self._tools_widget._tabs.setCurrentIndex(index)

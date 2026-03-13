"""IDA PluginForm wrapper around the shared Rikugan panel core."""

from __future__ import annotations

import importlib
from typing import Any

from rikugan.ui.panel_core import RikuganPanelCore
from rikugan.ui.qt_compat import QVBoxLayout, QWidget

from .actions import RikuganUIHooks
from .session_controller import IdaSessionController
from .tools_form import RikuganToolsForm

idaapi = importlib.import_module("idaapi")


class RikuganPanel(idaapi.PluginForm):
    """IDA dockable form embedding the shared panel core widget."""

    def __init__(self):
        super().__init__()
        self._form_widget: QWidget | None = None
        self._root: QWidget | None = None
        self._core: RikuganPanelCore | None = None

    def OnCreate(self, form: Any) -> None:
        try:
            self._form_widget = self.FormToPyQtWidget(form)
        except Exception:
            self._form_widget = self.FormToPySideWidget(form)

        self._root = QWidget()
        form_layout = QVBoxLayout(self._form_widget)
        form_layout.setContentsMargins(0, 0, 0, 0)
        form_layout.addWidget(self._root)

        root_layout = QVBoxLayout(self._root)
        root_layout.setContentsMargins(0, 0, 0, 0)
        self._core = RikuganPanelCore(
            controller_factory=IdaSessionController,
            ui_hooks_factory=lambda panel_getter: RikuganUIHooks(panel_getter=panel_getter),
            tools_form_factory=lambda tools_widget: RikuganToolsForm(tools_widget),
            parent=self._root,
        )
        root_layout.addWidget(self._core)

    def OnClose(self, form):
        self.shutdown()
        if self._root is not None:
            self._root.setParent(None)
            self._root = None

    def show(self):
        return self.Show(
            "Rikugan",
            options=(idaapi.PluginForm.WOPN_TAB | idaapi.PluginForm.WOPN_PERSIST),
        )

    def close(self):
        self.Close(0)

    def shutdown(self) -> None:
        if self._core is not None:
            self._core.shutdown()
            self._core.setParent(None)
            self._core = None

    def prefill_input(self, text: str, auto_submit: bool = False) -> None:
        if self._core is not None:
            self._core.prefill_input(text, auto_submit=auto_submit)

    def on_database_changed(self, new_path: str) -> None:
        if self._core is not None:
            self._core.on_database_changed(new_path)

    def __getattr__(self, name: str):
        # Forward UI action accessors like _input_area / _on_submit.
        core = object.__getattribute__(self, "_core")
        if core is not None and hasattr(core, name):
            return getattr(core, name)
        raise AttributeError(name)

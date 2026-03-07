"""Context bar: shows current address, function, model, and token count."""

from __future__ import annotations

import importlib
from typing import Optional

from .qt_compat import (
    QFrame, QHBoxLayout, QLabel, QWidget, QTimer,
)
from ..core.host import (
    get_binary_ninja_view,
    get_current_address,
    is_binary_ninja,
    is_ida,
)
from ..core.logging import log_debug

if is_ida():
    try:
        ida_funcs = importlib.import_module("ida_funcs")
        ida_name = importlib.import_module("ida_name")
    except ImportError:
        ida_funcs = ida_name = None  # type: ignore[assignment]  # noqa: N816
else:
    ida_funcs = ida_name = None  # type: ignore[assignment]  # noqa: N816


def _function_name_at(ea: int) -> Optional[str]:
    if is_ida() and ida_funcs is not None and ida_name is not None:
        try:
            func = ida_funcs.get_func(ea)
            if func:
                return ida_name.get_name(func.start_ea)
        except Exception:
            return None

    if is_binary_ninja():
        bv = get_binary_ninja_view()
        if bv is None:
            return None
        try:
            get_func_at = getattr(bv, "get_function_at", None)
            if callable(get_func_at):
                func = get_func_at(ea)
                if func is not None:
                    return getattr(func, "name", None)
            get_containing = getattr(bv, "get_functions_containing", None)
            if callable(get_containing):
                funcs = list(get_containing(ea))
                if funcs:
                    return getattr(funcs[0], "name", None)
        except Exception:
            return None

    return None


class ContextBar(QFrame):
    """Status bar showing current binary context and session info."""

    def __init__(self, parent: QWidget = None):
        super().__init__(parent)
        self.setObjectName("context_bar")
        self.setFixedHeight(28)

        layout = QHBoxLayout(self)
        layout.setContentsMargins(8, 2, 8, 2)
        layout.setSpacing(16)

        self._address_label = self._make_pair("Addr:", "\u2014")
        self._function_label = self._make_pair("Func:", "\u2014")
        self._model_label = self._make_pair("Model:", "\u2014")
        self._tokens_label = self._make_pair("Tokens:", "0")

        for label, value in (self._address_label, self._function_label,
                             self._model_label, self._tokens_label):
            layout.addWidget(label)
            layout.addWidget(value)

        layout.addStretch()

        self._stopped = False

        # Auto-update cursor position
        self._timer = QTimer(self)
        self._timer.timeout.connect(self._update_cursor)
        self._timer.start(2000)

    def _make_pair(self, label_text: str, initial: str):
        label = QLabel(label_text)
        label.setObjectName("context_label")
        value = QLabel(initial)
        value.setObjectName("context_value")
        return label, value

    def stop(self) -> None:
        """Stop the auto-update timer. Call before destruction."""
        self._stopped = True
        try:
            self._timer.stop()
            self._timer.timeout.disconnect(self._update_cursor)
        except (RuntimeError, TypeError) as e:
            log_debug(f"ContextBar.stop: timer already destroyed: {e}")

    def set_address(self, addr: str) -> None:
        self._address_label[1].setText(addr)

    def set_function(self, name: str) -> None:
        self._function_label[1].setText(name if len(name) < 30 else name[:27] + "...")

    def set_model(self, model: str) -> None:
        self._model_label[1].setText(model)

    def set_tokens(self, count: int, context_window: int = 0) -> None:
        if count >= 1000:
            text = f"{count / 1000:.1f}k"
        else:
            text = str(count)
        if context_window > 0:
            pct = min(int(count * 100 / context_window), 100)
            text += f" ({pct}%)"
        self._tokens_label[1].setText(text)

    def _update_cursor(self) -> None:
        if self._stopped:
            return
        try:
            ea = get_current_address()
            if ea is None:
                return
            self.set_address(f"0x{int(ea):x}")
            name = _function_name_at(int(ea))
            self.set_function(name or "\u2014")
        except Exception as e:
            log_debug(f"ContextBar._update_cursor failed: {e}")

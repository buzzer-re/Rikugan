"""Context bar: cursor position on the left, token pressure and model on the right."""

from __future__ import annotations

import importlib
import re

from ..core.host import (
    get_binary_ninja_view,
    get_current_address,
    is_binary_ninja,
    is_ida,
)
from ..core.logging import log_debug
from .qt_compat import (
    QFrame,
    QHBoxLayout,
    QLabel,
    QTimer,
    QWidget,
)
from .styles import build_context_bar_stylesheet

if is_ida():
    try:
        ida_funcs = importlib.import_module("ida_funcs")
        ida_name = importlib.import_module("ida_name")
    except ImportError:
        ida_funcs = ida_name = None  # type: ignore[assignment]
else:
    ida_funcs = ida_name = None  # type: ignore[assignment]

_EM_DASH = "—"
_DOT = "●"
_MIN_BAR_HEIGHT = 26
_MAX_FUNCTION_CHARS = 22
_MAX_MODEL_CHARS = 18

_DATE_SUFFIX_RE = re.compile(r"[-@]\d{6,8}$")
_VERSION_TAIL_RE = re.compile(r"(\d+)-(\d+)$")
_MODEL_PREFIXES = ("claude-", "models/", "anthropic/", "anthropic.", "openai/")


def shorten_model_name(model: str) -> str:
    """Compact a provider model id for the one-line context bar.

    ``claude-sonnet-4-5-20250929`` becomes ``sonnet-4.5``; anything still too
    long for the bar is elided from the left so the version stays visible.
    """
    name = (model or "").strip()
    if not name:
        return _EM_DASH
    name = _DATE_SUFFIX_RE.sub("", name)
    lowered = name.lower()
    for prefix in _MODEL_PREFIXES:
        if lowered.startswith(prefix):
            name = name[len(prefix) :]
            break
    name = _VERSION_TAIL_RE.sub(r"\1.\2", name)
    if len(name) > _MAX_MODEL_CHARS:
        name = "…" + name[-(_MAX_MODEL_CHARS - 1) :]
    return name


def format_token_count(count: int, context_window: int = 0) -> str:
    """Render token usage as ``24.1k (12%)``."""
    if count >= 1000:
        text = f"{count / 1000:.1f}k"
    else:
        text = str(count)
    if context_window > 0:
        pct = min(int(count * 100 / context_window), 100)
        text += f" ({pct}%)"
    return text


def elide_middle(text: str, limit: int) -> str:
    """Shorten *text* to *limit* characters, keeping both ends readable."""
    if len(text) <= limit or limit < 5:
        return text
    head = (limit - 1) // 2
    tail = limit - 1 - head
    return f"{text[:head]}…{text[-tail:]}"


def _function_name_at(ea: int) -> str | None:
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
    """Status bar: ``[dot] addr | function`` on the left, ``tokens | model`` right.

    The labels carry no ``Addr:``/``Func:`` captions — at sidebar width those
    words cost more room than the values they describe.
    """

    def __init__(self, parent: QWidget = None):
        super().__init__(parent)
        self.setObjectName("context_bar")
        self.setStyleSheet(build_context_bar_stylesheet(self))
        # A fixed height clips descenders and underscores once the host font is
        # larger than the one it was picked for.
        self.setFixedHeight(max(_MIN_BAR_HEIGHT, self.fontMetrics().height() + 8))

        layout = QHBoxLayout(self)
        layout.setContentsMargins(10, 0, 10, 0)
        layout.setSpacing(0)

        self._dot = QLabel(_DOT, self)
        self._dot.setObjectName("context_dot")
        self._dot.setProperty("state", "idle")
        layout.addWidget(self._dot)
        layout.addSpacing(6)

        self._address_label = self._make_value(_EM_DASH)
        layout.addWidget(self._address_label)
        layout.addWidget(self._make_separator())
        self._function_label = self._make_label(_EM_DASH)
        layout.addWidget(self._function_label, 1)

        layout.addSpacing(8)
        self._tokens_label = self._make_label("0")
        layout.addWidget(self._tokens_label)
        layout.addWidget(self._make_separator())
        self._model_label = self._make_value(_EM_DASH)
        layout.addWidget(self._model_label)

        self._stopped = False

        # Auto-update cursor position
        self._timer = QTimer(self)
        self._timer.timeout.connect(self._update_cursor)
        self._timer.start(2000)

    def _make_value(self, initial: str) -> QLabel:
        label = QLabel(initial, self)
        label.setObjectName("context_value")
        return label

    def _make_label(self, initial: str) -> QLabel:
        label = QLabel(initial, self)
        label.setObjectName("context_label")
        return label

    def _make_separator(self) -> QLabel:
        label = QLabel("│", self)
        label.setObjectName("context_separator")
        label.setContentsMargins(7, 0, 7, 0)
        return label

    def stop(self) -> None:
        """Stop the auto-update timer. Call before destruction."""
        self._stopped = True
        try:
            self._timer.stop()
            self._timer.timeout.disconnect(self._update_cursor)
        except (RuntimeError, TypeError) as e:
            log_debug(f"ContextBar.stop: timer already destroyed: {e}")

    def set_address(self, addr: str) -> None:
        self._address_label.setText(addr)

    def set_function(self, name: str) -> None:
        self._function_label.setText(elide_middle(name, _MAX_FUNCTION_CHARS))
        self._function_label.setToolTip(name)

    def set_model(self, model: str) -> None:
        self._model_label.setText(shorten_model_name(model))
        self._model_label.setToolTip(model)

    def set_tokens(self, count: int, context_window: int = 0) -> None:
        self._tokens_label.setText(format_token_count(count, context_window))

    def set_state(self, state: str) -> None:
        """Color the status dot: ``idle``, ``running``, or ``error``."""
        if self._dot.property("state") == state:
            return
        self._dot.setProperty("state", state)
        self._dot.style().unpolish(self._dot)
        self._dot.style().polish(self._dot)

    def _update_cursor(self) -> None:
        if self._stopped:
            return
        try:
            ea = get_current_address()
            if ea is None:
                return
            self.set_address(f"0x{int(ea):x}")
            name = _function_name_at(int(ea))
            self.set_function(name or _EM_DASH)
        except Exception as e:
            log_debug(f"ContextBar._update_cursor failed: {e}")

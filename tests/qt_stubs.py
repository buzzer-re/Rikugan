"""Shared PySide6 stub injection for UI tests.

Must be called BEFORE importing any rikugan.ui module. Example::

    from tests.qt_stubs import ensure_pyside6_stubs
    ensure_pyside6_stubs()
    from rikugan.ui.some_module import ...
"""

from __future__ import annotations

import sys
import types

_installed = False


def _qt_class(name: str) -> type:
    """Create a minimal stubbed Qt class that supports subclassing."""
    return type(name, (), {"__init__": lambda self, *a, **k: None})


class _Signal:
    """Minimal Signal stub that acts as a descriptor."""

    def __init__(self, *a):
        pass

    def connect(self, *a):
        pass

    def disconnect(self, *a):
        pass

    def emit(self, *a):
        pass

    def __get__(self, obj, objtype=None):
        return self


_WIDGET_NAMES = [
    "QAbstractItemView",
    "QApplication",
    "QCheckBox",
    "QComboBox",
    "QDialog",
    "QDialogButtonBox",
    "QDoubleSpinBox",
    "QFileDialog",
    "QFormLayout",
    "QFrame",
    "QGroupBox",
    "QHBoxLayout",
    "QHeaderView",
    "QLabel",
    "QLineEdit",
    "QListWidget",
    "QListWidgetItem",
    "QMenu",
    "QMessageBox",
    "QPlainTextEdit",
    "QProgressBar",
    "QPushButton",
    "QRadioButton",
    "QScrollArea",
    "QSizePolicy",
    "QSpinBox",
    "QSplitter",
    "QStackedWidget",
    "QTabBar",
    "QTableWidget",
    "QTableWidgetItem",
    "QTabWidget",
    "QTextEdit",
    "QToolButton",
    "QTreeWidget",
    "QTreeWidgetItem",
    "QVBoxLayout",
    "QWidget",
]

_GUI_NAMES = [
    "QColor",
    "QFont",
    "QIntValidator",
    "QSyntaxHighlighter",
    "QTextCharFormat",
]


def _stub_mod(name: str, **attrs) -> types.ModuleType:
    m = types.ModuleType(name)
    m.__dict__.update(attrs)
    return m


def ensure_pyside6_stubs() -> None:
    """Install minimal PySide6 stubs into sys.modules (idempotent)."""
    global _installed
    if _installed:
        return
    _installed = True

    _sentinel = type("_Qt", (), {})()

    sys.modules.setdefault("PySide6", _stub_mod("PySide6"))
    sys.modules.setdefault(
        "PySide6.QtCore",
        _stub_mod(
            "PySide6.QtCore",
            Signal=_Signal,
            Qt=_sentinel,
            QObject=_qt_class("QObject"),
            QTimer=_qt_class("QTimer"),
        ),
    )
    sys.modules.setdefault(
        "PySide6.QtWidgets",
        _stub_mod(
            "PySide6.QtWidgets",
            **{n: _qt_class(n) for n in _WIDGET_NAMES},
        ),
    )
    sys.modules.setdefault(
        "PySide6.QtGui",
        _stub_mod(
            "PySide6.QtGui",
            **{n: _qt_class(n) for n in _GUI_NAMES},
        ),
    )

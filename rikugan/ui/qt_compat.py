"""Qt compatibility layer for Rikugan.

IDA 9.x 64-bit and Binary Ninja ship PySide6 (Qt6).  IDA 9.1 32-bit on
Windows still uses Qt5 — its process has Qt5Core.dll loaded.  Importing
PySide6 in that environment loads Qt6 DLLs alongside Qt5, which triggers a
``FAST_FAIL_FATAL_APP_EXIT`` crash inside ``QWidgetPrivate::QWidgetPrivate``
(Qt6 widget constructor detects it is not running in a Qt6 QApplication).

Detection order:
1. Check ``sys.modules`` for an already-loaded binding (fast, cross-platform).
2. On Windows, check if ``Qt5Core.dll`` is loaded in the process — if so, the
   host is Qt5-based and we must avoid loading PySide6.
3. Default: try PySide6, fall back to PyQt5.
"""

from __future__ import annotations

import sys


def _detect_binding() -> str:
    """Return ``"PySide6"`` or ``"PyQt5"`` based on the host environment."""

    # Fast path: a binding is already imported by the host.
    has_pyside6 = any(k.startswith("PySide6.") for k in sys.modules)
    has_pyqt5 = any(k.startswith("PyQt5.") for k in sys.modules)

    if has_pyside6 and not has_pyqt5:
        return "PySide6"
    if has_pyqt5 and not has_pyside6:
        return "PyQt5"

    # On Windows, check whether Qt5 DLLs are already loaded by the host
    # process *before* importing anything that would pull in Qt6.
    if sys.platform == "win32":
        try:
            import ctypes

            kernel32 = ctypes.windll.kernel32  # type: ignore[attr-defined]
            kernel32.GetModuleHandleW.restype = ctypes.c_void_p
            if kernel32.GetModuleHandleW("Qt5Core.dll"):
                return "PyQt5"
        except Exception:
            pass

    # Default: prefer PySide6, fall back to PyQt5.
    try:
        import PySide6  # noqa: F401

        return "PySide6"
    except ImportError:
        return "PyQt5"


QT_BINDING: str = _detect_binding()

# ---------------------------------------------------------------------------
# Import the chosen binding, aliasing PyQt5 names to match PySide6 API.
# ---------------------------------------------------------------------------

if QT_BINDING == "PySide6":
    from PySide6.QtCore import QObject, Qt, QTimer, Signal
    from PySide6.QtGui import (
        QColor,
        QFont,
        QIntValidator,
        QSyntaxHighlighter,
        QTextCharFormat,
    )
    from PySide6.QtWidgets import (
        QAbstractItemView,
        QApplication,
        QCheckBox,
        QComboBox,
        QDialog,
        QDialogButtonBox,
        QDoubleSpinBox,
        QFileDialog,
        QFormLayout,
        QFrame,
        QGroupBox,
        QHBoxLayout,
        QHeaderView,
        QLabel,
        QLineEdit,
        QListWidget,
        QListWidgetItem,
        QMenu,
        QMessageBox,
        QPlainTextEdit,
        QProgressBar,
        QPushButton,
        QRadioButton,
        QScrollArea,
        QSizePolicy,
        QSpinBox,
        QSplitter,
        QTabBar,
        QTableWidget,
        QTableWidgetItem,
        QTabWidget,
        QTextEdit,
        QToolButton,
        QTreeWidget,
        QTreeWidgetItem,
        QVBoxLayout,
        QWidget,
    )
else:
    from PyQt5.QtCore import QObject, Qt, QTimer  # noqa: F401
    from PyQt5.QtCore import pyqtSignal as Signal  # noqa: F401
    from PyQt5.QtGui import (  # noqa: F401
        QColor,
        QFont,
        QIntValidator,
        QSyntaxHighlighter,
        QTextCharFormat,
    )
    from PyQt5.QtWidgets import (  # noqa: F401
        QAbstractItemView,
        QApplication,
        QCheckBox,
        QComboBox,
        QDialog,
        QDialogButtonBox,
        QDoubleSpinBox,
        QFileDialog,
        QFormLayout,
        QFrame,
        QGroupBox,
        QHBoxLayout,
        QHeaderView,
        QLabel,
        QLineEdit,
        QListWidget,
        QListWidgetItem,
        QMenu,
        QMessageBox,
        QPlainTextEdit,
        QProgressBar,
        QPushButton,
        QRadioButton,
        QScrollArea,
        QSizePolicy,
        QSpinBox,
        QSplitter,
        QTabBar,
        QTableWidget,
        QTableWidgetItem,
        QTabWidget,
        QTextEdit,
        QToolButton,
        QTreeWidget,
        QTreeWidgetItem,
        QVBoxLayout,
        QWidget,
    )


def is_pyside6() -> bool:
    return QT_BINDING == "PySide6"

"""Qt compatibility layer for Rikugan.

IDA 9+ ships PySide6 exclusively (its ``PyQt5`` module is a thin shim over
PySide6, not a separate binding).  We import from PySide6 directly to
minimize Shiboken type-wrapper initialization and reduce the crash surface
on Python 3.14.
"""

from __future__ import annotations

from PySide6.QtCore import QObject, Qt, QTimer, Signal  # noqa: F401
from PySide6.QtGui import (  # noqa: F401
    QColor,
    QFont,
    QIntValidator,
    QSyntaxHighlighter,
    QTextCharFormat,
)
from PySide6.QtWidgets import (  # noqa: F401
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

QT_BINDING = "PySide6"


def is_pyside6() -> bool:
    return True

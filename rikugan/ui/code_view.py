"""Read-only code display: syntax colours, a gutter, and diff lines.

The scanner is separate from Qt so its behaviour can be tested directly. It reads
each line left to right with one pattern, so whichever token starts first wins: a
# inside a string stays string, and a quote inside a comment stays comment.
"""

from __future__ import annotations

import os
import re
from dataclasses import dataclass

from .qt_compat import (
    QColor,
    QFont,
    QPainter,
    QPlainTextEdit,
    QRect,
    QSize,
    QSyntaxHighlighter,
    Qt,
    QTextBlockFormat,
    QTextCharFormat,
    QTextCursor,
    QWidget,
)
from .styles import blend_theme_color, ensure_contrast, get_chat_color_tokens

LANGUAGE_PLAIN = ""
LANGUAGE_PYTHON = "python"
LANGUAGE_C = "c"
LANGUAGE_SHELL = "shell"
LANGUAGE_JSON = "json"
LANGUAGE_CONFIG = "config"

MARKER_REMOVED = "-"
MARKER_ADDED = "+"
MARKER_CONTEXT = " "

_DEFAULT_VISIBLE_LINES = 20
_TAB_SPACES = 4
_GUTTER_PADDING = 8
_SCROLLBAR_RESERVE = 10
_HEIGHT_SLACK = 4

_REMOVED_TINT = "#f44747"
_ADDED_TINT = "#2ea043"
_TINT_AMOUNT = 0.20

_EXTENSIONS = {
    LANGUAGE_PYTHON: (".py", ".pyw", ".pyi", ".idc.py"),
    LANGUAGE_C: (
        ".c",
        ".h",
        ".cc",
        ".cpp",
        ".cxx",
        ".hpp",
        ".hh",
        ".cs",
        ".java",
        ".js",
        ".jsx",
        ".ts",
        ".tsx",
        ".go",
        ".rs",
        ".idc",
        ".kt",
        ".swift",
    ),
    LANGUAGE_SHELL: (".sh", ".bash", ".zsh", ".ps1", ".psm1"),
    LANGUAGE_JSON: (".json", ".jsonc"),
    LANGUAGE_CONFIG: (".yaml", ".yml", ".toml", ".ini", ".cfg", ".conf"),
}
_LANGUAGE_BY_EXTENSION = {ext: language for language, exts in _EXTENSIONS.items() for ext in exts}


def language_for_path(path: str) -> str:
    return _LANGUAGE_BY_EXTENSION.get(os.path.splitext(path)[1].lower(), LANGUAGE_PLAIN)


def _words(*words: str) -> str:
    return r"\b(?:" + "|".join(words) + r")\b"


_NUMBER = r"\b(?:0[xX][0-9a-fA-F_]+|0[bB][01_]+|\d[\d_]*\.?\d*(?:[eE][+-]?\d+)?)\b"
_DOUBLE_QUOTED = r'"(?:[^"\\]|\\.)*"?'
_SINGLE_QUOTED = r"'(?:[^'\\]|\\.)*'?"

# Each is an ordered alternation: earlier groups win a tie at the same position.
# "open_N" groups start a token that can run across lines; _MULTILINE_ENDS has
# what ends it.
_PATTERNS = {
    LANGUAGE_PYTHON: (
        r"(?P<comment>#.*)"
        r'|(?P<open_0>[bBrRfFuU]{0,2}""")'
        r"|(?P<open_1>[bBrRfFuU]{0,2}''')"
        rf"|(?P<string>[bBrRfFuU]{{0,2}}(?:{_DOUBLE_QUOTED}|{_SINGLE_QUOTED}))"
        rf"|(?P<number>{_NUMBER})"
        r"|(?P<meta>^\s*@[\w.]+)"
        + "|(?P<keyword>"
        + _words(
            "and",
            "as",
            "assert",
            "async",
            "await",
            "break",
            "class",
            "continue",
            "def",
            "del",
            "elif",
            "else",
            "except",
            "finally",
            "for",
            "from",
            "global",
            "if",
            "import",
            "in",
            "is",
            "lambda",
            "nonlocal",
            "not",
            "or",
            "pass",
            "raise",
            "return",
            "try",
            "while",
            "with",
            "yield",
        )
        + ")|(?P<builtin>"
        + _words(
            "print",
            "len",
            "range",
            "int",
            "str",
            "bytes",
            "list",
            "dict",
            "set",
            "tuple",
            "hex",
            "ord",
            "chr",
            "type",
            "isinstance",
            "enumerate",
            "zip",
            "map",
            "filter",
            "sorted",
            "open",
            "True",
            "False",
            "None",
            "self",
        )
        + ")"
    ),
    LANGUAGE_C: (
        r"(?P<comment>//.*)"
        r"|(?P<open_0>/\*)"
        rf"|(?P<string>{_DOUBLE_QUOTED}|{_SINGLE_QUOTED})"
        r"|(?P<meta>^\s*#\s*\w+)"
        rf"|(?P<number>{_NUMBER})"
        + "|(?P<keyword>"
        + _words(
            "auto",
            "break",
            "case",
            "catch",
            "class",
            "const",
            "continue",
            "default",
            "delete",
            "do",
            "else",
            "enum",
            "export",
            "extends",
            "extern",
            "fn",
            "for",
            "func",
            "function",
            "goto",
            "if",
            "impl",
            "import",
            "interface",
            "let",
            "match",
            "mut",
            "namespace",
            "new",
            "package",
            "private",
            "protected",
            "public",
            "return",
            "sizeof",
            "static",
            "struct",
            "switch",
            "template",
            "this",
            "throw",
            "trait",
            "try",
            "typedef",
            "union",
            "use",
            "using",
            "var",
            "virtual",
            "volatile",
            "while",
        )
        + ")|(?P<builtin>"
        + _words(
            "void",
            "char",
            "short",
            "int",
            "long",
            "float",
            "double",
            "bool",
            "signed",
            "unsigned",
            "size_t",
            "uint8_t",
            "uint16_t",
            "uint32_t",
            "uint64_t",
            "int8_t",
            "int16_t",
            "int32_t",
            "int64_t",
            "true",
            "false",
            "null",
            "nullptr",
            "NULL",
        )
        + ")"
    ),
    LANGUAGE_SHELL: (
        r"(?P<comment>(?:^|(?<=\s))#.*)"
        rf"|(?P<string>{_DOUBLE_QUOTED}|{_SINGLE_QUOTED})"
        r"|(?P<meta>\$\{?\w+\}?)"
        rf"|(?P<number>{_NUMBER})"
        + "|(?P<keyword>"
        + _words(
            "if",
            "then",
            "else",
            "elif",
            "fi",
            "for",
            "while",
            "until",
            "do",
            "done",
            "case",
            "esac",
            "in",
            "function",
            "return",
            "exit",
        )
        + ")|(?P<builtin>"
        + _words(
            "cd", "echo", "export", "source", "sudo", "rm", "mv", "cp", "mkdir", "cat", "grep", "python3", "python"
        )
        + ")"
    ),
    LANGUAGE_JSON: (
        r'(?P<keyword>"(?:[^"\\]|\\.)*"(?=\s*:))'
        rf"|(?P<string>{_DOUBLE_QUOTED})"
        rf"|(?P<number>{_NUMBER})"
        r"|(?P<builtin>\b(?:true|false|null)\b)"
    ),
    LANGUAGE_CONFIG: (
        rf"(?P<comment>(?:^|(?<=\s))[#;].*)|(?P<string>{_DOUBLE_QUOTED}|{_SINGLE_QUOTED})|(?P<number>{_NUMBER})"
    ),
}
_COMPILED = {language: re.compile(pattern) for language, pattern in _PATTERNS.items()}

# For each language, the token an "open_N" group starts: what ends it, and its kind.
_MULTILINE_ENDS = {
    LANGUAGE_PYTHON: ((re.compile(r'"""'), "string"), (re.compile(r"'''"), "string")),
    LANGUAGE_C: ((re.compile(r"\*/"), "comment"),),
}

_COLORS = {
    "comment": "#6a9955",
    "string": "#ce9178",
    "number": "#b5cea8",
    "keyword": "#c586c0",
    "builtin": "#dcdcaa",
    "meta": "#9cdcfe",
}


@dataclass(frozen=True)
class Span:
    start: int
    length: int
    kind: str


def scan_line(language: str, text: str, state: int = -1) -> tuple[list[Span], int]:
    """Colour one line. *state* says which multi-line token it starts inside.

    Returns the spans and the state for the next line, -1 meaning none.
    """
    pattern = _COMPILED.get(language)
    if pattern is None:
        return [], -1
    ends = _MULTILINE_ENDS.get(language, ())
    spans: list[Span] = []
    position = 0

    if 0 <= state < len(ends):
        end_pattern, kind = ends[state]
        closing = end_pattern.search(text)
        if closing is None:
            return [Span(0, len(text), kind)], state
        spans.append(Span(0, closing.end(), kind))
        position = closing.end()

    while position <= len(text):
        match = pattern.search(text, position)
        if match is None or match.end() == match.start():
            break
        group = match.lastgroup or ""
        if group.startswith("open_"):
            index = int(group.removeprefix("open_"))
            end_pattern, kind = ends[index]
            closing = end_pattern.search(text, match.end())
            if closing is None:
                spans.append(Span(match.start(), len(text) - match.start(), kind))
                return spans, index
            spans.append(Span(match.start(), closing.end() - match.start(), kind))
            position = closing.end()
            continue
        spans.append(Span(match.start(), match.end() - match.start(), group))
        position = match.end()
    return spans, -1


class _Highlighter(QSyntaxHighlighter):
    """Paints scan_line's spans, with colours kept readable on either theme."""

    def __init__(self, document, language: str, background: str):
        super().__init__(document)
        self._language = language
        self._formats = {kind: self._format(kind, color, background) for kind, color in _COLORS.items()}

    @staticmethod
    def _format(kind: str, color: str, background: str) -> QTextCharFormat:
        fmt = QTextCharFormat()
        # Code is monospace and set solid, so the large-text threshold is enough
        # and keeps the palette's hues apart.
        fmt.setForeground(QColor(ensure_contrast(color, background, 3.5)))
        if kind == "keyword":
            fmt.setFontWeight(QFont.Weight.Bold)
        if kind == "comment":
            fmt.setFontItalic(True)
        return fmt

    def highlightBlock(self, text: str) -> None:
        spans, state = scan_line(self._language, text, self.previousBlockState())
        for span in spans:
            self.setFormat(span.start, span.length, self._formats[span.kind])
        self.setCurrentBlockState(state)


class _Gutter(QWidget):
    def __init__(self, view: CodeView):
        super().__init__(view)
        self._view = view

    def sizeHint(self) -> QSize:
        return QSize(self._view.gutter_width(), 0)

    def paintEvent(self, event) -> None:
        self._view.paint_gutter(self, event)


class CodeView(QPlainTextEdit):
    """Read-only code. The gutter shows line numbers, or diff markers if given."""

    def __init__(
        self,
        text: str,
        language: str = LANGUAGE_PLAIN,
        *,
        markers: list[str] | None = None,
        line_numbers: bool = True,
        max_visible_lines: int = _DEFAULT_VISIBLE_LINES,
        parent: QWidget | None = None,
    ):
        super().__init__(parent)
        self.setObjectName("tool_code_view")
        self._markers = markers
        self._line_numbers = line_numbers or markers is not None
        tokens = get_chat_color_tokens(parent or self)
        self._background = tokens["code_bg"]
        self._gutter_ink = ensure_contrast(tokens["muted"], self._background, 4.5)
        # It owns its background in every host: left to IDA's native theme the
        # editor comes out white while the colours assume the chat's ground, and
        # this is where the user reads what is about to run.
        self.setStyleSheet(
            "QPlainTextEdit#tool_code_view { "
            f"  color: {tokens['text']}; background: {self._background}; "
            "  font-family: 'Consolas', 'Monaco', 'Courier New', monospace; font-size: 11px; "
            f"  border: 1px solid {tokens['border']}; border-radius: 4px; "
            "}"
            f"QScrollBar:vertical {{ width: 8px; background: {self._background}; }}"
            f"QScrollBar::handle:vertical {{ background: {tokens['border']}; border-radius: 4px; }}"
            f"QScrollBar:horizontal {{ height: 8px; background: {self._background}; }}"
            f"QScrollBar::handle:horizontal {{ background: {tokens['border']}; border-radius: 4px; }}"
        )
        self.setReadOnly(True)
        self.setLineWrapMode(QPlainTextEdit.LineWrapMode.NoWrap)
        self.setPlainText(text)
        self.setTabStopDistance(self.fontMetrics().horizontalAdvance(" ") * _TAB_SPACES)
        if markers is not None:
            self._tint_changed_lines(markers, tokens["code_bg"])

        self._highlighter = _Highlighter(self.document(), language, self._background) if language else None

        self._gutter = _Gutter(self) if self._line_numbers else None
        if self._gutter is not None:
            self.setViewportMargins(self.gutter_width(), 0, 0, 0)
            self.updateRequest.connect(self._on_update_request)

        lines = text.split("\n")
        self._fitted_height = self._height_for(lines, max_visible_lines)
        widest_line = max(lines, key=len).expandtabs(_TAB_SPACES)
        self._text_width = self.fontMetrics().horizontalAdvance(widest_line)
        self.setFixedHeight(self._fitted_height)

    def _height_for(self, lines: list[str], max_visible_lines: int) -> int:
        """Tall enough that a short block shows whole, with no scrollbar of its own."""
        chrome = round(2 * self.document().documentMargin()) + 2 * self.frameWidth() + _HEIGHT_SLACK
        return self.fontMetrics().lineSpacing() * min(len(lines), max_visible_lines) + chrome

    def _tint_changed_lines(self, markers: list[str], background: str) -> None:
        tints = {
            MARKER_REMOVED: QColor(blend_theme_color(background, _REMOVED_TINT, _TINT_AMOUNT)),
            MARKER_ADDED: QColor(blend_theme_color(background, _ADDED_TINT, _TINT_AMOUNT)),
        }
        block = self.document().firstBlock()
        for marker in markers:
            tint = tints.get(marker)
            if tint is not None and block.isValid():
                fmt = QTextBlockFormat()
                fmt.setBackground(tint)
                QTextCursor(block).setBlockFormat(fmt)
            block = block.next()

    def gutter_width(self) -> int:
        digits = 1 if self._markers is not None else len(str(max(1, self.blockCount())))
        return self.fontMetrics().horizontalAdvance("9") * digits + 2 * _GUTTER_PADDING

    def _on_update_request(self, rect, dy: int) -> None:
        gutter = self._gutter
        if gutter is None:
            return
        if dy:
            gutter.scroll(0, dy)
        else:
            gutter.update(0, rect.y(), gutter.width(), rect.height())

    def resizeEvent(self, event) -> None:
        super().resizeEvent(event)
        # Whether a line overflows depends on the width, known only now. Without
        # the extra room the horizontal scrollbar would cover the last line.
        margins = round(2 * self.document().documentMargin())
        overflows = self._text_width + margins > self.viewport().width()
        self.setFixedHeight(self._fitted_height + (_SCROLLBAR_RESERVE if overflows else 0))
        if self._gutter is not None:
            area = self.contentsRect()
            self._gutter.setGeometry(QRect(area.left(), area.top(), self.gutter_width(), area.height()))

    def paint_gutter(self, gutter: QWidget, event) -> None:
        painter = QPainter(gutter)
        painter.fillRect(event.rect(), QColor(self._background))
        painter.setPen(QColor(self._gutter_ink))
        block = self.firstVisibleBlock()
        number = block.blockNumber()
        top = round(self.blockBoundingGeometry(block).translated(self.contentOffset()).top())
        height = round(self.blockBoundingRect(block).height())
        width = gutter.width() - _GUTTER_PADDING
        while block.isValid() and top <= event.rect().bottom():
            if block.isVisible():
                label = str(number + 1) if self._markers is None else self._markers[number]
                painter.drawText(0, top, width, height, Qt.AlignmentFlag.AlignRight, label)
            block = block.next()
            number += 1
            top += height
        painter.end()

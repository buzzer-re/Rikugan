"""What a tool call is about to do, laid out for a person to review.

A tool's arguments arrive as JSON, which is the wrong way to read code: a script
or a file is one long string full of \\n. This turns them into what they mean, in
the expanded tool card and on the approval card alike.
"""

from __future__ import annotations

import difflib
import json
from dataclasses import dataclass, field

from .code_view import (
    LANGUAGE_PLAIN,
    LANGUAGE_PYTHON,
    LANGUAGE_SHELL,
    MARKER_ADDED,
    MARKER_CONTEXT,
    MARKER_REMOVED,
    CodeView,
    language_for_path,
)
from .qt_compat import QLabel, Qt, QVBoxLayout, QWidget, qt_flags
from .styles import ensure_contrast, get_chat_color_tokens

# Enough for anything a person would read here. write_file allows far more, and
# past this the review says it is partial rather than freezing the panel.
MAX_REVIEW_CHARS = 200_000
# Comparing lines is quadratic. Past this a diff is just old, then new.
_MAX_DIFF_LINES = 2_000
# Where the tool card's one-line summary starts to shorten what it shows.
_CARD_SUMMARY_CHARS = 80


@dataclass(frozen=True)
class ToolReview:
    heading: str = ""
    detail: str = ""
    code: str = ""
    language: str = LANGUAGE_PLAIN
    markers: list[str] | None = field(default=None)
    # Characters of the real text not shown, when it is too long to show.
    omitted_chars: int = 0


def _heading_for(text: str) -> str:
    """The card's header row already shows this, unless it had to shorten it."""
    return text if len(text) > _CARD_SUMMARY_CHARS else ""


def _count_lines(text: str) -> int:
    return len(text.split("\n")) - (1 if text.endswith("\n") else 0)


def _plural(count: int, noun: str) -> str:
    return f"{count:,} {noun}" + ("" if count == 1 else "s")


def _bounded(text: str) -> tuple[str, int]:
    if len(text) <= MAX_REVIEW_CHARS:
        return text.removesuffix("\n"), 0
    return text[:MAX_REVIEW_CHARS], len(text) - MAX_REVIEW_CHARS


def diff_lines(old: str, new: str) -> tuple[str, list[str]]:
    """Line-level diff of a replacement, returned as (text, gutter marker per line)."""
    old_lines, new_lines = old.split("\n"), new.split("\n")
    if len(old_lines) + len(new_lines) > _MAX_DIFF_LINES:
        rows = [(MARKER_REMOVED, line) for line in old_lines] + [(MARKER_ADDED, line) for line in new_lines]
    else:
        rows = []
        matcher = difflib.SequenceMatcher(None, old_lines, new_lines, autojunk=False)
        for tag, old_start, old_end, new_start, new_end in matcher.get_opcodes():
            if tag == "equal":
                rows += [(MARKER_CONTEXT, line) for line in old_lines[old_start:old_end]]
                continue
            rows += [(MARKER_REMOVED, line) for line in old_lines[old_start:old_end]]
            rows += [(MARKER_ADDED, line) for line in new_lines[new_start:new_end]]
    return "\n".join(line for _, line in rows), [marker for marker, _ in rows]


def _review_write_file(args: dict) -> ToolReview:
    path = str(args.get("file_path", ""))
    content = str(args.get("content", ""))
    code, omitted = _bounded(content)
    return ToolReview(
        heading=_heading_for(path),
        detail=_plural(_count_lines(content), "line") if content else "empty file",
        code=code,
        language=language_for_path(path),
        omitted_chars=omitted,
    )


def _review_edit_file(args: dict) -> ToolReview:
    path = str(args.get("file_path", ""))
    old, new = str(args.get("old_string", "")), str(args.get("new_string", ""))
    omitted = max(0, len(old) + len(new) - MAX_REVIEW_CHARS)
    if omitted:
        old, new = old[: MAX_REVIEW_CHARS // 2], new[: MAX_REVIEW_CHARS // 2]
    code, markers = diff_lines(old, new)
    removed, added = markers.count(MARKER_REMOVED), markers.count(MARKER_ADDED)
    detail = f"-{removed} +{added}" + (" · every occurrence" if args.get("replace_all") else "")
    return ToolReview(
        heading=_heading_for(path),
        detail=detail,
        code=code,
        language=language_for_path(path),
        markers=markers,
        omitted_chars=omitted,
    )


def _review_execute_python(args: dict) -> ToolReview:
    code, omitted = _bounded(str(args.get("code", args.get("script", ""))).strip("\n"))
    return ToolReview(code=code, language=LANGUAGE_PYTHON, omitted_chars=omitted)


def _review_run_shell(args: dict) -> ToolReview:
    code, omitted = _bounded(str(args.get("command", "")))
    facts = []
    if args.get("working_dir"):
        facts.append(f"in {args['working_dir']}")
    if args.get("timeout"):
        facts.append(f"timeout {args['timeout']}s")
    return ToolReview(detail=" · ".join(facts), code=code, language=LANGUAGE_SHELL, omitted_chars=omitted)


def _review_read_file(args: dict) -> ToolReview:
    facts = []
    if args.get("offset") not in (None, 1):
        facts.append(f"from line {args['offset']}")
    if args.get("limit"):
        facts.append(f"{_plural(int(args['limit']), 'line')} at most")
    return ToolReview(heading=_heading_for(str(args.get("file_path", ""))), detail=" · ".join(facts))


def _review_search_files(args: dict) -> ToolReview:
    facts = [f"in {args.get('path') or 'the workspace'}"]
    if args.get("glob"):
        facts.append(f"matching {args['glob']}")
    if args.get("mode") not in (None, "content"):
        facts.append(str(args["mode"]))
    if args.get("case_insensitive"):
        facts.append("ignoring case")
    if args.get("context_lines"):
        facts.append(f"{args['context_lines']} lines of context")
    return ToolReview(heading=_heading_for(str(args.get("pattern", ""))), detail=" · ".join(facts))


_BUILDERS = {
    "write_file": _review_write_file,
    "edit_file": _review_edit_file,
    "execute_python": _review_execute_python,
    "run_shell": _review_run_shell,
    "read_file": _review_read_file,
    "search_files": _review_search_files,
}


def build_review(tool_name: str, args_text: str) -> ToolReview | None:
    """The review of a call, or None for a tool with nothing special to show."""
    builder = _BUILDERS.get(tool_name)
    if builder is None:
        return None
    try:
        args = json.loads(args_text) if args_text else {}
    except (json.JSONDecodeError, TypeError):
        return None
    if not isinstance(args, dict):
        return None
    try:
        return builder(args)
    except (TypeError, ValueError):  # a malformed argument from the model
        return None


def result_language(tool_name: str, args_text: str) -> str:
    """How to colour a tool's result: file contents by extension, others plainly."""
    if tool_name not in ("read_file", "edit_file"):
        return LANGUAGE_PLAIN
    try:
        return language_for_path(str(json.loads(args_text).get("file_path", "")))
    except (json.JSONDecodeError, TypeError, AttributeError):
        return LANGUAGE_PLAIN


class ToolReviewView(QWidget):
    """A ToolReview as widgets. All text is plain: it can carry text from a binary."""

    def __init__(
        self,
        review: ToolReview,
        parent: QWidget | None = None,
        *,
        ink: str = "",
        muted: str = "",
    ):
        """*ink* and *muted* are for a surface other than the usual tool card."""
        super().__init__(parent)
        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(3)
        tokens = get_chat_color_tokens(parent or self)
        ink = ink or ensure_contrast(tokens["text"], tokens["tool_bg"], 7.0)
        muted = muted or ensure_contrast(tokens["muted"], tokens["tool_bg"], 4.5)

        if review.heading:
            layout.addWidget(self._label(review.heading, f"color: {ink}; font-weight: bold; font-size: 11px;"))
        if review.detail:
            layout.addWidget(self._label(review.detail, f"color: {muted}; font-size: 10px;"))
        if review.code:
            layout.addWidget(CodeView(review.code, review.language, markers=review.markers, parent=self))
        if review.omitted_chars:
            note = f"Only part of this is shown: {review.omitted_chars:,} more characters are not."
            layout.addWidget(self._label(note, f"color: {muted}; font-size: 10px; font-style: italic;"))

    @staticmethod
    def _label(text: str, style: str) -> QLabel:
        label = QLabel(text)
        label.setTextFormat(Qt.TextFormat.PlainText)
        label.setWordWrap(True)
        label.setStyleSheet(style)
        label.setTextInteractionFlags(
            qt_flags(
                Qt.TextInteractionFlag.TextSelectableByMouse,
                Qt.TextInteractionFlag.TextSelectableByKeyboard,
            )
        )
        return label

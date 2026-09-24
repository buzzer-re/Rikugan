"""Offscreen Qt checks for the chat panel's geometry and contrast.

Run as a script, not imported: the unit suite installs stub PySide6 modules in
``sys.modules``, so real Qt can only be exercised in a separate process. Prints
one ``FAIL: ...`` line per problem and exits non-zero if any were found.

    QT_QPA_PLATFORM=offscreen python tests/render/render_checks.py
"""

from __future__ import annotations

import os
import sys

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))

from PySide6.QtGui import QColor, QFont, QPalette  # noqa: E402
from PySide6.QtWidgets import QApplication, QVBoxLayout, QWidget  # noqa: E402

_FAILURES: list[str] = []

_PANEL_WIDTH = 420  # a Binary Ninja sidebar
_SLACK_TOLERANCE_PX = 6

_DARK_ROLES = {
    "Window": "#2b2b2b",
    "WindowText": "#d6d6d6",
    "Base": "#1f1f1f",
    "AlternateBase": "#323232",
    "Text": "#d6d6d6",
    "Button": "#3a3a3a",
    "ButtonText": "#d6d6d6",
    "Highlight": "#5a9fd4",
    "HighlightedText": "#ffffff",
    "Mid": "#6a6a6a",
    "Dark": "#1a1a1a",
    "Light": "#4a4a4a",
}

# A message with a wide, non-wrapping code block — the shape that showed a
# large empty band under the text.
_DIAGRAM = """Here is the real picture:

```
Main executable
 |-- _start / AppDelegate                  <- real code
 |-- NorthlightEngine_BootstrapAndMainLoop <- real code
 \\-- hundreds of stub trampolines (adrp+ldr+br through __got)
```

So the executable is essentially the loader."""

_LONG = "\n\n".join(f"Paragraph {i} carrying a sentence of ordinary prose." for i in range(12))
_REASONING_ONLY = "<think>I should call a tool now.</think>"


def _fail(message: str) -> None:
    _FAILURES.append(message)
    print(f"FAIL: {message}")


def _luminance(color: str) -> float:
    color = color.lstrip("#")
    r, g, b = (int(color[i : i + 2], 16) / 255.0 for i in (0, 2, 4))
    return 0.2126 * r + 0.7152 * g + 0.0722 * b


def _build_app() -> QApplication:
    app = QApplication([])
    app.setFont(QFont("DejaVu Sans", 13))
    palette = QPalette()
    for role, value in _DARK_ROLES.items():
        palette.setColor(getattr(QPalette, role), QColor(value))
    app.setPalette(palette)
    return app


def _check_bubble_heights(app, view) -> None:
    """A bubble must be exactly as tall as the text it holds."""
    from rikugan.ui.message_widgets import AssistantMessageWidget

    widgets = []
    for text in (_DIAGRAM, _LONG, "One short line."):
        widget = AssistantMessageWidget(parent=view._container)
        view._insert_widget(widget)
        app.processEvents()
        widget.set_text(text)
        app.processEvents()
        app.processEvents()
        widgets.append(widget)

    app.processEvents()
    for widget in widgets:
        label = widget._committed_label
        needed = label.measured_height(max(1, label.width()))
        slack = label.height() - needed
        if abs(slack) > _SLACK_TOLERANCE_PX:
            _fail(f"assistant bubble is {slack}px off its content height (blank band)")


def _check_reasoning_only_is_hidden(app, view) -> None:
    """A turn that is only <think> must not leave an empty grey box."""
    from rikugan.ui.message_widgets import AssistantMessageWidget

    widget = AssistantMessageWidget(parent=view._container)
    view._insert_widget(widget)
    app.processEvents()
    widget.set_text(_REASONING_ONLY)
    app.processEvents()
    if widget._bubble.isVisible():
        _fail("reasoning-only turn still shows an empty bubble")


def _check_body_text_contrast() -> None:
    """Body text must be readable on the bubble it sits on."""
    from rikugan.ui.message_widgets import _assistant_bubble_theme

    theme = _assistant_bubble_theme()
    gap = abs(_luminance(theme["text"]) - _luminance(theme["background"]))
    if gap < 0.35:
        _fail(f"assistant text contrast is only {gap:.2f} against its bubble")


def _check_question_renders_line_breaks(app, view) -> None:
    """The question widget must render markdown, not literal escapes."""
    from rikugan.ui.message_widgets import UserQuestionWidget

    question = "Rename these?\\n\\n1. sub_1000 -> main\\n2. sub_2000 -> init"
    widget = UserQuestionWidget(question, ["Yes", "No"], parent=view._container)
    view._insert_widget(widget)
    app.processEvents()
    rendered = widget._q_label.text()
    if "\\n" in rendered:
        _fail("question widget shows literal backslash-n instead of line breaks")
    if "<" not in rendered:
        _fail("question widget did not render markdown")


def _check_streamed_message_has_no_band(app, view) -> None:
    """Streaming a message must not make it taller than setting it at once."""
    from rikugan.ui.message_widgets import AssistantMessageWidget

    streamed = AssistantMessageWidget(parent=view._container)
    view._insert_widget(streamed)
    app.processEvents()
    for chunk in _LONG.split(" "):
        streamed.append_text(chunk + " ")
    for _ in range(400):  # drain the typewriter reveal
        streamed._reveal_tick()
        if streamed._displayed_len >= len(streamed._full_text):
            break
    app.processEvents()

    one_shot = AssistantMessageWidget(parent=view._container)
    view._insert_widget(one_shot)
    app.processEvents()
    one_shot.set_text(streamed.full_text())
    app.processEvents()
    app.processEvents()

    streamed_html = streamed._committed_html + streamed._tail_label.text()
    if streamed_html.count("<br>") > one_shot._committed_label.text().count("<br>"):
        _fail("streamed message accumulated more line breaks than the one-shot render")


def main() -> int:
    app = _build_app()
    from rikugan.ui.chat_view import ChatView

    host = QWidget()
    host.resize(_PANEL_WIDTH, 900)
    QVBoxLayout(host)
    view = ChatView(host)
    host.layout().addWidget(view)
    host.show()
    app.processEvents()

    _check_bubble_heights(app, view)
    _check_reasoning_only_is_hidden(app, view)
    _check_body_text_contrast()
    _check_question_renders_line_breaks(app, view)
    _check_streamed_message_has_no_band(app, view)

    if _FAILURES:
        print(f"\n{len(_FAILURES)} render check(s) failed")
        return 1
    print("all render checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())

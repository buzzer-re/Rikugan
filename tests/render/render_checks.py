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

from PySide6.QtGui import QColor, QFont, QPalette, QTextDocument  # noqa: E402
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

# A saved answer of the shape that showed the band on reopen: a numbered list of
# long lines, which wraps very differently at a placeholder width than at the
# panel's real one.
_RESTORED_ANSWER = """All five research notes have been written to the `notes/` folder:

1. **initialization/process-entry-point.md** - `_start`'s custom threading setup, with a mermaid flow of the whole boot sequence.
2. **initialization/cocoa-appkit-bootstrap.md** - the AppDelegate shim, thermal notification forwarding, and the `@available` guard helper.
3. **initialization/northlight-engine-bootstrap.md** - the full engine init sequence and main loop, with a sequence diagram.
4. **data-structures/data-path-resolution.md** - writable/read-only path resolution and the `cid_globalversion.bin` mechanism.
5. **general/dylib-architecture.md** - the executable is a loader/orchestrator, with all engine subsystems living in 18 dylibs.

All notes are cross-linked via `[[wiki-links]]` and tagged with `#genre` markers."""


def _fail(message: str) -> None:
    _FAILURES.append(message)
    print(f"FAIL: {message}")


def _content_height(label) -> int:
    """Height *label*'s text really needs, measured independently.

    Deliberately does not call the widget's own measurement: that is the code
    under test, and QLabel.heightForWidth is clamped by the very fixed height
    this is checking, so asking the label would always agree with itself.
    """
    doc = QTextDocument()
    doc.setDefaultFont(label.font())
    doc.setDocumentMargin(0)
    doc.setHtml(label.text())
    doc.setTextWidth(max(1, label.width()))
    return int(doc.size().height() + 0.5)


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
        needed = _content_height(label)
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


def _check_one_widget_per_message(app, view) -> None:
    """A provider interleaves text and tool_use blocks inside one message.

    Closing the bubble at the tool call split that message across two widgets,
    each holding a fragment and each rendering as a bare "Rikugan" label.
    """
    from rikugan.agent.turn import TurnEvent
    from rikugan.ui.message_widgets import AssistantMessageWidget

    before = len(view._container.findChildren(AssistantMessageWidget))
    answer = "Let me check the caller. "
    for event in (
        TurnEvent.turn_start(1),
        TurnEvent.text_delta(answer),
        TurnEvent.tool_call_start("rc1", "decompile_function"),
        TurnEvent.tool_call_done("rc1", "decompile_function", '{"address":"0x1000"}'),
        TurnEvent.text_done(answer),
        TurnEvent.tool_result_event("rc1", "decompile_function", "int main(){}", False),
        TurnEvent.turn_end(1),
    ):
        view.handle_event(event)
        app.processEvents()

    created = view._container.findChildren(AssistantMessageWidget)[before:]
    if len(created) != 1:
        _fail(f"one assistant message rendered as {len(created)} bubbles")
    for widget in created:
        _drain_reveal(widget)
    app.processEvents()
    if created and not created[0]._bubble.isVisible():
        _fail("assistant text was not shown for a message that also called a tool")


def _check_whitespace_only_message_is_hidden(app, view) -> None:
    """Whitespace-only output must not leave a bare role label behind."""
    from rikugan.ui.message_widgets import AssistantMessageWidget

    widget = AssistantMessageWidget(parent=view._container)
    view._insert_widget(widget)
    app.processEvents()
    widget.set_text("\n")
    app.processEvents()
    if widget.isVisible():
        _fail("whitespace-only message still shows a 'Rikugan' label")


def _drain_reveal(widget) -> None:
    for _ in range(400):
        if widget._displayed_len >= len(widget._full_text):
            return
        widget._reveal_tick()


def _check_restored_message_height(app) -> None:
    """Reopening a saved chat must not leave a huge band under the answer.

    Restore sets the text before the label is in a layout, so the first pin is
    taken at a placeholder width. QLabel.heightForWidth then clamps against the
    height that pin fixed, so the stale value could never shrink again.
    """
    from rikugan.core.types import Message, Role
    from rikugan.ui.chat_view import ChatView
    from rikugan.ui.message_widgets import AssistantMessageWidget

    host = QWidget()
    host.resize(1500, 1000)
    QVBoxLayout(host)
    view = ChatView(host)
    host.layout().addWidget(view)
    host.show()
    app.processEvents()

    view.restore_from_messages(
        [
            Message(role=Role.USER, content="write the notes"),
            Message(role=Role.ASSISTANT, content=_RESTORED_ANSWER),
        ]
    )
    app.processEvents()
    app.processEvents()

    for widget in view._container.findChildren(AssistantMessageWidget):
        label = widget._committed_label
        slack = label.height() - _content_height(label)
        if abs(slack) > _SLACK_TOLERANCE_PX:
            _fail(f"restored message is {slack}px taller than its content")


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
    _check_one_widget_per_message(app, view)
    _check_whitespace_only_message_is_hidden(app, view)
    _check_restored_message_height(app)
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

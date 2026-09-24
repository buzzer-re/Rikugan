"""Tests for rikugan.ui.message_widgets — pure logic helpers."""

# ruff: noqa: E402, I001

from __future__ import annotations

import unittest

from tests.qt_stubs import ensure_pyside6_stubs

ensure_pyside6_stubs()

from rikugan.ui.markdown import collapse_breaks, md_to_html
from rikugan.ui.message_widgets import (
    _assistant_bubble_theme,
    _commit_index_in_tail,
    _split_thinking,
    strip_partial_think_tag,
)


_LIGHT_TOKENS = {
    "panel": "#f2f2f2",
    "chat_canvas": "#eeeeee",
    "assistant_bg": "#e9e9e9",
    "tool_bg": "#ebebeb",
    "thinking_bg": "#e6e6e6",
    "input_bg": "#e4e4e4",
    "text": "#202020",
    "muted": "#8a8a8a",
    "subtle": "#626262",
    "border": "#b0b0b0",
    "accent": "#1476a8",
    "accent_text": "#ffffff",
    "code_bg": "#e2e2e2",
}

_DARK_TOKENS = {
    "panel": "#242424",
    "chat_canvas": "#2d2d2d",
    "assistant_bg": "#353535",
    "tool_bg": "#333333",
    "thinking_bg": "#393939",
    "input_bg": "#3c3c3c",
    "text": "#e6e6e6",
    "muted": "#888888",
    "subtle": "#aaaaaa",
    "border": "#555555",
    "accent": "#1678aa",
    "accent_text": "#ffffff",
    "code_bg": "#3a3a3a",
}


def _luminance(color: str) -> float:
    color = color.lstrip("#")
    r = int(color[0:2], 16) / 255.0
    g = int(color[2:4], 16) / 255.0
    b = int(color[4:6], 16) / 255.0
    return 0.2126 * r + 0.7152 * g + 0.0722 * b


# ---------------------------------------------------------------------------
# _split_thinking
# ---------------------------------------------------------------------------


class TestSplitThinking(unittest.TestCase):
    def test_no_think_tags_returns_all_visible(self):
        thinking, visible = _split_thinking("Hello world")
        self.assertEqual(thinking, "")
        self.assertEqual(visible, "Hello world")

    def test_complete_think_block_extracted(self):
        thinking, visible = _split_thinking("Before <think>reasoning</think> After")
        self.assertEqual(thinking, "reasoning")
        self.assertEqual(visible, "Before  After".strip())

    def test_visible_part_stripped(self):
        _thinking, visible = _split_thinking("<think>A</think>   result   ")
        self.assertEqual(visible, "result")

    def test_multiple_think_blocks(self):
        text = "<think>step1</think> middle <think>step2</think> end"
        thinking, visible = _split_thinking(text)
        self.assertIn("step1", thinking)
        self.assertIn("step2", thinking)
        self.assertIn("end", visible)

    def test_empty_string(self):
        thinking, visible = _split_thinking("")
        self.assertEqual(thinking, "")
        self.assertEqual(visible, "")

    def test_unclosed_think_tag_partial_streaming(self):
        text = "Before <think>partial reasoning"
        thinking, visible = _split_thinking(text)
        self.assertIn("partial reasoning", thinking)
        self.assertEqual(visible, "Before")

    def test_empty_think_block(self):
        thinking, visible = _split_thinking("<think></think> result")
        self.assertEqual(thinking, "")
        self.assertEqual(visible, "result")

    def test_think_whitespace_stripped(self):
        thinking, _visible = _split_thinking("<think>  trimmed  </think> x")
        self.assertEqual(thinking, "trimmed")

    def test_multiline_think_block(self):
        text = "<think>\nline1\nline2\n</think> visible"
        thinking, visible = _split_thinking(text)
        self.assertIn("line1", thinking)
        self.assertIn("line2", thinking)
        self.assertEqual(visible, "visible")

    def test_no_visible_content_after_think(self):
        thinking, visible = _split_thinking("<think>inner</think>")
        self.assertEqual(thinking, "inner")
        self.assertEqual(visible, "")

    def test_unclosed_think_empty_partial(self):
        text = "Before <think>"
        thinking, visible = _split_thinking(text)
        self.assertEqual(thinking, "")  # empty partial not added
        self.assertEqual(visible, "Before")


class TestMessageBubbleThemes(unittest.TestCase):
    def test_light_assistant_bubble_uses_light_surface_with_dark_text(self):
        theme = _assistant_bubble_theme(_LIGHT_TOKENS)
        self.assertGreater(_luminance(theme["background"]), 0.75)
        self.assertLess(_luminance(theme["text"]), 0.25)

    def test_dark_assistant_bubble_uses_dark_surface_with_light_text(self):
        theme = _assistant_bubble_theme(_DARK_TOKENS)
        self.assertGreater(_luminance(theme["background"]), 0.15)
        self.assertLess(_luminance(theme["background"]), 0.35)
        self.assertGreater(_luminance(theme["text"]), 0.75)


def _render_streamed(text: str) -> str:
    """Replay the committed/tail split exactly as _render_progressive does."""
    committed_html = ""
    committed_len = 0
    while True:
        tail = text[committed_len:]
        commit = _commit_index_in_tail(tail)
        if commit <= 0:
            break
        committed_html = collapse_breaks(committed_html + md_to_html(tail[:commit]))
        committed_len += commit
    return collapse_breaks(committed_html + md_to_html(text[committed_len:]))


class TestStreamedRenderMatchesOneShot(unittest.TestCase):
    """A message must not grow blank space just because it was streamed.

    Each committed block used to append an unconditional <br> on top of the one
    its own HTML already ended with, so a long answer gained a blank line per
    paragraph — hundreds of pixels of empty space below the text.
    """

    CASES = {
        "paragraphs": "First paragraph.\n\nSecond paragraph.\n\nThird paragraph.",
        "list_then_text": "Summary:\n\n1. First item\n2. Second item\n\nMore text here.\n\nDone.",
        "loose_list": "Steps:\n\n1. First step\n\n2. Second step\n\n3. Third step\n\nDone.",
        "code_block": "Look at this:\n\n```c\nint main(void) {\n  return 0;\n}\n```\n\nThat is the entry point.",
        "heading_and_text": "# Title\n\nBody text here.\n\n## Subtitle\n\nMore body text.",
        "single_paragraph": "Just one paragraph with no breaks at all.",
    }

    def test_streamed_html_is_identical_to_one_shot(self):
        for name, text in self.CASES.items():
            with self.subTest(case=name):
                self.assertEqual(md_to_html(text), _render_streamed(text))

    def test_no_triple_break_runs_survive(self):
        for name, text in self.CASES.items():
            with self.subTest(case=name):
                self.assertNotIn("<br><br><br>", _render_streamed(text))


class TestCommitDoesNotSplitLists(unittest.TestCase):
    def test_commit_point_skips_a_blank_line_between_list_items(self):
        tail = "1. First step\n\n2. Second step\n\nDone."
        commit = _commit_index_in_tail(tail)
        # The only safe commit point is after the list, not between its items.
        self.assertNotIn(commit, (len("1. First step\n\n"),))
        self.assertTrue(tail[:commit].rstrip().endswith("Second step") or commit == 0)

    def test_paragraph_after_list_still_commits(self):
        tail = "- a\n- b\n\nAfter the list.\n\n"
        self.assertGreater(_commit_index_in_tail(tail), 0)

    def test_plain_paragraphs_commit_as_before(self):
        tail = "One.\n\nTwo.\n\n"
        self.assertEqual(_commit_index_in_tail(tail), len(tail))


class TestStripPartialThinkTag(unittest.TestCase):
    """A half-revealed opening tag must not flash as literal text."""

    def test_each_prefix_of_the_tag_is_dropped(self):
        for size in range(1, len("<think>")):
            partial = "<think>"[:size]
            with self.subTest(partial=partial):
                self.assertEqual(strip_partial_think_tag(f"Answer{partial}"), "Answer")

    def test_complete_tag_is_left_for_the_splitter(self):
        self.assertEqual(strip_partial_think_tag("Answer<think>"), "Answer<think>")

    def test_ordinary_text_is_untouched(self):
        for text in ("Answer.", "a < b", "x <= y", ""):
            with self.subTest(text=text):
                self.assertEqual(strip_partial_think_tag(text), text)


if __name__ == "__main__":
    unittest.main()

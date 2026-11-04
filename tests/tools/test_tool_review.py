"""Turning a tool call's JSON into something a person can review."""

from __future__ import annotations

import json
import os
import subprocess
import sys
import textwrap
import unittest

from tests.qt_stubs import ensure_pyside6_stubs

ensure_pyside6_stubs()

from rikugan.core.sanitize import sanitize_tool_result, unwrap_tool_result  # noqa: E402
from rikugan.ui.code_view import (  # noqa: E402
    LANGUAGE_C,
    LANGUAGE_JSON,
    LANGUAGE_PLAIN,
    LANGUAGE_PYTHON,
    LANGUAGE_SHELL,
    MARKER_ADDED,
    MARKER_CONTEXT,
    MARKER_REMOVED,
    language_for_path,
    scan_line,
)
from rikugan.ui.tool_review import MAX_REVIEW_CHARS, build_review, diff_lines, result_language  # noqa: E402
from rikugan.ui.tool_widgets import (  # noqa: E402
    _chip_means_failure,
    _format_result_chip,
    _format_tool_summary,
)


def _kinds(language, text, state=-1):
    spans, end_state = scan_line(language, text, state)
    return [(text[s.start : s.start + s.length], s.kind) for s in spans], end_state


class TestScanLine(unittest.TestCase):
    def test_a_hash_inside_a_string_is_not_a_comment(self):
        kinds, _ = _kinds(LANGUAGE_PYTHON, 'x = "a # b"')
        self.assertEqual(kinds, [('"a # b"', "string")])

    def test_a_quote_inside_a_comment_is_not_a_string(self):
        kinds, _ = _kinds(LANGUAGE_PYTHON, "x = 1  # don't")
        self.assertEqual(kinds, [("1", "number"), ("# don't", "comment")])

    def test_keywords_and_builtins(self):
        kinds, _ = _kinds(LANGUAGE_PYTHON, "def f(): return len(x)")
        self.assertIn(("def", "keyword"), kinds)
        self.assertIn(("return", "keyword"), kinds)
        self.assertIn(("len", "builtin"), kinds)

    def test_a_keyword_inside_a_name_is_not_a_keyword(self):
        kinds, _ = _kinds(LANGUAGE_PYTHON, "format_ifs = 1")
        self.assertEqual(kinds, [("1", "number")])

    def test_hex_is_one_number(self):
        kinds, _ = _kinds(LANGUAGE_C, "a = 0x5A;")
        self.assertEqual(kinds, [("0x5A", "number")])

    def test_an_escaped_quote_stays_in_the_string(self):
        kinds, _ = _kinds(LANGUAGE_C, r'p("a\"b");')
        self.assertEqual(kinds, [(r'"a\"b"', "string")])

    def test_a_docstring_runs_across_lines(self):
        first, state = _kinds(LANGUAGE_PYTHON, 'x = """start')
        self.assertEqual(first, [('"""start', "string")])
        second, state = _kinds(LANGUAGE_PYTHON, "def not_code", state)
        self.assertEqual(second, [("def not_code", "string")])
        third, state = _kinds(LANGUAGE_PYTHON, 'end""" # after', state)
        self.assertEqual(third, [('end"""', "string"), ("# after", "comment")])
        self.assertEqual(state, -1)

    def test_a_docstring_closed_on_its_line_leaves_no_state(self):
        _, state = _kinds(LANGUAGE_PYTHON, '"""one line"""')
        self.assertEqual(state, -1)

    def test_a_c_block_comment_runs_across_lines(self):
        first, state = _kinds(LANGUAGE_C, "int a; /* begin")
        self.assertIn(("/* begin", "comment"), first)
        second, state = _kinds(LANGUAGE_C, "int b; */ int c;", state)
        self.assertEqual(second[0], ("int b; */", "comment"))
        self.assertIn(("int", "builtin"), second[1:])
        self.assertEqual(state, -1)

    def test_c_preprocessor(self):
        kinds, _ = _kinds(LANGUAGE_C, "#include <stdio.h>")
        self.assertEqual(kinds[0], ("#include", "meta"))

    def test_shell_variables_and_comments(self):
        kinds, _ = _kinds(LANGUAGE_SHELL, "echo $SCRATCH # note")
        self.assertIn(("$SCRATCH", "meta"), kinds)
        self.assertIn(("# note", "comment"), kinds)

    def test_a_shell_hash_inside_a_word_is_not_a_comment(self):
        kinds, _ = _kinds(LANGUAGE_SHELL, "curl a#b")
        self.assertNotIn("comment", [kind for _, kind in kinds])

    def test_json_keys_differ_from_values(self):
        kinds, _ = _kinds(LANGUAGE_JSON, '{"a": "b", "n": 1, "t": true}')
        self.assertEqual(
            kinds,
            [('"a"', "keyword"), ('"b"', "string"), ('"n"', "keyword"), ("1", "number"), ('"t"', "keyword"), ("true", "builtin")],
        )

    def test_an_unknown_language_colours_nothing(self):
        self.assertEqual(scan_line(LANGUAGE_PLAIN, "def x"), ([], -1))

    def test_every_language_survives_awkward_lines(self):
        lines = ("", "   ", '"', "'", '"""', "/*", "*/", "\\", "#", "$", "é𝒳", "\t\t", "0x", "1e", '"a\\')
        for language in (LANGUAGE_PYTHON, LANGUAGE_C, LANGUAGE_SHELL, LANGUAGE_JSON, "config"):
            for line in lines:
                for state in (-1, 0):
                    with self.subTest(language=language, line=line, state=state):
                        spans, _ = scan_line(language, line, state)
                        for span in spans:
                            self.assertGreaterEqual(span.start, 0)
                            self.assertLessEqual(span.start + span.length, len(line))

    def test_spans_never_overlap(self):
        spans, _ = scan_line(LANGUAGE_PYTHON, 'def f(a="x#y"): return 0x10  # done')
        ends = [s.start + s.length for s in spans]
        starts = [s.start for s in spans]
        self.assertTrue(all(start >= end for start, end in zip(starts[1:], ends)))


class TestLanguageForPath(unittest.TestCase):
    def test_by_extension(self):
        for path, language in {
            "scratch/a.py": LANGUAGE_PYTHON,
            "x/B.CPP": LANGUAGE_C,
            "run.sh": LANGUAGE_SHELL,
            "rAU.ps1": LANGUAGE_SHELL,
            "c.json": LANGUAGE_JSON,
            "notes.txt": LANGUAGE_PLAIN,
            "Makefile": LANGUAGE_PLAIN,
        }.items():
            with self.subTest(path):
                self.assertEqual(language_for_path(path), language)


class TestDiffLines(unittest.TestCase):
    def test_a_changed_line_is_removed_then_added(self):
        text, markers = diff_lines("a\nb\nc", "a\nB\nc")
        self.assertEqual(text, "a\nb\nB\nc")
        self.assertEqual(markers, [MARKER_CONTEXT, MARKER_REMOVED, MARKER_ADDED, MARKER_CONTEXT])

    def test_pure_addition(self):
        text, markers = diff_lines("a", "a\nb")
        self.assertEqual((text, markers), ("a\nb", [MARKER_CONTEXT, MARKER_ADDED]))

    def test_pure_deletion(self):
        text, markers = diff_lines("a\nb", "a")
        self.assertEqual((text, markers), ("a\nb", [MARKER_CONTEXT, MARKER_REMOVED]))

    def test_one_marker_per_line(self):
        text, markers = diff_lines("x\n\ny\n", "x\nz\n\ny\nw")
        self.assertEqual(len(text.split("\n")), len(markers))

    def test_a_huge_edit_does_not_compare_quadratically(self):
        old = "\n".join(f"a{n}" for n in range(5000))
        new = "\n".join(f"b{n}" for n in range(5000))
        _, markers = diff_lines(old, new)
        self.assertEqual(markers.count(MARKER_REMOVED), 5000)
        self.assertEqual(markers.count(MARKER_ADDED), 5000)


class TestBuildReview(unittest.TestCase):
    def _review(self, name, **args):
        return build_review(name, json.dumps(args))

    def test_write_file_shows_the_real_text_not_json(self):
        review = self._review("write_file", file_path="a.py", content="x = 1\ny = 2\n")
        self.assertEqual(review.code, "x = 1\ny = 2")
        self.assertEqual(review.language, LANGUAGE_PYTHON)
        self.assertEqual(review.detail, "2 lines")

    def test_an_empty_write_says_so(self):
        self.assertEqual(self._review("write_file", file_path="a.py", content="").detail, "empty file")

    def test_edit_file_is_a_diff(self):
        review = self._review("edit_file", file_path="a.c", old_string="a\nb", new_string="a\nc")
        self.assertEqual(review.code, "a\nb\nc")
        self.assertEqual(review.markers, [MARKER_CONTEXT, MARKER_REMOVED, MARKER_ADDED])
        self.assertEqual(review.language, LANGUAGE_C)
        self.assertEqual(review.detail, "-1 +1")

    def test_edit_file_says_when_it_replaces_everywhere(self):
        review = self._review("edit_file", file_path="a", old_string="a", new_string="b", replace_all=True)
        self.assertIn("every occurrence", review.detail)

    def test_execute_python_is_code(self):
        review = self._review("execute_python", code="print(1)\n")
        self.assertEqual((review.code, review.language), ("print(1)", LANGUAGE_PYTHON))

    def test_execute_python_accepts_script_too(self):
        self.assertEqual(self._review("execute_python", script="x").code, "x")

    def test_run_shell_shows_the_command_and_its_limits(self):
        review = self._review("run_shell", command="make all", working_dir="src", timeout=60)
        self.assertEqual((review.code, review.language), ("make all", LANGUAGE_SHELL))
        self.assertIn("in src", review.detail)
        self.assertIn("timeout 60s", review.detail)

    def test_read_file_states_its_range(self):
        self.assertEqual(self._review("read_file", file_path="a", offset=5, limit=3).detail, "from line 5 · 3 lines at most")

    def test_a_plain_read_adds_nothing(self):
        review = self._review("read_file", file_path="a", offset=1)
        self.assertEqual((review.heading, review.detail, review.code), ("", "", ""))

    def test_search_files_states_its_scope(self):
        review = self._review("search_files", pattern="x", glob="*.c", case_insensitive=True)
        self.assertIn("matching *.c", review.detail)
        self.assertIn("ignoring case", review.detail)

    def test_a_short_path_is_left_to_the_card_header(self):
        self.assertEqual(self._review("write_file", file_path="a.py", content="x").heading, "")

    def test_a_path_the_header_would_shorten_is_shown_whole(self):
        path = "d/" * 60 + "a.py"
        self.assertEqual(self._review("write_file", file_path=path, content="x").heading, path)

    def test_a_huge_write_says_it_is_partial(self):
        review = self._review("write_file", file_path="a", content="x" * (MAX_REVIEW_CHARS + 7))
        self.assertEqual(len(review.code), MAX_REVIEW_CHARS)
        self.assertEqual(review.omitted_chars, 7)

    def test_a_normal_write_claims_nothing_omitted(self):
        self.assertEqual(self._review("write_file", file_path="a", content="x").omitted_chars, 0)

    def test_a_huge_edit_says_it_is_partial(self):
        review = self._review("edit_file", file_path="a", old_string="x" * MAX_REVIEW_CHARS, new_string="y" * 10)
        self.assertGreater(review.omitted_chars, 0)

    def test_other_tools_have_no_review(self):
        self.assertIsNone(self._review("rename_function", old_name="a", new_name="b"))

    def test_malformed_arguments_fall_back_rather_than_crash(self):
        self.assertIsNone(build_review("write_file", "{not json"))
        self.assertIsNone(build_review("write_file", "[1, 2]"))
        self.assertIsNone(build_review("read_file", json.dumps({"file_path": "a", "limit": "many"})))

    def test_no_arguments_yet(self):
        self.assertIsNotNone(build_review("run_shell", ""))


class TestResultLanguage(unittest.TestCase):
    def test_a_read_file_is_coloured_by_its_extension(self):
        self.assertEqual(result_language("read_file", '{"file_path": "a.py"}'), LANGUAGE_PYTHON)

    def test_shell_output_is_plain(self):
        self.assertEqual(result_language("run_shell", '{"command": "x.py"}'), LANGUAGE_PLAIN)

    def test_bad_arguments_are_plain(self):
        self.assertEqual(result_language("read_file", "{oops"), LANGUAGE_PLAIN)


class TestUnwrapToolResult(unittest.TestCase):
    def test_the_wrapper_comes_off(self):
        wrapped = sanitize_tool_result("line one\nline two", "read_file")
        self.assertEqual(unwrap_tool_result(wrapped), "line one\nline two")

    def test_anything_else_is_untouched(self):
        for text in ("plain", "", "Error: nope", "<tool_result>no preamble</tool_result>"):
            with self.subTest(text):
                self.assertEqual(unwrap_tool_result(text), text)

    def test_it_only_removes_the_outer_wrapper(self):
        inner = '<tool_result name="x">data'
        self.assertIn(inner.split(">")[1], unwrap_tool_result(sanitize_tool_result(inner, "t")))


class TestCardRow(unittest.TestCase):
    def test_file_tools_summarise_as_their_path(self):
        for name in ("read_file", "write_file", "edit_file"):
            with self.subTest(name):
                self.assertEqual(_format_tool_summary(name, json.dumps({"file_path": "a.py", "content": "x"})), "a.py")

    def test_a_shell_command_summarises_as_its_first_line(self):
        self.assertEqual(_format_tool_summary("run_shell", json.dumps({"command": "make\necho done"})), "make")

    def test_search_summarises_as_its_pattern(self):
        self.assertEqual(_format_tool_summary("search_files", json.dumps({"pattern": "abc"})), '"abc"')

    def test_a_successful_command_is_a_tick(self):
        self.assertEqual(_format_result_chip("run_shell", "Exit code 0 · 1s"), "✓")

    def test_a_failed_command_shows_its_exit_code(self):
        self.assertEqual(_format_result_chip("run_shell", "Exit code 3 · 1s"), "exit 3")

    def test_a_stopped_command_says_so(self):
        self.assertEqual(_format_result_chip("run_shell", "The command timed out after 1s"), "stopped")

    def test_only_a_clean_command_counts_as_fine(self):
        self.assertTrue(_chip_means_failure("run_shell", "exit 3"))
        self.assertTrue(_chip_means_failure("run_shell", "stopped"))
        self.assertFalse(_chip_means_failure("run_shell", "✓"))
        self.assertFalse(_chip_means_failure("read_file", "3 lines"))

    def test_a_read_shows_its_length(self):
        self.assertEqual(_format_result_chip("read_file", "=== a.py (1,204 lines) ===\n1\tx"), "1,204 lines")

    def test_a_search_shows_its_count(self):
        result = "=== search_files [python]: /x/ in . ===\nRows 0-80 of 312:\na:1:x"
        self.assertEqual(_format_result_chip("search_files", result), "312 rows")

    def test_a_search_with_nothing_found(self):
        self.assertEqual(_format_result_chip("search_files", "=== s ===\nNo matches."), "0 rows")


_REAL_QT_SCRIPT = textwrap.dedent(
    """
    import json, os, sys
    os.environ["QT_QPA_PLATFORM"] = "offscreen"
    from PySide6.QtCore import Qt
    from PySide6.QtWidgets import QApplication, QLabel, QWidget, QVBoxLayout
    app = QApplication([])
    from rikugan.core.sanitize import sanitize_tool_result
    from rikugan.ui.code_view import CodeView
    from rikugan.ui.tool_widgets import ToolApprovalWidget, ToolCallWidget

    def settle(widget):
        widget.resize(700, 400)
        widget.show()
        for _ in range(3):
            app.processEvents()

    def card(name, args, result="done"):
        root = QWidget()
        lay = QVBoxLayout(root)
        w = ToolCallWidget(name, "id", parent=root)
        lay.addWidget(w)
        w.set_arguments(json.dumps(args))
        w.set_result(sanitize_tool_result(result, name), False)
        return root, w

    # Nothing heavy until a card is opened: most never are.
    root, w = card("write_file", {"file_path": "a.py", "content": "x = 1\\n"})
    assert not w.findChildren(CodeView), "built before expanding"
    w._toggle()
    settle(root)
    views = w.findChildren(CodeView)
    assert len(views) == 1 and views[0].toPlainText() == "x = 1", "write_file view"
    assert not w._args_label.isVisible(), "raw JSON still showing"
    w._toggle()
    w._toggle()
    assert len(w.findChildren(CodeView)) == 1, "built twice"

    # A short block fits exactly: no scrollbar, nothing clipped.
    root, w = card("write_file", {"file_path": "a.py", "content": "\\n".join("line %d" % n for n in range(13))})
    w._toggle()
    settle(root)
    view = w.findChildren(CodeView)[0]
    assert view.verticalScrollBar().maximum() == 0, "13 lines needed a scrollbar"

    # A wide line gets room for the horizontal bar, so the last line stays visible.
    wide = "\\n".join(["x" * 400] + ["line"] * 5)
    root, w = card("write_file", {"file_path": "a.py", "content": wide})
    w._toggle()
    settle(root)
    view = w.findChildren(CodeView)[0]
    assert view.horizontalScrollBar().maximum() > 0, "expected horizontal overflow"
    assert view.verticalScrollBar().maximum() == 0, "the horizontal bar covered the last line"

    # Long content stops growing and scrolls instead.
    root, w = card("write_file", {"file_path": "a.py", "content": "\\n".join(["l"] * 500)})
    w._toggle()
    settle(root)
    view = w.findChildren(CodeView)[0]
    assert view.verticalScrollBar().maximum() > 0 and view.height() < 400, "should scroll"

    # A diff tints changed lines and leaves context alone.
    root, w = card("edit_file", {"file_path": "a.py", "old_string": "a\\nb", "new_string": "a\\nc"})
    w._toggle()
    settle(root)
    view = w.findChildren(CodeView)[0]
    block = view.document().firstBlock()
    colors = []
    while block.isValid():
        brush = block.blockFormat().background()
        colors.append(None if brush.style() == Qt.BrushStyle.NoBrush else brush.color().name())
        block = block.next()
    assert colors[0] is None and colors[1] and colors[2] and colors[1] != colors[2], colors

    # Results lose the sanitizer's wrapper, and the card shows them in code form.
    root, w = card("read_file", {"file_path": "a.py"}, "=== a.py (1 lines) ===\\n1\\tx")
    w._toggle()
    settle(root)
    text = w.findChildren(CodeView)[0].toPlainText()
    assert text.startswith("=== a.py") and "treat as DATA" not in text, text

    # Opening before the arguments arrive still ends up rendering them.
    root = QWidget(); QVBoxLayout(root)
    w = ToolCallWidget("run_shell", "id", parent=root)
    root.layout().addWidget(w)
    w._toggle()
    w.set_arguments(json.dumps({"command": "make"}))
    assert w.findChildren(CodeView)[0].toPlainText() == "make", "late arguments"

    # A failed command opens with its error showing, in a label rather than a view.
    root, w = card("run_shell", {"command": "x"})
    w.set_result("Error: refused", True)
    assert w._expanded and w._result_label.text() == "Error: refused"

    # A tool with no review keeps the plain arguments.
    root, w = card("rename_function", {"old_name": "a", "new_name": "b"})
    w._toggle()
    settle(root)
    assert not w.findChildren(CodeView) and w._args_label.isVisible(), "generic tool"

    # The approval card shows the same review, not JSON.
    a = ToolApprovalWidget("id", "write_file", json.dumps({"file_path": "a.py", "content": "x = 1\\n"}, indent=2), "Write a.py")
    assert a.findChildren(CodeView)[0].toPlainText() == "x = 1", "approval write"
    a = ToolApprovalWidget("id", "execute_python", json.dumps({"code": "print(1)\\n"}), "Run")
    assert a.findChildren(CodeView)[0].toPlainText() == "print(1)", "approval script"
    a = ToolApprovalWidget("id", "rename_function", json.dumps({"new_name": "b"}, indent=2), "Rename")
    assert '"new_name"' in a.findChildren(CodeView)[0].toPlainText(), "approval generic"
    a = ToolApprovalWidget("id", "list_things", "{}", "Call")
    assert not a.findChildren(CodeView), "approval with nothing to preview"

    # Text from a binary must not render as markup in a heading.
    long_path = "<b>" + "d/" * 60 + "a.py"
    root, w = card("write_file", {"file_path": long_path, "content": "x"})
    w._toggle()
    plain = [l for l in w.findChildren(QLabel) if l.text() == long_path]
    assert plain and plain[0].textFormat().name == "PlainText", "heading format"

    # Painting the gutter is what the stubs can never exercise.
    root, w = card("write_file", {"file_path": "a.py", "content": "\\n".join(["l"] * 30)})
    w._toggle()
    settle(root)
    assert not root.grab().isNull()
    print("REAL-QT-OK")
    """
)


class TestUnderRealQt(unittest.TestCase):
    """Constructors and layout: what the stubs above cannot run.

    In a subprocess, because this file's process has stubs where Qt should be.
    """

    def test_the_widgets(self):
        # Skips on a machine that cannot start Qt at all, which says nothing about
        # the widgets. Anything failing past this point is a real failure.
        probe = subprocess.run(
            [
                sys.executable,
                "-c",
                "import os; os.environ['QT_QPA_PLATFORM'] = 'offscreen';"
                "from PySide6.QtWidgets import QApplication; QApplication([])",
            ],
            capture_output=True,
            check=False,
        )
        if probe.returncode != 0:
            self.skipTest("Qt cannot start here")
        root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
        done = subprocess.run(
            [sys.executable, "-c", _REAL_QT_SCRIPT],
            capture_output=True,
            text=True,
            cwd=root,
            env={**os.environ, "PYTHONPATH": root},
            timeout=120,
            check=False,
        )
        self.assertIn("REAL-QT-OK", done.stdout, done.stderr[-3000:])


if __name__ == "__main__":
    unittest.main()

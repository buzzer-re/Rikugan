"""An oversized result has to stay honest about what the model is not seeing."""

from __future__ import annotations

import os
import re
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(__file__))))

from rikugan.core.artifacts import (  # noqa: E402
    INLINE_RESULT_BUDGET,
    MAX_ARTIFACT_CHARS,
    MAX_LINE_CHARS,
    ArtifactStore,
    format_large_result,
)
from rikugan.core.sanitize import sanitize_tool_result, strip_injection_markers  # noqa: E402
from rikugan.core.workspace import PathRefused  # noqa: E402

_SHOWN = re.compile(r"Showing lines 1-(\d+)(?: and (\d+)-(\d+))? of (\d+); ([\d,]+) middle lines")


def _numbered_text(count, width=60):
    return "\n".join(f"line {n} ".ljust(width, "x") for n in range(1, count + 1))


def _render(text, **kwargs):
    store = ArtifactStore()
    return format_large_result("out", text, store, **kwargs), store


class TestSmallOutput(unittest.TestCase):
    def test_comes_back_whole_and_unnumbered(self):
        rendered, _ = _render("hello\nworld\n")
        self.assertEqual(rendered, "=== out ===\nhello\nworld")

    def test_saves_nothing(self):
        _, store = _render("hello")
        with self.assertRaises(PathRefused):
            store.read("artifact://1")

    def test_footer_is_kept(self):
        rendered, _ = _render("hello", footer="Wall: 1s")
        self.assertTrue(rendered.endswith("— Wall: 1s —"))


class TestOversizedOutput(unittest.TestCase):
    def setUp(self):
        self.text = _numbered_text(10_000)
        self.rendered, self.store = _render(self.text, footer="Wall: 1s")

    def test_stays_within_budget(self):
        self.assertLessEqual(len(self.rendered), INLINE_RESULT_BUDGET)

    def test_shows_both_ends(self):
        self.assertIn("line 1 ", self.rendered)
        self.assertIn("line 10000 ", self.rendered)

    def test_the_notice_comes_straight_after_the_title(self):
        lines = self.rendered.split("\n")
        self.assertEqual(lines[0], "=== out ===")
        self.assertTrue(lines[1].startswith("⟦Showing lines"))

    def test_the_notice_names_the_exact_call(self):
        head_end = int(_SHOWN.search(self.rendered).group(1))
        self.assertIn(f'read_file("artifact://1", offset={head_end + 1})', self.rendered)

    def test_the_accounting_adds_up(self):
        match = _SHOWN.search(self.rendered)
        head_end, tail_start, tail_end, total = (int(match.group(i)) for i in range(1, 5))
        elided = int(match.group(5).replace(",", ""))
        self.assertEqual(total, 10_000)
        self.assertEqual(tail_end, total)
        self.assertEqual(head_end + (tail_end - tail_start + 1) + elided, total)

    def test_the_notice_describes_what_is_really_shown(self):
        match = _SHOWN.search(self.rendered)
        head_end, tail_start = int(match.group(1)), int(match.group(2))
        self.assertIn(f"\nline {head_end} ".replace("\nline", f"{head_end}\tline"), self.rendered)
        self.assertIn(f"{tail_start}\tline {tail_start} ", self.rendered)
        self.assertNotIn(f"{head_end + 1}\tline {head_end + 1} ", self.rendered)
        self.assertNotIn(f"{tail_start - 1}\tline {tail_start - 1} ", self.rendered)

    def test_the_head_gets_more_than_the_tail(self):
        match = _SHOWN.search(self.rendered)
        head = int(match.group(1))
        tail = int(match.group(3)) - int(match.group(2)) + 1
        self.assertGreater(head, tail)
        self.assertGreater(tail, 0)

    def test_the_footer_survives(self):
        self.assertTrue(self.rendered.endswith("— Wall: 1s —"))

    def test_fully_saved_output_does_not_claim_otherwise(self):
        self.assertNotIn("not saved", self.rendered)


class TestBudgetHoldsForAnyInput(unittest.TestCase):
    def test_every_size_and_shape_fits(self):
        cases = {
            "one byte": "x",
            "just over": "x\n" * (INLINE_RESULT_BUDGET // 2 + 10),
            "many short lines": "x\n" * 5_000_000,
            "wide lines": _numbered_text(5_000, width=1_900),
            "one huge line": "x" * 50_000_000,
            "huge line between short": "a\n" + "x" * 5_000_000 + "\nz\n",
            "only newlines": "\n" * 1_000_000,
            "multibyte": "日本語\n" * 1_000_000,
        }
        for label, text in cases.items():
            with self.subTest(label):
                rendered, _ = _render(text, footer="Wall: 1s")
                self.assertLessEqual(len(rendered), INLINE_RESULT_BUDGET)

    def test_a_long_title_cannot_push_it_over(self):
        rendered = format_large_result("t" * 5_000, _numbered_text(10_000), ArtifactStore())
        self.assertLessEqual(len(rendered), INLINE_RESULT_BUDGET)

    def test_a_title_cannot_add_lines(self):
        rendered = format_large_result("a\nb", "x", ArtifactStore())
        self.assertEqual(rendered.split("\n")[0], "=== a b ===")


class TestLaterCaps(unittest.TestCase):
    def test_the_handle_survives_the_cap_on_old_tool_results(self):
        # Old results are cut to their first 500 chars, sanitizer's wrapper
        # included. If that takes the notice, the model loses the handle.
        rendered = format_large_result("t" * 5_000, _numbered_text(10_000), ArtifactStore())
        self.assertIn("artifact://1", sanitize_tool_result(rendered, "run_shell")[:500])

    def test_the_handle_survives_with_unsaved_bytes(self):
        rendered = format_large_result("t" * 5_000, _numbered_text(100_000), ArtifactStore())
        self.assertIn("artifact://1", sanitize_tool_result(rendered, "run_shell")[:500])

    def test_the_notice_survives_the_sanitizer(self):
        rendered, _ = _render(_numbered_text(10_000))
        self.assertIn("⟦Showing lines", strip_injection_markers(rendered))


class TestReadingBack(unittest.TestCase):
    def setUp(self):
        self.store = ArtifactStore()
        format_large_result("out", _numbered_text(10_000), self.store)

    def test_numbers_are_the_originals(self):
        page = self.store.read("artifact://1", offset=500, limit=2)
        self.assertIn("500\tline 500 ", page)
        self.assertIn("501\tline 501 ", page)
        self.assertNotIn("502\tline", page)

    def test_a_page_names_the_next_call(self):
        page = self.store.read("artifact://1", offset=500, limit=2)
        self.assertIn('read_file("artifact://1", offset=502)', page)

    def test_the_last_page_says_it_is_the_end(self):
        self.assertIn("the end", self.store.read("artifact://1", offset=9_999))

    def test_a_page_stays_within_budget(self):
        page = self.store.read("artifact://1", offset=1, limit=1_000_000)
        self.assertLessEqual(len(page), INLINE_RESULT_BUDGET)

    def test_following_the_hints_reaches_every_line(self):
        seen, offset = [], 1
        for _ in range(1_000):
            page = self.store.read("artifact://1", offset=offset)
            seen += [int(m) for m in re.findall(r"^(\d+)\t", page, re.M)]
            next_call = re.search(r"offset=(\d+)\)", page)
            if next_call is None:
                break
            offset = int(next_call.group(1))
        self.assertEqual(seen, list(range(1, 10_001)))

    def test_past_the_end_says_so(self):
        self.assertIn("past the end", self.store.read("artifact://1", offset=20_000))

    def test_a_handle_that_never_existed(self):
        with self.assertRaises(PathRefused) as ctx:
            self.store.read("artifact://99")
        self.assertIn("does not exist", str(ctx.exception))

    def test_a_malformed_handle(self):
        with self.assertRaises(PathRefused):
            self.store.read("artifact://../../etc/passwd")


class TestOutputOverTheSaveCap(unittest.TestCase):
    def setUp(self):
        # Well past MAX_ARTIFACT_CHARS at 60 chars a line.
        self.count = 2 * MAX_ARTIFACT_CHARS // 60
        self.store = ArtifactStore()
        self.rendered = format_large_result("out", _numbered_text(self.count), self.store)
        self.artifact = self.store.get(1)

    def test_only_now_does_the_notice_say_bytes_were_dropped(self):
        self.assertIn("not saved", self.rendered)
        self.assertGreater(self.artifact.unsaved_bytes, 0)

    def test_saved_size_respects_the_cap(self):
        self.assertLessEqual(self.artifact.chars, MAX_ARTIFACT_CHARS)

    def test_it_keeps_the_true_line_count(self):
        self.assertEqual(self.artifact.total_lines, self.count)

    def test_the_tail_keeps_original_numbers(self):
        page = self.store.read("artifact://1", offset=self.count)
        self.assertIn(f"{self.count}\tline {self.count} ", page)

    def test_a_read_in_the_gap_says_so_instead_of_returning_nothing(self):
        middle = self.count // 2
        self.assertTrue(len(self.artifact.head) < middle < self.artifact.tail_first_line)
        page = self.store.read("artifact://1", offset=middle)
        self.assertIn("never saved", page)
        self.assertIn(f"offset={self.artifact.tail_first_line}", page)

    def test_paging_up_to_the_gap_points_across_it(self):
        page = self.store.read("artifact://1", offset=len(self.artifact.head))
        self.assertIn("never saved", page)
        self.assertIn(f"offset={self.artifact.tail_first_line}", page)


class TestLongLines(unittest.TestCase):
    def test_one_huge_line_can_still_be_paged(self):
        store = ArtifactStore()
        text = "".join(chr(ord("a") + i % 26) * MAX_LINE_CHARS for i in range(100))
        format_large_result("out", text, store)
        page = store.read("artifact://1", offset=50, limit=1)
        self.assertIn("50\t" + chr(ord("a") + 49 % 26) * MAX_LINE_CHARS, page)


class TestEviction(unittest.TestCase):
    def _save(self, store, chars=10):
        return store.save("t", ["x" * chars])

    def test_the_artifact_count_is_capped_oldest_first(self):
        store = ArtifactStore()
        with patch("rikugan.core.artifacts.MAX_ARTIFACTS", 2):
            for _ in range(3):
                self._save(store)
        with self.assertRaises(PathRefused) as ctx:
            store.get(1)
        self.assertIn("dropped", str(ctx.exception))
        self.assertEqual(store.get(3).id, 3)

    def test_the_total_size_is_capped(self):
        store = ArtifactStore()
        with patch("rikugan.core.artifacts.MAX_STORE_CHARS", 25):
            for _ in range(3):
                self._save(store)
        with self.assertRaises(PathRefused):
            store.get(1)
        self.assertEqual(store.get(2).id, 2)

    def test_reading_keeps_an_artifact_alive(self):
        store = ArtifactStore()
        with patch("rikugan.core.artifacts.MAX_ARTIFACTS", 2):
            self._save(store)
            self._save(store)
            store.get(1)
            self._save(store)
        self.assertEqual(store.get(1).id, 1)
        with self.assertRaises(PathRefused):
            store.get(2)

    def test_ids_are_never_reused(self):
        store = ArtifactStore()
        with patch("rikugan.core.artifacts.MAX_ARTIFACTS", 1):
            self._save(store)
            self.assertEqual(self._save(store).id, 2)

    def test_the_new_one_survives_even_over_the_cap(self):
        store = ArtifactStore()
        with patch("rikugan.core.artifacts.MAX_STORE_CHARS", 5):
            artifact = self._save(store)
        self.assertIs(store.get(artifact.id), artifact)


class TestEmptyAndEdges(unittest.TestCase):
    def test_an_empty_artifact(self):
        store = ArtifactStore()
        store.save("t", [])
        self.assertIn("empty", store.read("artifact://1"))

    def test_empty_output_renders(self):
        rendered, _ = _render("")
        self.assertEqual(rendered, "=== out ===\n")


if __name__ == "__main__":
    unittest.main()

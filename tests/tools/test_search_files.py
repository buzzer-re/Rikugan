"""search_files: each behaviour checked against both engines, for the same shape."""

from __future__ import annotations

import os
import shutil
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, os.path.dirname(os.path.dirname(__file__)))
from tests.mocks.ida_mock import install_ida_mocks

install_ida_mocks()

for _mod_name in ("rikugan.core.types", "rikugan.core.config", "rikugan.state.session"):
    sys.modules.pop(_mod_name, None)

from rikugan.core.artifacts import INLINE_RESULT_BUDGET  # noqa: E402
from rikugan.core.chat_context import activate_session  # noqa: E402
from rikugan.core.workspace import Workspace  # noqa: E402
from rikugan.state.session import SessionState  # noqa: E402
from rikugan.tools import file_search  # noqa: E402
from rikugan.tools.registry import ToolRegistry  # noqa: E402

HAS_RIPGREP = shutil.which("rg") is not None


class _SearchTest(unittest.TestCase):
    """Runs against the Python scan. The subclass reruns everything on ripgrep."""

    engine = "python"

    def setUp(self):
        if self.engine == "ripgrep" and not HAS_RIPGREP:
            self.skipTest("ripgrep is not installed")
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.top = os.path.realpath(tmp.name)
        self.root = os.path.join(self.top, "case")
        os.makedirs(self.root)
        self.session = SessionState()
        self.session.files.workspace = Workspace.for_database(
            os.path.join(self.root, "sample.exe"), os.path.join(self.top, "config"), "abc", home=os.path.join(self.top, "h")
        )
        self.registry = ToolRegistry()
        self.registry.register_module(file_search)
        if self.engine == "python":
            patcher = patch("rikugan.tools.file_search.shutil.which", return_value=None)
            patcher.start()
            self.addCleanup(patcher.stop)
        self._file("a.py", "import os\ndef alpha():\n    return 1\n")
        self._file("b.txt", "nothing here\nALPHA upper\n")
        self._file("sub/c.py", "x = 1\nalpha = 2\ny = 3\nz = 4\nw = 5\nalpha = 6\n")
        self._file("sub/d.c", "int alpha;\n")

    def _file(self, name, text):
        path = os.path.join(self.root, name)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8") as f:
            f.write(text)

    def _search(self, **arguments):
        with activate_session(self.session):
            return self.registry.execute("search_files", arguments)

    def _rows(self, **arguments):
        return self._search(**arguments).split("\n")[2:]


class _Behaviours:
    """Shared assertions, which is what makes the two engines' results identical."""

    def test_names_the_engine(self):
        self.assertIn(f"[{self.engine}]", self._search(pattern="alpha"))

    def test_content_rows(self):
        self.assertEqual(
            self._rows(pattern="alpha"),
            ["a.py:2:def alpha():", "sub/c.py:2:alpha = 2", "sub/c.py:6:alpha = 6", "sub/d.c:1:int alpha;"],
        )

    def test_case_insensitive(self):
        self.assertIn("b.txt:2:ALPHA upper", self._rows(pattern="alpha", case_insensitive=True))

    def test_files_with_matches(self):
        self.assertEqual(
            self._rows(pattern="alpha", mode="files_with_matches"),
            ["a.py", "sub/c.py", "sub/d.c"],
        )

    def test_listing_files(self):
        self.assertEqual(self._rows(mode="files"), ["a.py", "b.txt", "sub/c.py", "sub/d.c"])

    def test_listing_files_by_glob(self):
        self.assertEqual(self._rows(mode="files", glob="*.py"), ["a.py", "sub/c.py"])

    def test_listing_filtered_by_a_path_pattern(self):
        self.assertEqual(self._rows(mode="files", pattern=r"^sub/"), ["sub/c.py", "sub/d.c"])

    def test_glob_with_a_slash_and_globstar(self):
        self.assertEqual(self._rows(mode="files", glob="sub/**/*.c"), ["sub/d.c"])

    def test_glob_braces(self):
        self.assertEqual(self._rows(mode="files", glob="*.{c,txt}"), ["b.txt", "sub/d.c"])

    def test_negated_glob(self):
        self.assertEqual(self._rows(mode="files", glob="!*.py"), ["b.txt", "sub/d.c"])

    def test_glob_narrows_content(self):
        self.assertEqual(self._rows(pattern="alpha", glob="*.c"), ["sub/d.c:1:int alpha;"])

    def test_context_lines(self):
        self.assertEqual(
            self._rows(pattern="alpha", path="sub/c.py", context_lines=1),
            [
                "c.py-1-x = 1",
                "c.py:2:alpha = 2",
                "c.py-3-y = 3",
                "--",
                "c.py-5-w = 5",
                "c.py:6:alpha = 6",
            ],
        )

    def test_context_between_files_is_separated(self):
        self.assertEqual(
            self._rows(pattern="alpha", glob="*.{c,py}", context_lines=0)[0], "a.py:2:def alpha():"
        )
        rows = self._rows(pattern="int alpha|def alpha", context_lines=1)
        self.assertIn("--", rows)

    def test_context_does_not_decorate_a_file_list(self):
        self.assertEqual(
            self._rows(pattern="alpha", mode="files_with_matches", context_lines=2),
            ["a.py", "sub/c.py", "sub/d.c"],
        )

    def test_the_stopped_notice_comes_first_so_later_cuts_keep_it(self):
        found = file_search._Found(stopped_early="x")
        self.assertTrue(file_search._stopped_note(found).startswith("⟦This search stopped early"))

    def test_a_directory_path(self):
        self.assertEqual(self._rows(pattern="alpha", path="sub", glob="*.c"), ["d.c:1:int alpha;"])

    def test_a_single_file_path(self):
        self.assertEqual(self._rows(pattern="def", path="a.py"), ["a.py:2:def alpha():"])

    def test_no_matches_says_so(self):
        self.assertIn("No matches.", self._search(pattern="zzz_absent"))

    def test_an_invalid_pattern_is_explained(self):
        result = self._search(pattern="(unclosed")
        self.assertTrue(result.startswith("Error:"))
        self.assertIn("ripgrep" if self.engine == "ripgrep" else "Python", result)

    def test_a_pattern_starting_with_a_dash_is_still_a_pattern(self):
        self._file("dash.txt", "--flag here\n")
        self.assertEqual(self._rows(pattern="--flag"), ["dash.txt:1:--flag here"])

    def test_binary_files_are_skipped(self):
        with open(os.path.join(self.root, "blob.bin"), "wb") as f:
            f.write(b"\x00\x01alpha\n")
        self.assertNotIn("blob.bin", self._search(pattern="alpha"))

    def test_git_and_node_modules_are_skipped(self):
        self._file(".git/config", "alpha\n")
        self._file("node_modules/x/y.js", "alpha\n")
        result = self._search(pattern="alpha")
        self.assertNotIn(".git", result)
        self.assertNotIn("node_modules", result)

    def test_gitignore_does_not_hide_files(self):
        # A sample directory is not a project; a stray .gitignore must not hide things.
        # ripgrep only honours one inside a repository, so make this look like one.
        os.makedirs(os.path.join(self.root, ".git"))
        self._file(".gitignore", "b.txt\n")
        self.assertIn("b.txt", self._rows(mode="files"))

    def test_hidden_files_are_searched(self):
        self._file(".hidden", "alpha\n")
        self.assertIn(".hidden:1:alpha", self._rows(pattern="alpha"))

    def test_a_symlink_out_of_the_workspace_is_not_followed(self):
        outside = os.path.join(self.top, "outside")
        os.makedirs(outside)
        with open(os.path.join(outside, "secret.txt"), "w", encoding="utf-8") as f:
            f.write("alpha secret\n")
        os.symlink(outside, os.path.join(self.root, "linkdir"))
        os.symlink(os.path.join(outside, "secret.txt"), os.path.join(self.root, "linkfile"))
        result = self._search(pattern="secret")
        self.assertNotIn("alpha secret", result)

    def test_a_path_outside_is_refused(self):
        self.assertIn("outside the workspace", self._search(pattern="x", path="/etc"))

    def test_a_missing_path_is_explained(self):
        self.assertIn("does not exist", self._search(pattern="x", path="nope"))

    def test_undo_backups_are_not_searched(self):
        scratch = self.session.files.workspace.scratch_root
        os.makedirs(os.path.join(scratch, ".backups"))
        with open(os.path.join(scratch, ".backups", "tok"), "w", encoding="utf-8") as f:
            f.write("alpha\n")
        with open(os.path.join(scratch, "note.txt"), "w", encoding="utf-8") as f:
            f.write("alpha\n")
        self.assertEqual(self._rows(pattern="alpha", path="scratch"), ["note.txt:1:alpha"])

    def test_a_long_line_is_clipped(self):
        self._file("wide.txt", "alpha" + "x" * 5000 + "\n")
        row = next(r for r in self._rows(pattern="alpha", glob="wide.txt"))
        self.assertLess(len(row), 600)
        self.assertIn("line continues", row)

    def test_rows_are_paged(self):
        self._file("many.txt", "".join(f"hit {n}\n" for n in range(300)))
        first = self._search(pattern="hit", glob="many.txt", limit=10)
        self.assertIn("many.txt:10:hit 9", first)
        self.assertNotIn("many.txt:11:hit 10", first)
        self.assertIn("offset=10", first)
        second = self._search(pattern="hit", glob="many.txt", offset=10, limit=10)
        self.assertIn("many.txt:11:hit 10", second)

    def test_a_large_result_stays_in_budget_with_a_handle(self):
        self._file("many.txt", "".join(f"hit {n} ".ljust(480, "x") + "\n" for n in range(300)))
        result = self._search(pattern="hit", glob="many.txt", limit=200)
        self.assertLessEqual(len(result), INLINE_RESULT_BUDGET)
        self.assertIn("artifact://1", result)

    def test_scratch_with_nothing_in_it_yet(self):
        self.assertIn("does not exist", self._search(pattern="x", path="scratch"))

    def test_a_bad_mode(self):
        self.assertIn("mode must be", self._search(pattern="x", mode="grep"))

    def test_content_needs_a_pattern(self):
        self.assertIn("pattern is required", self._search())

    def test_no_workspace_explains_itself(self):
        self.assertIn("no workspace", self.registry.execute("search_files", {"pattern": "x"}))


class TestWithPython(_Behaviours, _SearchTest):
    engine = "python"

    def test_stops_at_the_file_limit_and_says_so(self):
        with patch("rikugan.tools.file_search._MAX_SCANNED_FILES", 1):
            result = self._search(pattern="alpha")
        self.assertIn("stopped early", result)
        self.assertIn("1 file limit", result)

    def test_stops_at_the_deadline_and_says_so(self):
        with patch("rikugan.tools.file_search._SEARCH_SECONDS", -1):
            result = self._search(pattern="alpha")
        self.assertIn("stopped early", result)

    def test_a_complete_search_claims_nothing(self):
        self.assertNotIn("stopped early", self._search(pattern="alpha"))

    def test_a_listing_cut_at_the_limit_says_so(self):
        with patch("rikugan.tools.file_search._MAX_SCANNED_FILES", 1):
            self.assertIn("stopped early", self._search(mode="files"))

    def test_an_oversized_file_is_skipped(self):
        with patch("rikugan.tools.file_search._MAX_FILE_BYTES", 5):
            self.assertIn("No matches.", self._search(pattern="alpha"))


@unittest.skipUnless(HAS_RIPGREP, "ripgrep is not installed")
class TestWithRipgrep(_Behaviours, _SearchTest):
    engine = "ripgrep"

    def test_stops_at_the_output_cap_and_says_so(self):
        self._file("many.txt", "".join(f"hit {n}\n" for n in range(5000)))
        with patch("rikugan.tools.file_search._MAX_OUTPUT_BYTES", 1000), patch(
            "rikugan.tools.file_search._READ_CHUNK", 100
        ):
            result = self._search(pattern="hit")
        self.assertIn("stopped early", result)

    def test_never_runs_through_a_shell(self):
        # The pattern is the model's: metacharacters must stay text.
        marker = os.path.join(self.top, "pwned")
        self._search(pattern=f"$(touch {marker})")
        self._search(pattern=f"x; touch {marker}")
        self.assertFalse(os.path.exists(marker))

    def test_a_complete_search_claims_nothing(self):
        self.assertNotIn("stopped early", self._search(pattern="alpha"))

    def test_results_are_sorted_or_paging_would_shuffle_rows_between_calls(self):
        # ripgrep is parallel by default, so its order differs run to run. A small
        # fixture cannot show that reliably, which is why this checks the flag.
        argv = file_search._ripgrep_argv("x", "content", "", False, 0)
        self.assertEqual(argv[argv.index("--sort") + 1], "path")

    def test_a_complete_search_keeps_its_last_row(self):
        # Dropping a possibly partial row must only apply to a cut-off read.
        rows = self._rows(pattern="int alpha")
        self.assertEqual(rows, ["sub/d.c:1:int alpha;"])


class TestGlobMatcher(unittest.TestCase):
    def test_no_glob_matches_everything(self):
        self.assertTrue(file_search._glob_matcher("")("any/thing"))

    def test_a_name_matches_at_any_depth(self):
        self.assertTrue(file_search._glob_matcher("*.py")("a/b/c.py"))

    def test_nested_braces_expand(self):
        self.assertEqual(sorted(file_search._expand_braces("{a,b}.{c,d}")), ["a.c", "a.d", "b.c", "b.d"])


if __name__ == "__main__":
    unittest.main()

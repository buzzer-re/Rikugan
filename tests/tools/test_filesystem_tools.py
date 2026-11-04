"""read_file, write_file and edit_file, through the real registry."""

from __future__ import annotations

import os
import re
import stat
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(__file__)))
from tests.mocks.ida_mock import install_ida_mocks

install_ida_mocks()

for _mod_name in ("rikugan.core.types", "rikugan.core.config", "rikugan.state.session"):
    sys.modules.pop(_mod_name, None)

from rikugan.core.artifacts import INLINE_RESULT_BUDGET  # noqa: E402
from rikugan.core.chat_context import activate_session  # noqa: E402
from rikugan.core.errors import ToolNotFoundError, UndoNoLongerPossible  # noqa: E402
from rikugan.core.workspace import Workspace  # noqa: E402
from rikugan.state.session import SessionState  # noqa: E402
from rikugan.tools import filesystem  # noqa: E402
from rikugan.tools.registry import ToolRegistry  # noqa: E402


class _FileToolTest(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.top = os.path.realpath(tmp.name)
        self.bin_dir = os.path.join(self.top, "case")
        os.makedirs(self.bin_dir)
        self.binary = os.path.join(self.bin_dir, "sample.exe")
        self._write_bytes("sample.exe", b"MZ\x00\x00")
        self.session = SessionState()
        self.session.files.workspace = Workspace.for_database(
            self.binary, os.path.join(self.top, "config"), "abc", home=os.path.join(self.top, "home")
        )
        self.registry = ToolRegistry()
        self.registry.register_module(filesystem)

    def _call(self, name, **arguments):
        with activate_session(self.session):
            return self.registry.execute(name, arguments)

    def _path(self, name):
        return os.path.join(self.bin_dir, name)

    def _write_bytes(self, name, data):
        with open(self._path(name), "wb") as f:
            f.write(data)

    def _bytes(self, name):
        with open(self._path(name), "rb") as f:
            return f.read()

    def _make(self, name, text):
        """A file the model has read, ready for changing."""
        self._write_bytes(name, text.encode("utf-8"))
        self._call("read_file", file_path=name)


class TestReadFile(_FileToolTest):
    def test_lines_are_numbered(self):
        self._write_bytes("a.py", b"one\ntwo\n")
        result = self._call("read_file", file_path="a.py")
        self.assertIn("1\tone\n2\ttwo", result)

    def test_an_empty_file_says_so(self):
        self._write_bytes("a.py", b"")
        self.assertIn("(empty file)", self._call("read_file", file_path="a.py"))

    def test_a_page_by_offset_and_limit(self):
        self._write_bytes("a.py", b"".join(b"line %d\n" % n for n in range(1, 11)))
        result = self._call("read_file", file_path="a.py", offset=4, limit=2)
        self.assertIn("4\tline 4\n5\tline 5", result)
        self.assertNotIn("6\tline 6", result)
        self.assertIn("Continue with offset=6", result)

    def test_past_the_end_says_so(self):
        self._write_bytes("a.py", b"one\n")
        self.assertIn("past the end", self._call("read_file", file_path="a.py", offset=9))

    def test_the_last_page_says_it_is_the_end(self):
        self._write_bytes("a.py", b"one\ntwo\n")
        self.assertIn("the end", self._call("read_file", file_path="a.py", offset=2))

    def test_crlf_is_not_shown(self):
        self._write_bytes("a.py", b"one\r\ntwo\r\n")
        self.assertNotIn("\r", self._call("read_file", file_path="a.py", offset=1, limit=5))

    def test_form_feed_does_not_shift_line_numbers(self):
        self._write_bytes("a.py", b"one\x0cstill one\ntwo\n")
        self.assertIn("2\ttwo", self._call("read_file", file_path="a.py"))

    def test_a_binary_file_is_refused(self):
        result = self._call("read_file", file_path="sample.exe")
        self.assertIn("binary", result)

    def test_a_refusal_comes_back_as_text_the_model_can_act_on(self):
        result = self._call("read_file", file_path="/etc/passwd")
        self.assertTrue(result.startswith("Error: path refused"))
        self.assertIn("scratch/", result)

    def test_a_long_line_is_clipped_and_says_so(self):
        self._write_bytes("a.py", b"x" * 50_000 + b"\ntwo\n")
        result = self._call("read_file", file_path="a.py")
        self.assertIn("line continues for 48,000 more chars", result)
        self.assertIn("2\ttwo", result)


class TestReadingALargeFile(_FileToolTest):
    def setUp(self):
        super().setUp()
        self.count = 20_000
        self._write_bytes("big.txt", b"".join(b"line %d\n" % n for n in range(1, self.count + 1)))

    def test_shows_both_ends_with_real_line_numbers(self):
        result = self._call("read_file", file_path="big.txt")
        self.assertIn("1\tline 1\n", result)
        self.assertIn(f"{self.count}\tline {self.count}", result)

    def test_never_exceeds_its_budget(self):
        self.assertLessEqual(len(self._call("read_file", file_path="big.txt")), INLINE_RESULT_BUDGET)

    def test_the_notice_leads_to_the_rest(self):
        result = self._call("read_file", file_path="big.txt")
        next_offset = int(re.search(r'read_file\("artifact://1", offset=(\d+)\)', result).group(1))
        page = self._call("read_file", file_path="artifact://1", offset=next_offset, limit=1)
        self.assertIn(f"{next_offset}\tline {next_offset}", page)

    def test_paging_the_real_file_needs_no_artifact(self):
        result = self._call("read_file", file_path="big.txt", offset=5000, limit=2)
        self.assertIn("5000\tline 5000", result)
        self.assertNotIn("artifact://", result)

    def test_a_page_too_big_for_one_result_fills_it_and_says_where_to_continue(self):
        result = self._call("read_file", file_path="big.txt", offset=1, limit=self.count)
        self.assertLessEqual(len(result), INLINE_RESULT_BUDGET)
        last = int(re.search(r"Continue with offset=(\d+)", result).group(1)) - 1
        self.assertIn(f"{last}\tline {last}", result)
        self.assertNotIn(f"{last + 1}\tline {last + 1}", result)

    def test_a_missing_artifact_is_explained(self):
        self.assertIn("does not exist", self._call("read_file", file_path="artifact://9"))

    def test_the_registry_does_not_cut_it(self):
        # The default cap is 8000. Past it the tail and the end of the head would go.
        self.assertNotIn("truncated by global safety cap", self._call("read_file", file_path="big.txt"))


class TestWriteFile(_FileToolTest):
    def test_creates_a_new_file_and_its_parents(self):
        result = self._call("write_file", file_path="out/deep/a.py", content="x = 1\n")
        self.assertEqual(self._bytes("out/deep/a.py"), b"x = 1\n")
        self.assertIn("Created", result)

    def test_scratch_is_writable(self):
        self._call("write_file", file_path="scratch/a.py", content="x")
        scratch = self.session.files.workspace.scratch_root
        self.assertTrue(os.path.exists(os.path.join(scratch, "a.py")))

    def test_refuses_an_existing_file_it_has_not_read(self):
        self._write_bytes("a.py", b"precious")
        result = self._call("write_file", file_path="a.py", content="x")
        self.assertIn("read", result.lower())
        self.assertEqual(self._bytes("a.py"), b"precious")

    def test_replaces_one_it_has_read(self):
        self._make("a.py", "old")
        self.assertIn("Replaced", self._call("write_file", file_path="a.py", content="new"))
        self.assertEqual(self._bytes("a.py"), b"new")

    def test_refuses_one_that_changed_since_it_was_read(self):
        self._make("a.py", "old")
        os.utime(self._path("a.py"), ns=(1, 1))
        result = self._call("write_file", file_path="a.py", content="new")
        self.assertIn("changed on disk", result)
        self.assertEqual(self._bytes("a.py"), b"old")

    def test_can_write_again_without_rereading_its_own_write(self):
        self._call("write_file", file_path="a.py", content="one")
        self._call("write_file", file_path="a.py", content="two")
        self.assertEqual(self._bytes("a.py"), b"two")

    def test_refuses_the_analysed_binary(self):
        result = self._call("write_file", file_path="sample.exe", content="x")
        self.assertIn("refused", result)
        self.assertEqual(self._bytes("sample.exe"), b"MZ\x00\x00")

    def test_refuses_a_path_outside(self):
        outside = os.path.join(self.top, "outside.txt")
        self.assertIn("outside the workspace", self._call("write_file", file_path=outside, content="x"))
        self.assertFalse(os.path.exists(outside))

    def test_refuses_an_oversized_write(self):
        result = self._call("write_file", file_path="a.py", content="x" * (5 * 1024 * 1024 + 1))
        self.assertIn("limit", result)
        self.assertFalse(os.path.exists(self._path("a.py")))


class TestEditFile(_FileToolTest):
    def _edit(self, old, new, name="a.py", **kwargs):
        return self._call("edit_file", file_path=name, old_string=old, new_string=new, **kwargs)

    def test_replaces_a_unique_match(self):
        self._make("a.py", "alpha\nbeta\ngamma\n")
        self._edit("beta", "BETA")
        self.assertEqual(self._bytes("a.py"), b"alpha\nBETA\ngamma\n")

    def test_shows_the_result_in_context(self):
        self._make("a.py", "alpha\nbeta\ngamma\n")
        result = self._edit("beta", "BETA")
        self.assertIn("2\tBETA", result)
        self.assertIn("1\talpha", result)

    def test_a_large_replacement_shows_only_a_bounded_snippet(self):
        self._make("a.py", "alpha\nbeta\n")
        result = self._edit("beta", "\n".join(f"new {n}" for n in range(500)))
        self.assertIn("…", result)
        self.assertLess(result.count("\n"), 20)

    def test_refuses_a_file_it_has_not_read(self):
        self._write_bytes("a.py", b"alpha")
        self.assertIn("read", self._edit("alpha", "x").lower())
        self.assertEqual(self._bytes("a.py"), b"alpha")

    def test_refuses_a_file_changed_since_it_was_read(self):
        self._make("a.py", "alpha")
        os.utime(self._path("a.py"), ns=(1, 1))
        self.assertIn("changed on disk", self._edit("alpha", "x"))
        self.assertEqual(self._bytes("a.py"), b"alpha")

    def test_a_series_of_edits_needs_no_rereading(self):
        self._make("a.py", "alpha beta")
        self._edit("alpha", "one")
        self._edit("beta", "two")
        self.assertEqual(self._bytes("a.py"), b"one two")

    def test_no_match_fails_and_changes_nothing(self):
        self._make("a.py", "alpha")
        self.assertIn("not found", self._edit("nope", "x"))
        self.assertEqual(self._bytes("a.py"), b"alpha")

    def test_no_match_because_of_whitespace_says_so_but_does_not_apply(self):
        self._make("a.py", "def f():\n\treturn 1\n")
        result = self._edit("def f():\n    return 1", "x")
        self.assertIn("whitespace", result)
        self.assertEqual(self._bytes("a.py"), b"def f():\n\treturn 1\n")

    def test_no_match_because_of_copied_line_numbers_says_so(self):
        self._make("a.py", "alpha\nbeta\n")
        self.assertIn("line numbers", self._edit("1\talpha\n2\tbeta", "x"))
        self.assertEqual(self._bytes("a.py"), b"alpha\nbeta\n")

    def test_several_matches_fail_and_name_every_line(self):
        self._make("a.py", "x\nfoo\nx\nfoo\nx\nfoo\n")
        result = self._edit("foo", "bar")
        self.assertIn("3 times", result)
        self.assertIn("lines 2, 4, 6", result)
        self.assertEqual(self._bytes("a.py"), b"x\nfoo\nx\nfoo\nx\nfoo\n")

    def test_many_matches_are_summarised(self):
        self._make("a.py", "foo\n" * 50)
        self.assertIn("and 30 more", self._edit("foo", "bar"))

    def test_replace_all(self):
        self._make("a.py", "foo foo foo")
        result = self._edit("foo", "bar", replace_all=True)
        self.assertEqual(self._bytes("a.py"), b"bar bar bar")
        self.assertIn("3 replacements", result)

    def test_identical_strings_are_refused(self):
        self._make("a.py", "alpha")
        self.assertIn("identical", self._edit("alpha", "alpha"))

    def test_an_empty_old_string_is_refused(self):
        self._make("a.py", "alpha")
        self.assertIn("empty", self._edit("", "x"))
        self.assertEqual(self._bytes("a.py"), b"alpha")

    def test_regex_characters_are_literal(self):
        self._make("a.py", "a.b axb (c)")
        self._edit("a.b", "1")
        self.assertEqual(self._bytes("a.py"), b"1 axb (c)")

    def test_non_utf8_is_refused_rather_than_damaged(self):
        self._write_bytes("a.py", b"caf\xe9 alpha")
        self._call("read_file", file_path="a.py")
        self.assertIn("UTF-8", self._edit("alpha", "x"))
        self.assertEqual(self._bytes("a.py"), b"caf\xe9 alpha")

    def test_crlf_files_stay_crlf(self):
        self._make("a.py", "one\r\ntwo\r\nthree\r\n")
        self._edit("one\ntwo", "ONE\nTWO\nEXTRA")
        self.assertEqual(self._bytes("a.py"), b"ONE\r\nTWO\r\nEXTRA\r\nthree\r\n")

    def test_lf_files_stay_lf(self):
        self._make("a.py", "one\ntwo\n")
        self._edit("one", "ONE\nMORE")
        self.assertEqual(self._bytes("a.py"), b"ONE\nMORE\ntwo\n")

    def test_a_missing_final_newline_stays_missing(self):
        self._make("a.py", "one\ntwo")
        self._edit("two", "TWO")
        self.assertEqual(self._bytes("a.py"), b"one\nTWO")

    def test_a_final_newline_stays(self):
        self._make("a.py", "one\n")
        self._edit("one", "ONE")
        self.assertEqual(self._bytes("a.py"), b"ONE\n")

    def test_keeps_the_files_mode(self):
        self._make("run.sh", "#!/bin/sh\necho hi\n")
        os.chmod(self._path("run.sh"), 0o755)
        self._call("read_file", file_path="run.sh")
        self._edit("hi", "bye", name="run.sh")
        self.assertEqual(stat.S_IMODE(os.stat(self._path("run.sh")).st_mode), 0o755)

    def test_leaves_no_temporary_files(self):
        self._make("a.py", "alpha")
        self._edit("alpha", "x")
        self.assertEqual([n for n in os.listdir(self.bin_dir) if n.endswith(".tmp")], [])


class TestUndoRecords(_FileToolTest):
    def _undo_last(self):
        record = self.session.mutation_log.pop()
        return self._call(record.reverse_tool, **record.reverse_arguments)

    def test_an_edit_is_recorded_as_reversible(self):
        self._make("a.py", "alpha")
        self._call("edit_file", file_path="a.py", old_string="alpha", new_string="x")
        self.assertTrue(self.session.mutation_log[-1].reversible)

    def test_a_failed_edit_records_nothing(self):
        self._make("a.py", "alpha")
        self._call("edit_file", file_path="a.py", old_string="nope", new_string="x")
        self.assertEqual(self.session.mutation_log, [])

    def test_undoing_an_edit_restores_the_bytes(self):
        self._make("a.py", "alpha\r\nbeta\n")
        self._call("edit_file", file_path="a.py", old_string="beta", new_string="x")
        self._undo_last()
        self.assertEqual(self._bytes("a.py"), b"alpha\r\nbeta\n")

    def test_a_chain_of_edits_undoes_one_at_a_time(self):
        # Undoing rewrites the file; if that changed its mtime away from what the
        # earlier edit recorded, the next undo would think the user had touched it.
        self._make("a.py", "zero")
        self._call("edit_file", file_path="a.py", old_string="zero", new_string="one")
        self._call("edit_file", file_path="a.py", old_string="one", new_string="two")
        self._call("edit_file", file_path="a.py", old_string="two", new_string="three")
        self._undo_last()
        self.assertEqual(self._bytes("a.py"), b"two")
        self._undo_last()
        self.assertEqual(self._bytes("a.py"), b"one")
        self._undo_last()
        self.assertEqual(self._bytes("a.py"), b"zero")

    def test_it_can_edit_again_after_an_undo(self):
        self._make("a.py", "alpha")
        self._call("edit_file", file_path="a.py", old_string="alpha", new_string="x")
        self._undo_last()
        self._call("edit_file", file_path="a.py", old_string="alpha", new_string="y")
        self.assertEqual(self._bytes("a.py"), b"y")

    def test_undoing_a_created_file_removes_it(self):
        self._call("write_file", file_path="new.py", content="x")
        self._undo_last()
        self.assertFalse(os.path.exists(self._path("new.py")))

    def test_undoing_a_replacement_restores_the_old_file(self):
        self._make("a.py", "old")
        self._call("write_file", file_path="a.py", content="new")
        self._undo_last()
        self.assertEqual(self._bytes("a.py"), b"old")

    def test_a_file_changed_since_is_left_alone(self):
        self._make("a.py", "alpha")
        self._call("edit_file", file_path="a.py", old_string="alpha", new_string="agent")
        self._write_bytes("a.py", b"the user's newer work")
        os.utime(self._path("a.py"), ns=(1, 1))
        with self.assertRaises(UndoNoLongerPossible):
            self._undo_last()
        self.assertEqual(self._bytes("a.py"), b"the user's newer work")

    def test_a_created_file_changed_since_is_not_deleted(self):
        self._call("write_file", file_path="new.py", content="x")
        self._write_bytes("new.py", b"the user's newer work")
        os.utime(self._path("new.py"), ns=(1, 1))
        with self.assertRaises(UndoNoLongerPossible):
            self._undo_last()
        self.assertTrue(os.path.exists(self._path("new.py")))

    def test_a_discarded_backup_cannot_be_retried(self):
        self._make("a.py", "alpha")
        self._call("edit_file", file_path="a.py", old_string="alpha", new_string="x")
        backup = self.session.mutation_log[-1].reverse_arguments["backup"]
        os.remove(os.path.join(self.session.files.workspace.backups_dir, backup))
        with self.assertRaises(UndoNoLongerPossible):
            self._undo_last()
        self.assertEqual(self._bytes("a.py"), b"x")


class TestInternalTool(_FileToolTest):
    def test_the_model_is_not_told_about_it(self):
        self.assertNotIn("restore_file_backup", self.registry.list_names())
        declared = [t["function"]["name"] for t in self.registry.to_provider_format()]
        self.assertNotIn("restore_file_backup", declared)
        self.assertIn("edit_file", declared)

    def test_settings_do_not_list_it(self):
        self.assertNotIn("restore_file_backup", [d.name for d in self.registry.list_tools()])

    def test_undo_can_still_reach_it(self):
        self.assertIsNotNone(self.registry.get("restore_file_backup"))
        with self.assertRaises(ToolNotFoundError):
            self.registry.execute("no_such_tool", {})


class TestOutsideAChat(unittest.TestCase):
    def test_a_tool_with_no_active_chat_explains_itself(self):
        registry = ToolRegistry()
        registry.register_module(filesystem)
        self.assertIn("inside a chat", registry.execute("read_file", {"file_path": "a"}))

    def test_a_chat_with_no_workspace_explains_itself(self):
        registry = ToolRegistry()
        registry.register_module(filesystem)
        with activate_session(SessionState()):
            self.assertIn("no workspace", registry.execute("read_file", {"file_path": "a"}))


if __name__ == "__main__":
    unittest.main()

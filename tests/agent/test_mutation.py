"""Tests for the mutation tracking module."""

from __future__ import annotations

import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(__file__)))
from tests.mocks.ida_mock import install_ida_mocks
install_ida_mocks()

from rikugan.agent.mutation import (
    MutationRecord,
    build_reverse_record,
    capture_pre_state,
    mutation_succeeded,
)


class TestBuildReverseRecord(unittest.TestCase):
    """What each undo would call. test_mutation_contract checks these against real tools."""

    def test_rename_function_restores_the_name_it_read(self):
        rec = build_reverse_record(
            "rename_function",
            {"address": "0x401000", "new_name": "main"},
            pre_state={"old_name": "sub_401000"},
        )
        self.assertTrue(rec.reversible)
        self.assertEqual(rec.reverse_arguments, {"address": "0x401000", "new_name": "sub_401000"})
        self.assertIn("sub_401000", rec.description)

    def test_rename_function_without_its_old_name_cannot_be_undone(self):
        rec = build_reverse_record(
            "rename_function",
            {"address": "0x401000", "new_name": "main"},
            pre_state={"_capture_failed": "No function at 0x401000"},
        )
        self.assertFalse(rec.reversible)
        self.assertIn("No function", rec.irreversible_reason)

    def test_rename_variable_swaps_the_names(self):
        rec = build_reverse_record(
            "rename_variable",
            {"func_address": "0x401000", "old_name": "var_10", "new_name": "counter"},
        )
        self.assertEqual(
            rec.reverse_arguments,
            {"func_address": "0x401000", "old_name": "counter", "new_name": "var_10"},
        )

    def test_set_comment_restores_the_old_one(self):
        rec = build_reverse_record(
            "set_comment",
            {"address": "0x401000", "comment": "new"},
            pre_state={"old_comment": "old"},
        )
        self.assertEqual(rec.reverse_tool, "set_comment")
        self.assertEqual(rec.reverse_arguments["comment"], "old")

    def test_set_comment_with_none_before_writes_it_empty(self):
        # Writing an empty comment is how both hosts remove one.
        rec = build_reverse_record(
            "set_comment",
            {"address": "0x401000", "comment": "new"},
            pre_state={"old_comment": ""},
        )
        self.assertTrue(rec.reversible)
        self.assertEqual(rec.reverse_arguments["comment"], "")

    def test_set_comment_keeps_to_the_same_slot(self):
        rec = build_reverse_record(
            "set_comment",
            {"address": "0x401000", "comment": "new", "repeatable": True},
            pre_state={"old_comment": ""},
        )
        self.assertIs(rec.reverse_arguments["repeatable"], True)

    def test_a_comment_whose_old_value_was_not_read_cannot_be_undone(self):
        # Treating that as "had none" would delete a comment the user wrote.
        rec = build_reverse_record(
            "set_comment",
            {"address": "0x401000", "comment": "new"},
            pre_state={"_capture_failed": "boom"},
        )
        self.assertFalse(rec.reversible)
        self.assertIn("boom", rec.irreversible_reason)

    def test_set_function_comment(self):
        rec = build_reverse_record(
            "set_function_comment",
            {"address": "0x401000", "comment": "new"},
            pre_state={"old_comment": "old"},
        )
        self.assertEqual(rec.reverse_arguments["address"], "0x401000")
        self.assertEqual(rec.reverse_arguments["comment"], "old")

    def test_rename_address_with_old_name(self):
        rec = build_reverse_record(
            "rename_address",
            {"address": "0x600000", "new_name": "g_counter"},
            pre_state={"old_name": "data_600000"},
        )
        self.assertTrue(rec.reversible)
        self.assertEqual(rec.reverse_arguments["new_name"], "data_600000")

    def test_rename_address_without_old_name(self):
        rec = build_reverse_record("rename_address", {"address": "0x600000", "new_name": "g_counter"})
        self.assertFalse(rec.reversible)

    def test_set_function_prototype(self):
        rec = build_reverse_record(
            "set_function_prototype",
            {"address": "0x401000", "prototype": "int main(int argc, char **argv)"},
            pre_state={"old_prototype": "int sub_401000()"},
        )
        self.assertEqual(rec.reverse_arguments, {"address": "0x401000", "prototype": "int sub_401000()"})

    def test_set_type_is_declared_irreversible(self):
        # Neither host exposes a type-at-address getter, so the prior type
        # cannot be read. Saying so beats offering an undo that no-ops.
        rec = build_reverse_record("set_type", {"address": "0x401000", "type_string": "int *"})
        self.assertFalse(rec.reversible)
        self.assertIn("no reverse operation", rec.irreversible_reason)

    def test_unknown_tool_not_reversible(self):
        rec = build_reverse_record("execute_python", {"code": "print('hello')"})
        self.assertFalse(rec.reversible)


class TestCapturePreState(unittest.TestCase):
    def _capture(self, tool, args, answers):
        self.calls = []

        def executor(name, call_args):
            self.calls.append((name, call_args))
            return answers[name]

        return capture_pre_state(tool, args, executor)

    def test_rename_function_reads_the_name(self):
        pre = self._capture("rename_function", {"address": "0x1"}, {"read_function_name": "sub_1"})
        self.assertEqual(pre["old_name"], "sub_1")

    def test_set_comment_reads_the_same_slot_it_will_write(self):
        self._capture("set_comment", {"address": "0x1", "repeatable": True}, {"read_comment": ""})
        self.assertEqual(self.calls, [("read_comment", {"address": "0x1", "repeatable": True})])

    def test_set_function_comment_reads_the_old_one(self):
        pre = self._capture("set_function_comment", {"address": "0x1"}, {"read_function_comment": "old"})
        self.assertEqual(pre["old_comment"], "old")

    def test_an_address_with_no_name_is_not_named_after_the_message(self):
        # get_name_at says "No name at 0x1". Taken as the old name, undo would
        # label the address with that sentence.
        pre = self._capture("rename_address", {"address": "0x1"}, {"get_name_at": "No name at 0x1"})
        self.assertNotIn("old_name", pre)
        self.assertIn("no name to restore", pre["_capture_failed"])

    def test_a_failed_decompile_is_not_taken_for_a_comment(self):
        pre = self._capture(
            "set_pseudocode_comment",
            {"func_address": "0x1", "target_address": "0x2"},
            {"get_pseudocode_comment": "Decompilation failed at 0x1: nope"},
        )
        self.assertNotIn("old_comment", pre)
        self.assertIn("Decompilation failed", pre["_capture_failed"])

    def test_a_function_with_no_prototype_says_so(self):
        pre = self._capture("set_function_prototype", {"address": "0x1"}, {"read_function_prototype": ""})
        self.assertIn("no prototype", pre["_capture_failed"])

    def test_rename_variable_needs_nothing_read(self):
        args = {"func_address": "0x1", "old_name": "a", "new_name": "b"}
        self.assertEqual(self._capture("rename_variable", args, {}), {})

    def test_executor_failure_graceful(self):
        def executor(name, args):
            raise RuntimeError("tool not available")

        # Must not raise — and must record why, so the mutation row can say
        # the change is not undoable instead of failing silently at undo time.
        pre = capture_pre_state("set_comment", {"address": "0x1000"}, executor)
        self.assertIn("tool not available", pre["_capture_failed"])


class TestMutationSucceeded(unittest.TestCase):
    def test_success_messages(self):
        for tool, text in (
            ("rename_function", "Renamed 0x1: a → b"),
            ("rename_variable", "Renamed variable: a → b"),
            ("rename_address", "Named 0x1: a → b"),
            ("set_comment", "Set comment at 0x1: "),
            ("set_function_comment", "Set function comment at 0x1"),
            ("set_pseudocode_comment", "Set pseudocode comment at 0x2 in function 0x1:\nx"),
            ("set_function_prototype", "Set prototype at 0x1: int f()"),
        ):
            with self.subTest(tool):
                self.assertTrue(mutation_succeeded(tool, text))

    def test_every_way_they_report_failure(self):
        for tool, text in (
            ("rename_function", "No function at 0x1"),
            ("rename_function", "Failed to rename function at 0x1"),
            ("rename_variable", "Hex-Rays decompiler not available"),
            ("rename_variable", "Decompilation failed: x"),
            ("rename_variable", "No decompilation for 0x1"),
            ("rename_variable", "Failed to rename variable a"),
            ("rename_variable", "Variable 'a' not found in function at 0x1"),
            ("rename_variable", "Variable renaming not available for this function"),
            ("rename_variable", "Variable renaming is not supported by this Binary Ninja API version"),
            ("rename_address", "Failed to set name at 0x1"),
            ("set_comment", "Failed to set comment at 0x1"),
            ("set_function_comment", "No function at 0x1"),
            ("set_pseudocode_comment", "Address 0x2 is outside function 0x1–0x3"),
            ("set_function_prototype", "Failed to set prototype. Check syntax: x"),
            ("set_function_prototype", "idc module not available"),
        ):
            with self.subTest(text):
                self.assertFalse(mutation_succeeded(tool, text))

    def test_an_unknown_message_counts_as_failure(self):
        self.assertFalse(mutation_succeeded("rename_function", "something new"))

    def test_other_tools_are_taken_at_their_word(self):
        self.assertTrue(mutation_succeeded("write_bytes", "whatever it says"))
        self.assertTrue(mutation_succeeded("restore_file_backup", "Restored a.py"))


class TestMutationRecord(unittest.TestCase):
    """Tests for MutationRecord dataclass."""

    def test_defaults(self):
        rec = MutationRecord(
            tool_name="test",
            arguments={},
            reverse_tool="test_reverse",
            reverse_arguments={},
        )
        self.assertTrue(rec.reversible)
        self.assertGreater(rec.timestamp, 0)
        self.assertEqual(rec.description, "")

    def test_non_reversible(self):
        rec = MutationRecord(
            tool_name="execute_python",
            arguments={"code": "x=1"},
            reverse_tool="",
            reverse_arguments={},
            reversible=False,
        )
        self.assertFalse(rec.reversible)


if __name__ == "__main__":
    unittest.main()

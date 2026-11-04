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
)


class TestBuildReverseRecord(unittest.TestCase):
    """Tests for build_reverse_record() generating undo operations."""

    def test_rename_function(self):
        rec = build_reverse_record(
            "rename_function",
            {"old_name": "sub_401000", "new_name": "main"},
        )
        self.assertIsNotNone(rec)
        self.assertTrue(rec.reversible)
        self.assertEqual(rec.reverse_tool, "rename_function")
        self.assertEqual(rec.reverse_arguments["old_name"], "main")
        self.assertEqual(rec.reverse_arguments["new_name"], "sub_401000")

    def test_rename_variable(self):
        rec = build_reverse_record(
            "rename_variable",
            {"function_name": "main", "variable_name": "var_10", "new_name": "counter"},
        )
        self.assertIsNotNone(rec)
        self.assertTrue(rec.reversible)
        self.assertEqual(rec.reverse_tool, "rename_variable")
        self.assertEqual(rec.reverse_arguments["variable_name"], "counter")
        self.assertEqual(rec.reverse_arguments["new_name"], "var_10")

    def test_set_comment_with_existing(self):
        rec = build_reverse_record(
            "set_comment",
            {"address": "0x401000", "comment": "new comment"},
            pre_state={"old_comment": "old comment"},
        )
        self.assertIsNotNone(rec)
        self.assertTrue(rec.reversible)
        self.assertEqual(rec.reverse_tool, "set_comment")
        self.assertEqual(rec.reverse_arguments["comment"], "old comment")

    def test_set_comment_without_existing(self):
        rec = build_reverse_record(
            "set_comment",
            {"address": "0x401000", "comment": "new comment"},
            pre_state={"old_comment": ""},
        )
        self.assertIsNotNone(rec)
        self.assertTrue(rec.reversible)
        self.assertEqual(rec.reverse_tool, "delete_comment")
        self.assertEqual(rec.reverse_arguments["address"], "0x401000")

    def test_set_function_comment_with_existing(self):
        rec = build_reverse_record(
            "set_function_comment",
            {"function_name": "main", "comment": "new"},
            pre_state={"old_comment": "old"},
        )
        self.assertTrue(rec.reversible)
        self.assertEqual(rec.reverse_tool, "set_function_comment")
        self.assertEqual(rec.reverse_arguments["comment"], "old")

    def test_set_function_comment_without_existing(self):
        rec = build_reverse_record(
            "set_function_comment",
            {"function_name": "main", "comment": "new"},
            pre_state={"old_comment": ""},
        )
        self.assertTrue(rec.reversible)
        self.assertEqual(rec.reverse_tool, "delete_function_comment")

    def test_rename_address_with_old_name(self):
        rec = build_reverse_record(
            "rename_address",
            {"address": "0x600000", "new_name": "g_counter"},
            pre_state={"old_name": "data_600000"},
        )
        self.assertTrue(rec.reversible)
        self.assertEqual(rec.reverse_arguments["new_name"], "data_600000")

    def test_rename_address_without_old_name(self):
        rec = build_reverse_record(
            "rename_address",
            {"address": "0x600000", "new_name": "g_counter"},
        )
        self.assertFalse(rec.reversible)

    def test_set_function_prototype(self):
        rec = build_reverse_record(
            "set_function_prototype",
            {"name_or_address": "main", "prototype": "int main(int argc, char **argv)"},
            pre_state={"old_prototype": "int sub_401000()"},
        )
        self.assertTrue(rec.reversible)
        self.assertEqual(rec.reverse_arguments["prototype"], "int sub_401000()")

    def test_set_type_is_declared_irreversible(self):
        # Neither host exposes a type-at-address getter, so the prior type
        # cannot be read. Saying so beats offering an undo that no-ops.
        rec = build_reverse_record("set_type", {"address": "0x401000", "type_string": "int *"})
        self.assertFalse(rec.reversible)
        self.assertIn("no reverse operation", rec.irreversible_reason)

    def test_unknown_tool_not_reversible(self):
        rec = build_reverse_record(
            "execute_python",
            {"code": "print('hello')"},
        )
        self.assertIsNotNone(rec)
        self.assertFalse(rec.reversible)

    def test_description_populated(self):
        rec = build_reverse_record(
            "rename_function",
            {"old_name": "sub_401000", "new_name": "main"},
        )
        self.assertIn("sub_401000", rec.description)
        self.assertIn("main", rec.description)


class TestCapturePreState(unittest.TestCase):
    """Tests for capture_pre_state() fetching current state before mutation."""

    def test_set_comment_captures_old(self):
        def mock_executor(name, args):
            if name == "get_comment":
                return "existing comment"
            return ""

        pre = capture_pre_state("set_comment", {"address": "0x1000"}, mock_executor)
        self.assertEqual(pre["old_comment"], "existing comment")

    def test_set_function_comment_captures_old(self):
        def mock_executor(name, args):
            if name == "get_function_comment":
                return "func comment"
            return ""

        pre = capture_pre_state(
            "set_function_comment",
            {"function_name": "main"},
            mock_executor,
        )
        self.assertEqual(pre["old_comment"], "func comment")

    def test_rename_function_no_prestate_needed(self):
        def mock_executor(name, args):
            raise RuntimeError("should not be called")

        pre = capture_pre_state("rename_function", {"old_name": "a", "new_name": "b"}, mock_executor)
        self.assertEqual(pre, {})

    def test_executor_failure_graceful(self):
        def mock_executor(name, args):
            raise RuntimeError("tool not available")

        # Must not raise — and must record why, so the mutation row can say
        # the change is not undoable instead of failing silently at undo time.
        pre = capture_pre_state("set_comment", {"address": "0x1000"}, mock_executor)
        self.assertIn("tool not available", pre["_capture_failed"])


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

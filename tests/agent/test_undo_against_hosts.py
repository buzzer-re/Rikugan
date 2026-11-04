"""Mutate, then /undo on a later message, through the real tools of each host.

Only the host API underneath is faked, as a small in-memory database. The tools,
registry, loop and undo path are real, which is what the argument mismatches that
broke undo needed to be caught by.
"""

from __future__ import annotations

import os
import sys
import tempfile
import types
import unittest
from unittest.mock import MagicMock, patch

sys.path.insert(0, os.path.dirname(os.path.dirname(__file__)))
from tests.mocks.ida_mock import install_ida_mocks

install_ida_mocks()

for _mod_name in ("rikugan.core.types", "rikugan.core.config", "rikugan.state.session"):
    sys.modules.pop(_mod_name, None)

from rikugan.agent.loop import AgentLoop  # noqa: E402
from rikugan.agent.turn import TurnEventType  # noqa: E402
from rikugan.binja.tools import annotations as bn_annotations  # noqa: E402
from rikugan.binja.tools import navigation as bn_navigation  # noqa: E402
from rikugan.binja.tools import types_tools as bn_types  # noqa: E402
from rikugan.core.config import RikuganConfig  # noqa: E402
from rikugan.core.types import ToolCall  # noqa: E402
from rikugan.ida.tools import annotations as ida_annotations  # noqa: E402
from rikugan.ida.tools import navigation as ida_navigation  # noqa: E402
from rikugan.ida.tools import types_tools as ida_types  # noqa: E402
from rikugan.state.session import SessionState  # noqa: E402
from rikugan.tools.registry import ToolRegistry  # noqa: E402

FUNC = 0x401000
MID_FUNC = 0x401010
DATA = 0x600000


class _Database:
    """What a host keeps, whichever host's API is reading it."""

    def __init__(self):
        self.names = {FUNC: "sub_401000", DATA: "data_600000"}
        self.comments = {}
        self.function_comment = ""
        self.prototype = "int __cdecl(int a1)"
        self.writes_fail = False


def _set_or_clear(store, key, value):
    if value:
        store[key] = value
    else:
        store.pop(key, None)


class _Host:
    def install(self, test, db):
        raise NotImplementedError


class _Ida(_Host):
    def install(self, test, db):
        func = types.SimpleNamespace(start_ea=FUNC)

        def set_name(ea, name, flags):
            if db.writes_fail:
                return False
            db.names[ea] = name
            return True

        def set_func_cmt(_func, comment, _repeatable):
            db.function_comment = comment
            return True

        def set_cmt(ea, comment, repeatable):
            _set_or_clear(db.comments, (ea, bool(repeatable)), comment)
            return True

        def set_type(_ea, prototype):
            db.prototype = prototype
            return True

        ida_funcs = types.SimpleNamespace(
            get_func=lambda ea: func if FUNC <= ea < FUNC + 0x100 else None,
            get_func_cmt=lambda _func, _repeatable: db.function_comment or None,
            set_func_cmt=set_func_cmt,
        )
        ida_name = types.SimpleNamespace(
            get_name=lambda ea: db.names.get(ea, ""), set_name=set_name, SN_NOWARN=0, SN_NOCHECK=0
        )
        idc = types.SimpleNamespace(
            get_cmt=lambda ea, repeatable: db.comments.get((ea, bool(repeatable))),
            set_cmt=set_cmt,
            get_type=lambda _ea: db.prototype or None,
            SetType=set_type,
        )
        for module, attrs in (
            (ida_annotations, {"ida_funcs": ida_funcs, "ida_name": ida_name, "idc": idc}),
            (ida_navigation, {"ida_funcs": ida_funcs, "ida_name": ida_name}),
            (ida_types, {"idc": idc}),
        ):
            for attr, value in attrs.items():
                patcher = patch.object(module, attr, value)
                patcher.start()
                test.addCleanup(patcher.stop)
        return (ida_annotations, ida_navigation, ida_types)


class _BinaryNinja(_Host):
    def install(self, test, db):
        class _Function:
            start = FUNC
            name = property(lambda self: db.names.get(FUNC, ""))
            comment = property(
                lambda self: db.function_comment, lambda self, value: setattr(db, "function_comment", value)
            )
            type = property(lambda self: db.prototype or None)

            def set_user_type(self, value):
                db.prototype = value

        func = _Function()

        def rename_symbol_at(_bv, ea, name):
            if db.writes_fail:
                return False
            db.names[ea] = name
            return True

        bv = types.SimpleNamespace(
            get_comment_at=lambda ea: db.comments.get((ea, False), ""),
            set_comment_at=lambda ea, comment: _set_or_clear(db.comments, (ea, False), comment),
            get_symbol_at=lambda ea: types.SimpleNamespace(full_name=db.names[ea]) if ea in db.names else None,
        )
        get_function_at = lambda _bv, ea: func if FUNC <= ea < FUNC + 0x100 else None  # noqa: E731
        for module, attrs in (
            (
                bn_annotations,
                {"require_bv": lambda: bv, "get_function_at": get_function_at, "rename_symbol_at": rename_symbol_at},
            ),
            (bn_navigation, {"require_bv": lambda: bv, "_get_name_at": lambda _bv, ea: db.names.get(ea, "")}),
            (
                bn_types,
                {
                    "require_bv": lambda: bv,
                    "get_function_at": get_function_at,
                    "parse_type_string": lambda _bv, source: (source, ""),
                },
            ),
        ):
            for attr, value in attrs.items():
                patcher = patch.object(module, attr, value)
                patcher.start()
                test.addCleanup(patcher.stop)
        return (bn_annotations, bn_navigation, bn_types)


class _Round(unittest.TestCase):
    host: _Host

    def setUp(self):
        self.db = _Database()
        self.registry = ToolRegistry()
        for module in self.host.install(self, self.db):
            self.registry.register_module(module)
        self.config = RikuganConfig()
        self.config._config_dir = tempfile.mkdtemp()
        self.config.auto_context = False
        self.session = SessionState()

    def _loop(self):
        return AgentLoop(MagicMock(), self.registry, self.config, self.session)

    def _mutate(self, name, **arguments):
        tool_call = ToolCall(id="c", name=name, arguments=arguments)
        generator = self._loop()._execute_single_tool(tool_call)
        events = []
        while True:
            try:
                events.append(next(generator))
            except StopIteration as stop:
                self.assertFalse(stop.value.is_error, stop.value.content)
                return events

    def _undo(self, count=""):
        # A new loop each time: that is what /undo gets, being a message of its own.
        return list(self._loop()._handle_undo_command(f"/undo {count}"))[-1].text


class _Behaviours:
    def test_renaming_a_function_undoes(self):
        self._mutate("rename_function", address=hex(FUNC), new_name="main")
        self.assertEqual(self.db.names[FUNC], "main")
        self.assertIn("Undid 1", self._undo())
        self.assertEqual(self.db.names[FUNC], "sub_401000")

    def test_renaming_from_inside_a_function_undoes(self):
        self._mutate("rename_function", address=hex(MID_FUNC), new_name="main")
        self._undo()
        self.assertEqual(self.db.names[FUNC], "sub_401000")

    def test_a_chain_of_renames_undoes_step_by_step(self):
        self._mutate("rename_function", address=hex(FUNC), new_name="first")
        self._mutate("rename_function", address=hex(FUNC), new_name="second")
        self._undo()
        self.assertEqual(self.db.names[FUNC], "first")
        self._undo()
        self.assertEqual(self.db.names[FUNC], "sub_401000")

    def test_renaming_an_address_undoes(self):
        self._mutate("rename_address", address=hex(DATA), new_name="g_counter")
        self._undo()
        self.assertEqual(self.db.names[DATA], "data_600000")

    def test_naming_an_unnamed_address_says_it_cannot_be_undone(self):
        self._mutate("rename_address", address="0x700000", new_name="g_new")
        record = self.session.mutation_log[-1]
        self.assertFalse(record.reversible)
        self.assertIn("no name to restore", record.irreversible_reason)
        self._undo()
        self.assertEqual(self.db.names[0x700000], "g_new")

    def test_replacing_a_comment_undoes(self):
        self.db.comments[(DATA, False)] = "the user's note"
        self._mutate("set_comment", address=hex(DATA), comment="agent's note")
        self.assertEqual(self.db.comments[(DATA, False)], "agent's note")
        self._undo()
        self.assertEqual(self.db.comments[(DATA, False)], "the user's note")

    def test_adding_a_comment_undoes_to_none(self):
        self._mutate("set_comment", address=hex(DATA), comment="agent's note")
        self._undo()
        self.assertNotIn((DATA, False), self.db.comments)

    def test_replacing_a_function_comment_undoes(self):
        self.db.function_comment = "the user's note"
        self._mutate("set_function_comment", address=hex(FUNC), comment="agent's note")
        self.assertEqual(self.db.function_comment, "agent's note")
        self._undo()
        self.assertEqual(self.db.function_comment, "the user's note")

    def test_changing_a_prototype_undoes(self):
        self._mutate("set_function_prototype", address=hex(FUNC), prototype="void f(void)")
        self.assertEqual(self.db.prototype, "void f(void)")
        self._undo()
        self.assertEqual(self.db.prototype, "int __cdecl(int a1)")

    def test_a_function_with_no_prototype_says_it_cannot_be_undone(self):
        self.db.prototype = ""
        self._mutate("set_function_prototype", address=hex(FUNC), prototype="void f(void)")
        record = self.session.mutation_log[-1]
        self.assertFalse(record.reversible)
        self.assertIn("no prototype", record.irreversible_reason)

    def test_a_rename_that_failed_leaves_no_record(self):
        # It changed nothing, so there is nothing to list or undo.
        self.db.writes_fail = True
        events = self._mutate("rename_function", address=hex(FUNC), new_name="main")
        self.assertEqual(self.session.mutation_log, [])
        self.assertEqual([e for e in events if e.type == TurnEventType.MUTATION_RECORDED], [])

    def test_an_undo_that_failed_keeps_its_record_and_says_so(self):
        self._mutate("rename_function", address=hex(FUNC), new_name="main")
        self.db.writes_fail = True
        reply = self._undo()
        self.assertEqual(len(self.session.mutation_log), 1)
        self.assertIn("Failed to undo", reply)
        self.assertNotIn("Undid", reply)
        self.assertEqual(self.db.names[FUNC], "main")

        self.db.writes_fail = False
        self.assertIn("Undid 1", self._undo())
        self.assertEqual(self.db.names[FUNC], "sub_401000")

    def test_a_mixed_history_undoes_newest_first(self):
        self._mutate("rename_function", address=hex(FUNC), new_name="main")
        self._mutate("set_comment", address=hex(DATA), comment="note")
        self.assertIn("Undid 2", self._undo(2))
        self.assertEqual(self.db.names[FUNC], "sub_401000")
        self.assertEqual(self.db.comments, {})

    def test_the_model_cannot_call_the_readers(self):
        tool_call = ToolCall(id="c", name="read_comment", arguments={"address": hex(DATA)})
        generator = self._loop()._execute_single_tool(tool_call)
        while True:
            try:
                next(generator)
            except StopIteration as stop:
                self.assertTrue(stop.value.is_error)
                return

    def test_the_readers_are_not_declared(self):
        declared = [t["function"]["name"] for t in self.registry.to_provider_format()]
        self.assertNotIn("read_comment", declared)
        self.assertIn("rename_function", declared)


class TestIda(_Behaviours, _Round):
    host = _Ida()

    def test_a_repeatable_comment_undoes_in_its_own_slot(self):
        self.db.comments[(DATA, False)] = "regular"
        self.db.comments[(DATA, True)] = "repeatable"
        self._mutate("set_comment", address=hex(DATA), comment="agent's", repeatable=True)
        self.assertEqual(self.db.comments[(DATA, True)], "agent's")
        self._undo()
        self.assertEqual(self.db.comments[(DATA, True)], "repeatable")
        self.assertEqual(self.db.comments[(DATA, False)], "regular")


class TestBinaryNinja(_Behaviours, _Round):
    host = _BinaryNinja()


if __name__ == "__main__":
    unittest.main()

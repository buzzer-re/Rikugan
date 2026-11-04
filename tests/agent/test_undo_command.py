"""What /undo does to the log, and what it tells the panel.

The panel lists the log, so every record leaving it has to be announced — and a
record has to stay when its mutation is still applied.
"""

from __future__ import annotations

import os
import sys
import tempfile
import unittest
from unittest.mock import MagicMock, call

sys.path.insert(0, os.path.dirname(os.path.dirname(__file__)))
from tests.mocks.ida_mock import install_ida_mocks

install_ida_mocks()

for _mod_name in ("rikugan.core.types", "rikugan.core.config", "rikugan.state.session"):
    sys.modules.pop(_mod_name, None)

from rikugan.agent.loop import AgentLoop  # noqa: E402
from rikugan.agent.turn import TurnEventType  # noqa: E402
from rikugan.core.config import RikuganConfig  # noqa: E402
from rikugan.core.errors import ToolError  # noqa: E402
from rikugan.core.types import MutationRecord  # noqa: E402
from rikugan.state.session import SessionState  # noqa: E402


def _record(name, reversible=True, reason=""):
    return MutationRecord(
        tool_name="rename_function",
        arguments={},
        reverse_tool="rename_function",
        reverse_arguments={"which": name},
        description=name,
        reversible=reversible,
        irreversible_reason=reason,
    )


class TestUndoCommand(unittest.TestCase):
    def _undo(self, command, *records, execute=None):
        cfg = RikuganConfig()
        cfg._config_dir = tempfile.mkdtemp()
        self.tools = MagicMock()
        self.tools.execute.return_value = "Renamed 0x1: a → b"
        if execute is not None:
            self.tools.execute.side_effect = execute
        self.session = SessionState()
        self.session.mutation_log.extend(records)
        loop = AgentLoop(MagicMock(), self.tools, cfg, self.session)
        self.events = list(loop._handle_undo_command(command))

    def _announced(self):
        return [e.text for e in self.events if e.type == TurnEventType.MUTATION_UNDONE]

    def _final_text(self):
        return self.events[-1].text

    def test_undoing_announces_the_record_that_left(self):
        self._undo("/undo", _record("a"))
        self.assertEqual(self._announced(), ["a"])
        self.assertEqual(self.session.mutation_log, [])

    def test_newest_goes_first_and_each_is_announced(self):
        self._undo("/undo 2", _record("old"), _record("new"))
        self.assertEqual(self._announced(), ["new", "old"])
        self.assertEqual(
            self.tools.execute.call_args_list,
            [call("rename_function", {"which": "new"}), call("rename_function", {"which": "old"})],
        )

    def test_only_as_many_as_asked(self):
        keep = _record("keep")
        self._undo("/undo", keep, _record("new"))
        self.assertEqual(self.session.mutation_log, [keep])

    def test_a_failed_reversal_keeps_the_record(self):
        # The mutation is still applied. Dropping the record would hide it from
        # the panel and make a retry impossible.
        record = _record("a")
        self._undo("/undo", record, execute=ToolError("database busy"))
        self.assertEqual(self.session.mutation_log, [record])
        self.assertEqual(self._announced(), [])
        self.assertIn("database busy", self._final_text())

    def test_a_failed_reversal_stops_older_ones_running_out_of_order(self):
        older, newer = _record("older"), _record("newer")
        self._undo("/undo 2", older, newer, execute=ToolError("database busy"))
        self.assertEqual(self.tools.execute.call_count, 1)
        self.assertEqual(self.session.mutation_log, [older, newer])

    def test_earlier_successes_stay_undone_when_a_later_one_fails(self):
        older, newer = _record("older"), _record("newer")
        self._undo("/undo 2", older, newer, execute=["Renamed 0x1: a → b", ToolError("database busy")])
        self.assertEqual(self.session.mutation_log, [older])
        self.assertEqual(self._announced(), ["newer"])
        self.assertIn("Undid 1", self._final_text())

    def test_a_reversal_reporting_failure_in_text_is_a_failure(self):
        # These tools return "Failed ..." rather than raising. Counting that as
        # undone would drop the record of a change that is still applied.
        record = _record("a")
        self._undo("/undo", record, execute=["Failed to rename function at 0x401000"])
        self.assertEqual(self.session.mutation_log, [record])
        self.assertEqual(self._announced(), [])
        self.assertIn("Failed to rename", self._final_text())
        self.assertNotIn("Undid", self._final_text())

    def test_an_irreversible_record_says_why_and_leaves(self):
        self._undo("/undo", _record("a", reversible=False, reason="no getter for it"))
        self.tools.execute.assert_not_called()
        self.assertEqual(self.session.mutation_log, [])
        self.assertEqual(self._announced(), ["a"])
        self.assertIn("no getter for it", self._final_text())


if __name__ == "__main__":
    unittest.main()

"""The mutation log must outlive the loop that wrote it.

An AgentLoop is constructed for every user message, and /undo is itself a user
message — so a log owned by the loop is always empty by the time /undo reads
it. That is why the feature could never undo anything, including through the
mutation panel's button, which submits the same /undo command.
"""

from __future__ import annotations

import os
import sys
import tempfile
import unittest
from unittest.mock import MagicMock

sys.path.insert(0, os.path.dirname(os.path.dirname(__file__)))
from tests.mocks.ida_mock import install_ida_mocks

install_ida_mocks()

for _mod_name in ("rikugan.core.types", "rikugan.core.config", "rikugan.state.session"):
    sys.modules.pop(_mod_name, None)

from rikugan.agent.loop import AgentLoop  # noqa: E402
from rikugan.core.config import RikuganConfig  # noqa: E402
from rikugan.core.types import MutationRecord  # noqa: E402
from rikugan.state.session import SessionState  # noqa: E402


def _record(name="rename_function"):
    return MutationRecord(
        tool_name=name,
        arguments={"old_name": "sub_401000", "new_name": "parse_header"},
        reverse_tool=name,
        reverse_arguments={"old_name": "parse_header", "new_name": "sub_401000"},
        description=f"{name} sub_401000 -> parse_header",
    )


def _loop(session):
    cfg = RikuganConfig()
    cfg._config_dir = tempfile.mkdtemp()
    return AgentLoop(MagicMock(), MagicMock(), cfg, session)


class TestMutationLogOutlivesTheLoop(unittest.TestCase):
    def test_a_later_loop_sees_an_earlier_loops_mutations(self):
        # This is the whole bug: the turn that records and the turn that undoes
        # are different AgentLoop objects over the same chat.
        session = SessionState()
        _loop(session)._mutation_log.append(_record())

        self.assertEqual(len(_loop(session)._mutation_log), 1)

    def test_the_log_belongs_to_the_session(self):
        session = SessionState()
        loop = _loop(session)
        loop._mutation_log.append(_record())
        self.assertIs(loop._mutation_log, session.mutation_log)

    def test_separate_chats_do_not_share_a_log(self):
        first, second = SessionState(), SessionState()
        _loop(first)._mutation_log.append(_record())
        self.assertEqual(len(second.mutation_log), 0)

    def test_the_log_is_not_written_to_disk(self):
        # A reverse call only means anything against a live database, and a
        # restored chat is replayed into a different one.
        import json

        from rikugan.state.history import SessionHistory

        cfg = RikuganConfig()
        cfg._config_dir = tempfile.mkdtemp()
        session = SessionState()
        session.mutation_log.append(_record())

        path = SessionHistory(cfg).save_session(session)
        with open(path, encoding="utf-8") as f:
            written = json.load(f)

        self.assertNotIn("mutation_log", written)


if __name__ == "__main__":
    unittest.main()

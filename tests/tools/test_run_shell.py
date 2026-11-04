"""run_shell: real processes, real signals."""

from __future__ import annotations

import os
import sys
import tempfile
import threading
import time
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
from rikugan.tools import shell  # noqa: E402
from rikugan.tools.registry import ToolRegistry  # noqa: E402


def _alive(pid):
    # A killed child of an exited group can linger as a zombie, so ask its state.
    try:
        with open(f"/proc/{pid}/stat", encoding="utf-8") as f:
            return f.read().rsplit(")", 1)[1].split()[0] != "Z"
    except FileNotFoundError:
        return False


class _ShellTest(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.top = os.path.realpath(tmp.name)
        self.root = os.path.join(self.top, "case")
        os.makedirs(self.root)
        self.binary = os.path.join(self.root, "sample.exe")
        with open(self.binary, "wb") as f:
            f.write(b"#!/bin/sh\ntouch ran\n")
        os.chmod(self.binary, 0o755)
        self.session = SessionState()
        self.session.files.workspace = Workspace.for_database(
            self.binary + ".i64",
            os.path.join(self.top, "config"),
            "abc",
            input_path=self.binary,
            home=os.path.join(self.top, "home"),
        )
        self.registry = ToolRegistry()
        self.registry.register_module(shell)

    def _run(self, command, cancel=None, **arguments):
        with activate_session(self.session, cancel):
            return self.registry.execute("run_shell", {"command": command, **arguments})


class TestResult(_ShellTest):
    def test_exit_code_zero(self):
        self.assertIn("Exit code 0", self._run("true"))

    def test_exit_code_nonzero(self):
        self.assertIn("Exit code 3", self._run("exit 3"))

    def test_streams_are_labelled_separately(self):
        result = self._run("echo to-out; echo to-err >&2")
        lines = result.split("\n")
        self.assertLess(lines.index("to-out"), lines.index("=== stderr ==="))
        self.assertGreater(lines.index("to-err"), lines.index("=== stderr ==="))

    def test_an_empty_stream_is_not_shown(self):
        self.assertNotIn("stderr", self._run("echo hi"))

    def test_no_output_says_so(self):
        self.assertIn("(no output)", self._run("true"))

    def test_pipes_work(self):
        self.assertIn("HI", self._run("echo hi | tr a-z A-Z"))

    def test_colour_codes_are_stripped(self):
        result = self._run(r"printf '\033[31mred\033[0m\n'")
        self.assertIn("red", result)
        self.assertNotIn("\x1b", result)

    def test_a_bare_cat_returns_instead_of_waiting_for_input(self):
        started = time.monotonic()
        self.assertIn("Exit code 0", self._run("cat", timeout=20))
        self.assertLess(time.monotonic() - started, 10)

    def test_empty_command(self):
        self.assertIn("empty", self._run("   "))

    def test_no_workspace_explains_itself(self):
        self.assertIn("no workspace", self.registry.execute("run_shell", {"command": "true"}))


class TestWorkingDirectory(_ShellTest):
    def test_defaults_to_the_binary_directory(self):
        self.assertIn(self.root, self._run("pwd"))

    def test_inside_the_workspace(self):
        os.makedirs(os.path.join(self.root, "sub"))
        self.assertIn(os.path.join(self.root, "sub"), self._run("pwd", working_dir="sub"))

    def test_outside_is_refused_and_nothing_runs(self):
        marker = os.path.join(self.root, "ran")
        result = self._run(f"touch {marker}", working_dir="/tmp")
        self.assertIn("outside the workspace", result)
        self.assertFalse(os.path.exists(marker))

    def test_a_file_is_refused(self):
        self.assertIn("not a directory", self._run("pwd", working_dir="sample.exe"))

    def test_scratch_is_created_when_it_is_the_only_root(self):
        self.session.files.workspace = Workspace.for_database("", os.path.join(self.top, "config"), "abc")
        self.assertIn(self.session.files.workspace.scratch_root, self._run("pwd"))


class TestEnvironment(_ShellTest):
    def test_credentials_are_not_passed_on(self):
        env = {"ANTHROPIC_API_KEY": "k1", "GITHUB_TOKEN": "k2", "AWS_SECRET_ACCESS_KEY": "k3", "DB_PASSWORD": "k4"}
        with patch.dict(os.environ, env):
            result = self._run("env")
        for value in ("k1", "k2", "k3", "k4"):
            self.assertNotIn(f"={value}", result)

    def test_ordinary_variables_are(self):
        with patch.dict(os.environ, {"MY_SETTING": "kept"}):
            self.assertIn("MY_SETTING=kept", self._run("env"))

    def test_the_hosts_python_home_is_not(self):
        with patch.dict(os.environ, {"PYTHONHOME": "/host/python"}):
            self.assertNotIn("PYTHONHOME", self._run("env"))


class TestStopping(_ShellTest):
    def test_a_timeout_says_so(self):
        started = time.monotonic()
        result = self._run("sleep 30", timeout=1)
        self.assertIn("timed out after 1s", result)
        self.assertLess(time.monotonic() - started, 6)

    def test_a_timeout_takes_children_with_it(self):
        pid_file = os.path.join(self.root, "pid")
        self._run(f"sh -c 'sleep 30 & echo $! > {pid_file}; wait'", timeout=1)
        with open(pid_file, encoding="utf-8") as f:
            child = int(f.read())
        for _ in range(40):
            if not _alive(child):
                break
            time.sleep(0.05)
        self.assertFalse(_alive(child), "the child outlived the timeout")

    def test_output_from_before_the_timeout_is_kept(self):
        self.assertIn("before", self._run("echo before; sleep 30", timeout=1))

    def test_stop_ends_it_promptly(self):
        cancel = threading.Event()
        threading.Timer(0.3, cancel.set).start()
        started = time.monotonic()
        result = self._run("sleep 30", cancel=cancel, timeout=60)
        self.assertIn("cancelled by the user", result)
        self.assertLess(time.monotonic() - started, 6)

    def test_the_timeout_is_clamped(self):
        with patch.object(shell, "_wait_for_exit", return_value="") as wait:
            self._run("true", timeout=99_999)
        self.assertEqual(wait.call_args.args[1], shell.MAX_TIMEOUT_SECONDS)

    def test_its_own_timeout_always_fires_before_the_registrys(self):
        # The registry's cannot stop a thread, so it would orphan the process.
        self.assertGreater(shell.run_shell._tool_definition.timeout, shell.MAX_TIMEOUT_SECONDS)

    def test_a_background_job_does_not_hold_the_result_up(self):
        started = time.monotonic()
        result = self._run("sleep 5 & echo started")
        self.assertLess(time.monotonic() - started, 4)
        self.assertIn("started", result)
        self.assertIn("background process", result)


class TestLargeOutput(_ShellTest):
    def test_stays_in_budget_with_both_ends_and_a_handle(self):
        result = self._run("seq 1 200000")
        self.assertLessEqual(len(result), INLINE_RESULT_BUDGET)
        self.assertIn("1\t1\n", result)
        self.assertIn("200000\t200000", result)
        self.assertIn("artifact://1", result)

    def test_both_streams_large_still_fit(self):
        result = self._run("seq 1 100000; seq 1 100000 >&2")
        self.assertLessEqual(len(result), INLINE_RESULT_BUDGET)
        self.assertIn("artifact://1", result)
        self.assertIn("artifact://2", result)

    def test_a_small_stderr_is_not_lost_beside_a_huge_stdout(self):
        result = self._run("seq 1 200000; echo the-final-$((1 + 1)) >&2")
        self.assertIn("the-final-2", result)
        self.assertLessEqual(len(result), INLINE_RESULT_BUDGET)

    def test_a_flood_does_not_block_and_keeps_its_tail(self):
        with patch.object(shell, "_CAPTURE_HEAD_BYTES", 1000), patch.object(shell, "_CAPTURE_TAIL_BYTES", 1000):
            result = self._run("seq 1 100000; echo the-last-$((1 + 1))", timeout=30)
        self.assertIn("Exit code 0", result)
        self.assertIn("the-last-2", result)

    def test_output_that_was_dropped_says_so(self):
        with patch.object(shell, "_CAPTURE_HEAD_BYTES", 1000), patch.object(shell, "_CAPTURE_TAIL_BYTES", 1000):
            result = self._run("seq 1 100000")
        self.assertIn("were not kept", result)

    def test_nothing_is_claimed_dropped_when_it_was_not(self):
        self.assertNotIn("were not kept", self._run("seq 1 100"))

    def test_the_registry_does_not_cut_it(self):
        self.assertNotIn("truncated by global safety cap", self._run("seq 1 200000"))


class TestSmallOutputIsNeverElided(_ShellTest):
    def test_two_short_streams_are_both_whole(self):
        # A two-line error once came back as "1 middle lines elided", because the
        # budget left for it covered little more than its own text.
        result = self._run("echo fine; echo line-one >&2; echo line-two >&2")
        self.assertNotIn("elided", result)
        self.assertIn("line-one\nline-two", result)

    def test_a_short_stderr_beside_a_flood_is_whole(self):
        result = self._run("seq 1 200000; echo line-one >&2; echo line-two >&2")
        self.assertIn("=== stderr ===\nline-one\nline-two", result)

    def test_a_short_stdout_beside_a_flood_is_whole(self):
        result = self._run("seq 1 200000 >&2; echo line-one; echo line-two")
        self.assertIn("=== stdout ===\nline-one\nline-two", result)

    def test_medium_streams_are_both_whole(self):
        result = self._run("seq 1 300; seq 1 300 >&2")
        self.assertNotIn("elided", result)

    def test_a_long_command_does_not_eat_the_budget(self):
        filler = "x" * 3000
        self.assertNotIn("elided", self._run(f": {filler}; echo fine; echo oops >&2"))


class TestScratch(_ShellTest):
    def _registry_with_files(self):
        from rikugan.tools import filesystem

        self.registry.register_module(filesystem)

    def test_a_written_file_is_on_the_same_disk_for_a_command(self):
        # The agent reported write_file and run_shell as separate "namespaces".
        self._registry_with_files()
        with activate_session(self.session):
            self.registry.execute("write_file", {"file_path": "scratch/hello.py", "content": "print('hi from disk')\n"})
        self.assertIn("hi from disk", self._run('python3 "$SCRATCH/hello.py"'))

    def test_a_command_writes_what_the_file_tools_read(self):
        self._registry_with_files()
        self._run('echo from-shell > "$SCRATCH/out.txt"')
        with activate_session(self.session):
            self.assertIn("from-shell", self.registry.execute("read_file", {"file_path": "scratch/out.txt"}))

    def test_write_file_says_where_a_scratch_file_really_is(self):
        self._registry_with_files()
        with activate_session(self.session):
            result = self.registry.execute("write_file", {"file_path": "scratch/a.py", "content": "x"})
        self.assertIn(os.path.join(self.session.files.workspace.scratch_root, "a.py"), result)

    def test_scratch_exists_for_a_first_command(self):
        self.assertIn("Exit code 0", self._run('test -d "$SCRATCH"'))

    def test_the_working_dir_shorthand_works(self):
        self.assertIn(self.session.files.workspace.scratch_root, self._run("pwd", working_dir="scratch"))

    def test_the_shorthand_in_a_command_is_refused_with_the_real_path(self):
        result = self._run("python3 scratch/hello.py")
        self.assertIn("refused", result)
        self.assertIn("$SCRATCH", result)
        self.assertIn(self.session.files.workspace.scratch_root, result)

    def test_nothing_stray_is_left_beside_the_sample(self):
        for command in (
            "mkdir -p scratch",
            "mkdir scratch && echo x > scratch/a.py",
            "true; cd scratch",
            "echo x > scratch/a.py",
            "echo x >scratch/a.py",
            "touch scratch",
            "OUT=scratch/a.py true",
            "tool --out=scratch/a.py",
            'cat > scratch/a.py <<EOF\nprint(1)\nEOF',
        ):
            with self.subTest(command):
                self.assertIn("nothing ran", self._run(command))
        self.assertFalse(os.path.exists(os.path.join(self.root, "scratch")))

    def test_a_real_folder_with_that_name_is_left_to_work(self):
        os.makedirs(os.path.join(self.root, "scratch"))
        self.assertIn("Exit code 0", self._run("ls scratch/"))

    def test_the_word_alone_is_not_a_path(self):
        for command in ("grep -c scratch sample.exe", "echo scratch"):
            with self.subTest(command):
                self.assertNotIn("nothing ran", self._run(command))

    def test_a_heredoc_body_may_mention_it(self):
        target = os.path.join(self.session.files.workspace.scratch_root, "a.py")
        result = self._run(f'mkdir -p "$SCRATCH"; cat > {target} <<EOF\nprint("scratch/x")\nEOF')
        self.assertNotIn("nothing ran", result)
        with open(target, encoding="utf-8") as f:
            self.assertIn("scratch/x", f.read())


class TestStreamBudgets(unittest.TestCase):
    def test_every_split_fits(self):
        sizes = (0, 10, 5_000, 9_000, 10_000, 15_000, 19_000, 100_000)
        for out in sizes:
            for err in sizes:
                with self.subTest(out=out, err=err):
                    a, b = shell._stream_budgets("o" * out, "e" * err)
                    self.assertLessEqual(a + b, INLINE_RESULT_BUDGET - 200)

    def test_a_stream_that_fits_gets_what_it_needs(self):
        _, err_budget = shell._stream_budgets("o" * 100_000, "e" * 50)
        self.assertGreaterEqual(err_budget, 50 + len("=== stderr ===\n"))


class TestAnalysedBinary(_ShellTest):
    def test_running_it_is_refused(self):
        commands = {
            "relative": "./sample.exe",
            "bare": "sample.exe",
            "absolute": self.binary,
            "after a separator": "cd . && ./sample.exe",
            "after a semicolon": "true; ./sample.exe",
            "on a later line": "true\n./sample.exe",
            "piped": "echo x | ./sample.exe",
            "with env": "FOO=1 ./sample.exe",
            "through sudo": "sudo ./sample.exe",
            "through wine": "wine sample.exe",
            "through timeout": "timeout 5 ./sample.exe",
            "through nested runners": "nohup nice -n 5 ./sample.exe",
            "through sh": "sh ./sample.exe",
            "subshell": "(./sample.exe)",
            "quoted": '"./sample.exe"',
        }
        for label, command in commands.items():
            with self.subTest(label):
                result = self._run(command)
                self.assertIn("binary being analysed", result)
        self.assertFalse(os.path.exists(os.path.join(self.root, "ran")))

    def test_inspecting_it_is_fine(self):
        for command in (
            "file sample.exe",
            "wc -c sample.exe",
            "sha256sum sample.exe | cut -d' ' -f1",
            "cat ./sample.exe > /dev/null",
            "grep -c touch sample.exe",
        ):
            with self.subTest(command):
                self.assertNotIn("binary being analysed", self._run(command))

    def test_the_refusal_says_inspection_is_fine(self):
        self.assertIn("strings", self._run("./sample.exe"))

    def test_unbalanced_quotes_go_to_the_shell_to_report(self):
        self.assertIn("Exit code", self._run("echo 'oops"))


if __name__ == "__main__":
    unittest.main()

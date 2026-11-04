"""run_shell: run a command in the workspace.

This is real shell access, by design: the capability requested is shell, and
splitting commands up would break pipes while stopping nothing, since the model
could always write ``sh -c``. The user's consent is the boundary.

It is not a sandbox, and script_guard is not being bypassed by accident. The two
protect different things: script_guard stops code running inside the host process
from spawning processes, with the database and host memory in reach. This runs
out of process, cannot touch the database, and has a hard timeout.

There is exactly one refusal, of running the analysed binary, which catches the
plain mistake and is not a guarantee. A general command blocklist would be
theatre: a substring filter on shell text cannot keep a promise like that.
"""

from __future__ import annotations

import os
import re
import shlex
import signal
import subprocess
import threading
import time
from collections import deque
from typing import TYPE_CHECKING, Annotated

from ..constants import FILESYSTEM_CATEGORY
from ..core.artifacts import INLINE_RESULT_BUDGET, format_large_result
from ..core.chat_context import current_cancel_event, current_session
from ..core.logging import log_debug
from ..core.workspace import PathRefused
from .base import tool
from .filesystem import refusals_as_text

if TYPE_CHECKING:
    from ..core.workspace import Workspace

SCRATCH_ENV_VAR = "SCRATCH"
DEFAULT_TIMEOUT_SECONDS = 30
MAX_TIMEOUT_SECONDS = 240
# Past the longest command, so the command's own timeout always fires first. The
# registry's cannot stop a running thread: it would orphan the process instead.
_TOOL_TIMEOUT_SECONDS = MAX_TIMEOUT_SECONDS + 15

_CAPTURE_HEAD_BYTES = 4 * 1024 * 1024
_CAPTURE_TAIL_BYTES = 4 * 1024 * 1024
_READ_CHUNK = 64 * 1024
# The status line above the streams, and what each stream's title costs.
_RESULT_OVERHEAD = 200
_STREAM_TITLE_CHARS = 20
_POLL_SECONDS = 0.05
_TERMINATE_GRACE_SECONDS = 2.0
# A background job can hold the pipe open after the shell itself has exited.
_DRAIN_SECONDS = 1.0

# Credentials the agent has no business handing to a command it was steered into.
_SECRET_ENV_NAME = re.compile(r"API_?KEY|TOKEN|SECRET|PASSWORD|PASSWD|CREDENTIAL", re.IGNORECASE)
# Set for the host's embedded Python; left in place it breaks any python3 run here.
_HOST_ONLY_ENV = frozenset({"PYTHONHOME"})

_ANSI_ESCAPE = re.compile(r"\x1b\[[0-9;?]*[ -/]*[@-~]")

_COMMAND_SEPARATORS = frozenset({";", ";;", "&&", "||", "|", "|&", "&", "(", ")"})
_ENV_ASSIGNMENT = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*=")
# Commands that run their argument. Their flags and numbers are skipped.
_DIRECTORY_COMMANDS = frozenset({"mkdir", "cd", "pushd", "ls", "rmdir", "rm", "touch", "cp", "mv", "find", "du"})
_RUNNERS = frozenset(
    {
        "sudo",
        "doas",
        "nohup",
        "env",
        "exec",
        "command",
        "time",
        "timeout",
        "nice",
        "wine",
        "wine64",
        "mono",
        "sh",
        "bash",
    }
)


def _tokens(command: str) -> list[str]:
    lexer = shlex.shlex(command.replace("\n", ";"), posix=True, punctuation_chars=True)
    lexer.whitespace_split = True
    try:
        return list(lexer)
    except ValueError:  # unbalanced quotes: the shell will report it better
        return []


def _command_words(command: str) -> list[str]:
    """The words that would be run as programs, as far as the text can tell."""
    tokens = _tokens(command)
    words: list[str] = []
    expecting_command = True
    after_runner = False
    for token in tokens:
        if token in _COMMAND_SEPARATORS:
            expecting_command, after_runner = True, False
        elif expecting_command and _ENV_ASSIGNMENT.match(token):
            continue
        elif after_runner and (token.startswith("-") or token.isdigit() or _ENV_ASSIGNMENT.match(token)):
            continue
        elif expecting_command or after_runner:
            words.append(token)
            after_runner = os.path.basename(token) in _RUNNERS
            expecting_command = False
    return words


def _refuse_running_the_binary(command: str, cwd: str, workspace: Workspace) -> None:
    for word in _command_words(command):
        if os.path.realpath(os.path.join(cwd, word)).casefold() in workspace.protected:
            raise PathRefused(
                f"Error: refused — this would run '{word}', the binary being analysed. "
                "Analysis here is static only. Inspecting it, with file, strings or a hex dump, is fine."
            )


def _means_the_scratch_shorthand(command: str) -> bool:
    """Whether the command uses scratch/ as a relative path.

    Only the first line of a heredoc counts, since the rest is file content. A
    bare "scratch" only counts for commands that take a directory: in
    "grep scratch notes.txt" it is just a word.
    """
    if "<<" in command:
        command = command.split("\n", 1)[0]
    program = ""
    for token in _tokens(command):
        if token in _COMMAND_SEPARATORS:
            program = ""
            continue
        if not program and not _ENV_ASSIGNMENT.match(token):
            program = os.path.basename(token)
        if token.rsplit("=", 1)[-1].startswith("scratch/"):
            return True
        if token == "scratch" and program in _DIRECTORY_COMMANDS:
            return True
    return False


def _refuse_scratch_shorthand(command: str, cwd: str, workspace: Workspace) -> None:
    """Stop scratch/ quietly meaning a different folder than it does for file tools.

    The file tools resolve it to the agent's scratch directory, but a shell sees
    an ordinary relative path. So a file just written looks missing, and the
    obvious workaround, mkdir scratch, leaves a stray folder beside the sample.
    """
    if os.path.lexists(os.path.join(cwd, "scratch")) or not _means_the_scratch_shorthand(command):
        return
    raise PathRefused(
        f"Error: refused, and nothing ran — in a shell, scratch/ would be a folder inside {cwd}, not your "
        f"scratch directory. It is the same disk, at a different path: use $SCRATCH (which is "
        f"{workspace.scratch_root}), or pass working_dir='scratch'."
    )


def _shell_environment(workspace: Workspace) -> dict[str, str]:
    env = {
        name: value
        for name, value in os.environ.items()
        if name not in _HOST_ONLY_ENV and not _SECRET_ENV_NAME.search(name)
    }
    env[SCRATCH_ENV_VAR] = workspace.scratch_root
    return env


class _BoundedPipe(threading.Thread):
    """Drain a pipe, keeping both ends of whatever came through it.

    Draining has to continue past the limit, or a chatty command blocks forever
    on a full pipe. The tail is kept because that is where the final error is.
    """

    def __init__(self, pipe) -> None:
        super().__init__(daemon=True)
        self._pipe = pipe
        self._head = bytearray()
        self._tail: deque[bytes] = deque()
        self._tail_size = 0
        self.dropped_bytes = 0

    def run(self) -> None:
        try:
            # os.read, because a file object's read() waits for a full chunk or EOF:
            # output a background job keeps the pipe open behind would never arrive.
            while chunk := os.read(self._pipe.fileno(), _READ_CHUNK):
                self._keep(chunk)
        except (OSError, ValueError) as e:
            log_debug(f"run_shell: reading output stopped: {e}")

    def _keep(self, chunk: bytes) -> None:
        room = _CAPTURE_HEAD_BYTES - len(self._head)
        if room > 0:
            self._head += chunk[:room]
            chunk = chunk[room:]
        if not chunk:
            return
        self._tail.append(chunk)
        self._tail_size += len(chunk)
        while self._tail_size - len(self._tail[0]) >= _CAPTURE_TAIL_BYTES:
            dropped = self._tail.popleft()
            self._tail_size -= len(dropped)
            self.dropped_bytes += len(dropped)

    def text(self) -> str:
        head = bytes(self._head).decode("utf-8", "replace")
        tail = b"".join(list(self._tail)).decode("utf-8", "replace")
        gap = f"\n⟦{self.dropped_bytes:,} bytes of output here were not kept⟧\n" if self.dropped_bytes else ""
        return _ANSI_ESCAPE.sub("", f"{head}{gap}{tail}")


def _kill_process_tree(process: subprocess.Popen) -> None:
    """Stop the command and everything it started, so a timed-out make leaves nothing."""
    if os.name == "nt":
        subprocess.run(
            ["taskkill", "/T", "/F", "/PID", str(process.pid)],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=False,
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
        )
        return
    try:
        os.killpg(process.pid, signal.SIGTERM)
    except ProcessLookupError:
        return
    deadline = time.monotonic() + _TERMINATE_GRACE_SECONDS
    while process.poll() is None and time.monotonic() < deadline:
        time.sleep(_POLL_SECONDS)
    try:
        os.killpg(process.pid, signal.SIGKILL)
    except ProcessLookupError:
        return


def _wait_for_exit(process: subprocess.Popen, timeout: int) -> str:
    """Wait, and return why the command had to be stopped, if it did."""
    cancel = current_cancel_event()
    deadline = time.monotonic() + timeout
    while process.poll() is None:
        if cancel is not None and cancel.is_set():
            _kill_process_tree(process)
            return "cancelled by the user"
        if time.monotonic() > deadline:
            _kill_process_tree(process)
            return f"timed out after {timeout}s and was killed"
        time.sleep(_POLL_SECONDS)
    return ""


def _stream_budgets(stdout: str, stderr: str) -> tuple[int, int]:
    """Share the result so a stream that fits is shown whole, whatever the other does.

    A short error beside a flood of output is the part worth reading.
    """
    total = INLINE_RESULT_BUDGET - _RESULT_OVERHEAD
    half = total // 2
    needs_out = len(stdout) + _STREAM_TITLE_CHARS
    needs_err = len(stderr) + _STREAM_TITLE_CHARS
    if needs_out + needs_err <= total:
        return needs_out, needs_err
    if needs_err <= half:
        return total - needs_err, needs_err
    if needs_out <= half:
        return needs_out, total - needs_out
    return half, half


def _working_directory(raw: str, workspace: Workspace) -> str:
    # Before anything else: $SCRATCH and working_dir='scratch' both promise it exists.
    os.makedirs(workspace.scratch_root, exist_ok=True)
    if raw:
        cwd = workspace.resolve(raw)
        if not os.path.isdir(cwd):
            raise PathRefused(f"Error: working_dir '{raw}' is not a directory.")
        return cwd
    return workspace.default_root


@tool(
    category=FILESYSTEM_CATEGORY,
    timeout=_TOOL_TIMEOUT_SECONDS,
    long_running=True,
    result_limit=INLINE_RESULT_BUDGET,
    description=(
        "Run a shell command in the workspace, for building, testing or inspecting files. stdout and stderr "
        "come back separately with the exit code; large output shows both ends with an artifact:// handle. "
        "Commands get no input. The scratch directory is $SCRATCH here: scratch/ only works in the file tools. "
        "Prefer search_files to grep and read_file to cat. Changes made this way are "
        "not undoable. Never runs the binary being analysed."
    ),
)
@refusals_as_text
def run_shell(
    command: Annotated[str, "The command to run. Pipes and redirects work"],
    timeout: Annotated[int, f"Seconds before it is killed, up to {MAX_TIMEOUT_SECONDS}"] = DEFAULT_TIMEOUT_SECONDS,
    working_dir: Annotated[str, "Directory to run in, inside the workspace. Defaults to the binary's"] = "",
) -> str:
    """Run a shell command."""
    session = current_session()
    if session is None or session.files.workspace is None:
        raise PathRefused("Error: no workspace is open, so there is nowhere to run a command.")
    files = session.files
    if not command.strip():
        return "Error: command is empty."

    workspace = files.workspace
    assert workspace is not None
    cwd = _working_directory(working_dir, workspace)
    _refuse_running_the_binary(command, cwd, workspace)
    _refuse_scratch_shorthand(command, cwd, workspace)
    seconds = max(1, min(timeout, MAX_TIMEOUT_SECONDS))

    started = time.monotonic()
    process = subprocess.Popen(
        command,
        shell=True,
        cwd=cwd,
        env=_shell_environment(workspace),
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        # Its own process group, so a timeout can take the whole tree down.
        start_new_session=os.name != "nt",
        creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
    )
    out, err = _BoundedPipe(process.stdout), _BoundedPipe(process.stderr)
    out.start()
    err.start()
    stopped = _wait_for_exit(process, seconds)
    out.join(_DRAIN_SECONDS)
    err.join(_DRAIN_SECONDS)
    elapsed = time.monotonic() - started

    status = f"Exit code {process.returncode}" if not stopped else f"The command {stopped}"
    lines = [f"{status} · {elapsed:.2f}s"]
    if not stopped and (out.is_alive() or err.is_alive()):
        lines.append("A background process is still running and holding its output; this is what it had written.")

    stdout, stderr = out.text(), err.text()
    stdout_budget, stderr_budget = _stream_budgets(stdout, stderr)
    if stdout:
        lines.append(format_large_result("stdout", stdout, files.artifacts, budget=stdout_budget))
    if stderr:
        lines.append(format_large_result("stderr", stderr, files.artifacts, budget=stderr_budget))
    if not stdout and not stderr:
        lines.append("(no output)")
    return "\n".join(lines)

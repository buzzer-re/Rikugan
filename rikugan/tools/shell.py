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

DEFAULT_TIMEOUT_SECONDS = 30
MAX_TIMEOUT_SECONDS = 240
# Past the longest command, so the command's own timeout always fires first. The
# registry's cannot stop a running thread: it would orphan the process instead.
_TOOL_TIMEOUT_SECONDS = MAX_TIMEOUT_SECONDS + 15

_CAPTURE_HEAD_BYTES = 4 * 1024 * 1024
_CAPTURE_TAIL_BYTES = 4 * 1024 * 1024
_READ_CHUNK = 64 * 1024
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


def _command_words(command: str) -> list[str]:
    """The words that would be run as programs, as far as the text can tell."""
    lexer = shlex.shlex(command.replace("\n", ";"), posix=True, punctuation_chars=True)
    lexer.whitespace_split = True
    try:
        tokens = list(lexer)
    except ValueError:  # unbalanced quotes: the shell will report it better
        return []

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


def _scrubbed_environment() -> dict[str, str]:
    return {
        name: value
        for name, value in os.environ.items()
        if name not in _HOST_ONLY_ENV and not _SECRET_ENV_NAME.search(name)
    }


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
    """Share the result between the streams: each gets half, or what the other left."""
    half = (INLINE_RESULT_BUDGET - 200) // 2
    if not stderr:
        return half * 2, 0
    if not stdout:
        return 0, half * 2
    stdout_budget = max(half, half * 2 - len(stderr) - 50)
    stderr_budget = max(half, half * 2 - len(stdout) - 50)
    if stdout_budget > half:
        return stdout_budget, half * 2 - stdout_budget
    if stderr_budget > half:
        return half * 2 - stderr_budget, stderr_budget
    return half, half


def _working_directory(raw: str, workspace: Workspace) -> str:
    if raw:
        cwd = workspace.resolve(raw)
        if not os.path.isdir(cwd):
            raise PathRefused(f"Error: working_dir '{raw}' is not a directory.")
        return cwd
    # With no usable binary directory this is scratch, which may not exist yet.
    os.makedirs(workspace.default_root, exist_ok=True)
    return workspace.default_root


@tool(
    category=FILESYSTEM_CATEGORY,
    timeout=_TOOL_TIMEOUT_SECONDS,
    long_running=True,
    result_limit=INLINE_RESULT_BUDGET,
    description=(
        "Run a shell command in the workspace, for building, testing or inspecting files. stdout and stderr "
        "come back separately with the exit code; large output shows both ends with an artifact:// handle. "
        "Commands get no input. Prefer search_files to grep and read_file to cat. Changes made this way are "
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
    seconds = max(1, min(timeout, MAX_TIMEOUT_SECONDS))

    started = time.monotonic()
    process = subprocess.Popen(
        command,
        shell=True,
        cwd=cwd,
        env=_scrubbed_environment(),
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
        lines.append(format_large_result(f"stdout: {command}", stdout, files.artifacts, budget=stdout_budget))
    if stderr:
        lines.append(format_large_result(f"stderr: {command}", stderr, files.artifacts, budget=stderr_budget))
    if not stdout and not stderr:
        lines.append("(no output)")
    return "\n".join(lines)

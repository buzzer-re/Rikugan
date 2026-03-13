"""Bridge to external CLI agents via subprocess.

Security: always uses subprocess.Popen with an args list, never shell=True.
Streams stdout line-by-line and supports cancellation via threading.Event.
"""

from __future__ import annotations

import queue
import subprocess
import threading
import time
import uuid
from typing import Any

from ...core.logging import log_error, log_info
from .types import (
    A2AEvent,
    A2AEventType,
    A2ATask,
    A2ATaskStatus,
    ExternalAgentConfig,
)

# ---------------------------------------------------------------------------
# Command builders for known CLI agents
# ---------------------------------------------------------------------------


def _build_claude_command(prompt: str, config: ExternalAgentConfig) -> list[str]:
    """Build command for Claude Code CLI."""
    cmd = ["claude", "--print", "--output-format", "text"]
    if config.model:
        cmd.extend(["--model", config.model])
    cmd.append(prompt)
    return cmd


def _build_codex_command(prompt: str, config: ExternalAgentConfig) -> list[str]:
    """Build command for Codex CLI."""
    cmd = ["codex", "--quiet"]
    if config.model:
        cmd.extend(["--model", config.model])
    cmd.append(prompt)
    return cmd


_COMMAND_BUILDERS: dict[str, Any] = {
    "claude": _build_claude_command,
    "codex": _build_codex_command,
}


# ---------------------------------------------------------------------------
# Bridge
# ---------------------------------------------------------------------------


class SubprocessBridge:
    """Execute tasks by spawning external CLI agents as subprocesses.

    Each task runs in its own background thread.  Results are delivered
    via an event queue that the caller can poll.
    """

    def __init__(self) -> None:
        self._tasks: dict[str, A2ATask] = {}
        self._event_queue: queue.Queue[A2AEvent] = queue.Queue()
        self._cancel_events: dict[str, threading.Event] = {}
        self._threads: dict[str, threading.Thread] = {}

    def dispatch(
        self,
        config: ExternalAgentConfig,
        prompt: str,
        timeout: float = 300.0,
    ) -> str:
        """Dispatch a task to an external agent. Returns the task ID.

        Parameters
        ----------
        config : ExternalAgentConfig
            Configuration for the target agent.
        prompt : str
            The prompt/task to send to the agent.
        timeout : float
            Maximum time in seconds to wait for the subprocess.
        """
        task_id = uuid.uuid4().hex[:12]
        cancel = threading.Event()
        self._cancel_events[task_id] = cancel

        task = A2ATask(
            id=task_id,
            agent_name=config.name,
            prompt=prompt,
            status=A2ATaskStatus.PENDING,
            created_at=time.time(),
        )
        self._tasks[task_id] = task

        thread = threading.Thread(
            target=self._run_subprocess,
            args=(task_id, config, prompt, timeout, cancel),
            daemon=True,
            name=f"a2a-subprocess-{task_id[:6]}",
        )
        self._threads[task_id] = thread
        thread.start()

        log_info(f"A2A subprocess dispatched: task={task_id}, agent={config.name}")
        return task_id

    def cancel(self, task_id: str) -> None:
        """Cancel a running subprocess task."""
        cancel = self._cancel_events.get(task_id)
        if cancel:
            cancel.set()
        task = self._tasks.get(task_id)
        if task and task.status in (A2ATaskStatus.PENDING, A2ATaskStatus.RUNNING):
            task.status = A2ATaskStatus.CANCELLED
            task.completed_at = time.time()
            self._event_queue.put(
                A2AEvent(
                    type=A2AEventType.TASK_CANCELLED,
                    task_id=task_id,
                    agent_name=task.agent_name,
                )
            )

    def poll_event(self) -> A2AEvent | None:
        """Non-blocking poll for the next A2A event."""
        try:
            return self._event_queue.get_nowait()
        except queue.Empty:
            return None

    def get_task(self, task_id: str) -> A2ATask | None:
        """Look up a task by ID."""
        return self._tasks.get(task_id)

    def list_tasks(self) -> list[A2ATask]:
        """Return all tasks."""
        return list(self._tasks.values())

    def _run_subprocess(
        self,
        task_id: str,
        config: ExternalAgentConfig,
        prompt: str,
        timeout: float,
        cancel: threading.Event,
    ) -> None:
        """Background thread: spawn subprocess and stream output."""
        task = self._tasks[task_id]
        task.status = A2ATaskStatus.RUNNING

        # Build command using the appropriate builder
        builder = _COMMAND_BUILDERS.get(config.name)
        if builder is None:
            # Generic fallback: just run the executable with the prompt as an arg
            cmd = [config.name, prompt]
        else:
            cmd = builder(prompt, config)

        self._event_queue.put(
            A2AEvent(
                type=A2AEventType.TASK_STARTED,
                task_id=task_id,
                agent_name=config.name,
            )
        )

        # Build environment
        env = None
        if config.env:
            import os

            env = dict(os.environ)
            env.update(config.env)

        try:
            # SECURITY: args list, NOT shell=True
            proc = subprocess.Popen(
                cmd,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                env=env,
                text=True,
            )
        except FileNotFoundError:
            task.status = A2ATaskStatus.FAILED
            task.error = f"Executable not found: {cmd[0]}"
            task.completed_at = time.time()
            self._event_queue.put(
                A2AEvent(
                    type=A2AEventType.TASK_FAILED,
                    task_id=task_id,
                    agent_name=config.name,
                    error=task.error,
                )
            )
            log_error(f"A2A subprocess not found: {cmd[0]}")
            return
        except OSError as e:
            task.status = A2ATaskStatus.FAILED
            task.error = f"Failed to start subprocess: {e}"
            task.completed_at = time.time()
            self._event_queue.put(
                A2AEvent(
                    type=A2AEventType.TASK_FAILED,
                    task_id=task_id,
                    agent_name=config.name,
                    error=task.error,
                )
            )
            log_error(f"A2A subprocess OSError: {e}")
            return

        output_lines: list[str] = []
        start_time = time.time()

        try:
            # Stream stdout line-by-line
            assert proc.stdout is not None
            for line in proc.stdout:
                if cancel.is_set():
                    proc.terminate()
                    try:
                        proc.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        proc.kill()
                    task.status = A2ATaskStatus.CANCELLED
                    task.completed_at = time.time()
                    self._event_queue.put(
                        A2AEvent(
                            type=A2AEventType.TASK_CANCELLED,
                            task_id=task_id,
                            agent_name=config.name,
                        )
                    )
                    return

                elapsed = time.time() - start_time
                if elapsed > timeout:
                    proc.terminate()
                    try:
                        proc.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        proc.kill()
                    task.status = A2ATaskStatus.TIMEOUT
                    task.error = f"Subprocess timed out after {timeout:.0f}s"
                    task.completed_at = time.time()
                    self._event_queue.put(
                        A2AEvent(
                            type=A2AEventType.TASK_FAILED,
                            task_id=task_id,
                            agent_name=config.name,
                            error=task.error,
                        )
                    )
                    log_error(f"A2A subprocess timeout: {task_id}")
                    return

                stripped = line.rstrip("\n")
                output_lines.append(stripped)
                self._event_queue.put(
                    A2AEvent(
                        type=A2AEventType.TASK_OUTPUT,
                        task_id=task_id,
                        agent_name=config.name,
                        text=stripped,
                    )
                )

            # Wait for process to finish
            proc.wait(timeout=10)

            if proc.returncode == 0:
                task.result = "\n".join(output_lines)
                task.status = A2ATaskStatus.COMPLETED
                task.completed_at = time.time()
                self._event_queue.put(
                    A2AEvent(
                        type=A2AEventType.TASK_COMPLETED,
                        task_id=task_id,
                        agent_name=config.name,
                        text=task.result,
                    )
                )
                elapsed = task.completed_at - task.created_at
                log_info(
                    f"A2A subprocess completed: task={task_id}, elapsed={elapsed:.1f}s, output_len={len(task.result)}"
                )
            else:
                stderr_text = ""
                if proc.stderr:
                    stderr_text = proc.stderr.read()
                task.status = A2ATaskStatus.FAILED
                task.error = f"Exit code {proc.returncode}: {stderr_text}"
                task.completed_at = time.time()
                self._event_queue.put(
                    A2AEvent(
                        type=A2AEventType.TASK_FAILED,
                        task_id=task_id,
                        agent_name=config.name,
                        error=task.error,
                    )
                )
                log_error(f"A2A subprocess failed: task={task_id}, rc={proc.returncode}")

        except subprocess.TimeoutExpired:
            proc.kill()
            task.status = A2ATaskStatus.TIMEOUT
            task.error = "Process did not exit after termination"
            task.completed_at = time.time()
            self._event_queue.put(
                A2AEvent(
                    type=A2AEventType.TASK_FAILED,
                    task_id=task_id,
                    agent_name=config.name,
                    error=task.error,
                )
            )
        except Exception as e:
            task.status = A2ATaskStatus.FAILED
            task.error = str(e)
            task.completed_at = time.time()
            self._event_queue.put(
                A2AEvent(
                    type=A2AEventType.TASK_FAILED,
                    task_id=task_id,
                    agent_name=config.name,
                    error=task.error,
                )
            )
            log_error(f"A2A subprocess error: task={task_id}, error={e}")

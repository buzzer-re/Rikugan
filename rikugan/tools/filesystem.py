"""Read, write and edit files in the agent's workspace.

These tools are deliberately not ``mutating``: in this codebase that means
"changes the analysis database", and would marshal them onto the UI thread, flush
the result cache and prompt on every write. They keep their own undo records,
because only they know the backup. Paths are confined by core.workspace.

edit_file is exact on purpose. A tool that sometimes guesses right is worse than
one that fails loudly, so nothing here normalizes or fuzzily applies a match; on
failure it says what would fix it.
"""

from __future__ import annotations

import functools
import os
import re
import shutil
from collections.abc import Callable
from typing import TYPE_CHECKING, Annotated, Any

from ..constants import FILESYSTEM_CATEGORY
from ..core.artifacts import (
    INLINE_RESULT_BUDGET,
    MAX_LINE_CHARS,
    format_large_result,
    is_artifact_ref,
    number_line,
    take_within_budget,
)
from ..core.chat_context import current_session
from ..core.errors import UndoNoLongerPossible
from ..core.types import MutationRecord
from ..core.workspace import (
    PathRefused,
    Workspace,
    atomic_write,
    require_readable_text,
    require_writable_size,
)
from .base import tool

if TYPE_CHECKING:
    from ..state.session import FileAccess, SessionState

# Room for a result's title and notice, on top of its lines.
_PAGE_OVERHEAD = 500
_MAX_REPORTED_MATCHES = 20
_SNIPPET_CONTEXT_LINES = 3
_SNIPPET_MAX_LINES = 12
_SNIPPET_LINE_CHARS = 200

_LINE_NUMBER_PREFIX = re.compile(r"^\s*\d+\t", re.MULTILINE)


def _session() -> SessionState:
    session = current_session()
    if session is None:
        raise PathRefused("Error: file tools are only available inside a chat.")
    return session


def _workspace(files: FileAccess) -> Workspace:
    if files.workspace is None:
        raise PathRefused("Error: no workspace is open, so there is nowhere to read or write.")
    return files.workspace


def _refusals_as_text(func: Callable[..., str]) -> Callable[..., str]:
    """Hand refusals back as text, which the model can read and correct.

    Raising would count towards the consecutive-error limit that disables tools,
    and a wrong path is an everyday, recoverable thing.
    """

    @functools.wraps(func)
    def wrapper(*args: Any, **kwargs: Any) -> str:
        try:
            return func(*args, **kwargs)
        except PathRefused as e:
            return str(e)
        except OSError as e:
            return f"Error: {e}"

    return wrapper


def _clip_line(line: str) -> str:
    """Bound a line's length while keeping the file's numbering intact."""
    if line.endswith("\r"):
        line = line[:-1]
    if len(line) <= MAX_LINE_CHARS:
        return line
    return f"{line[:MAX_LINE_CHARS]}… [line continues for {len(line) - MAX_LINE_CHARS:,} more chars]"


def _file_lines(text: str) -> list[str]:
    # Only \n: splitlines() also breaks on form feeds and Unicode separators,
    # numbering lines differently from every editor the user might open this in.
    lines = text.split("\n")
    if lines[-1] == "":
        lines.pop()
    return lines


def _read_page(title: str, lines: list[str], offset: int, limit: int) -> str:
    """One page of a real file. The notice names the call that continues it."""
    total = len(lines)
    if offset > total:
        return f"=== {title} ===\n⟦offset {offset} is past the end: it has {total} lines.⟧"
    end = total if limit <= 0 else min(total, offset + limit - 1)
    wanted = [_clip_line(line) for line in lines[offset - 1 : end]]
    taken = take_within_budget(wanted, offset, INLINE_RESULT_BUDGET - _PAGE_OVERHEAD)
    last = offset + len(taken) - 1
    if last >= total:
        notice = f"⟦Lines {offset}-{last} of {total}: the end.⟧"
    else:
        notice = f"⟦Lines {offset}-{last} of {total}. Continue with offset={last + 1}.⟧"
    return "\n".join([f"=== {title} ===", notice, *taken])


@tool(
    category=FILESYSTEM_CATEGORY,
    result_limit=INLINE_RESULT_BUDGET,
    description=(
        "Read a text file, or a saved result named artifact://N. Lines come back as '<number><tab><text>'. "
        "A large file shows its head and tail and is saved so you can page through the rest; the result "
        "names the call to continue with. Read a file before writing or editing it."
    ),
)
@_refusals_as_text
def read_file(
    file_path: Annotated[str, "File to read, relative to the binary's directory or under scratch/, or artifact://N"],
    offset: Annotated[int, "1-based line to start from"] = 1,
    limit: Annotated[int, "Most lines to return. 0 means as many as fit"] = 0,
) -> str:
    """Read a text file or a saved artifact."""
    files = _session().files
    offset = max(1, offset)
    if is_artifact_ref(file_path):
        return files.artifacts.read(file_path, offset, limit if limit > 0 else 1_000_000)

    path = _workspace(files).resolve(file_path)
    require_readable_text(path)
    # Stat first: a change during the read then looks like a change, not a match.
    mtime = os.stat(path).st_mtime_ns
    with open(path, "rb") as f:
        text = f.read().decode("utf-8", "replace")
    files.read_mtimes[path] = mtime

    lines = _file_lines(text)
    title = f"{file_path} ({len(lines)} lines)"
    if not lines:
        return f"=== {title} ===\n(empty file)"
    if offset == 1 and limit <= 0:
        return _read_whole(title, lines, files)
    return _read_page(title, lines, offset, limit)


def _read_whole(title: str, lines: list[str], files: FileAccess) -> str:
    numbered = [number_line(n, _clip_line(line)) for n, line in enumerate(lines, start=1)]
    if sum(len(line) + 1 for line in numbered) <= INLINE_RESULT_BUDGET - _PAGE_OVERHEAD:
        return "\n".join([f"=== {title} ===", *numbered])
    # Unnumbered: the renderer numbers what it shows, and these stay the file's
    # own numbers because clipped lines never need wrapping.
    return format_large_result(title, "\n".join(_clip_line(line) for line in lines), files.artifacts)


def _require_seen(files: FileAccess, path: str, raw: str) -> None:
    """Refuse to change a file whose current content the model has not read.

    Clobbering content it never saw is the most damaging thing this tool could
    do, and the easiest to prevent.
    """
    known = files.read_mtimes.get(path)
    if known is None:
        raise PathRefused(f"Error: read {raw} with read_file first, so you know what you are replacing.")
    if os.stat(path).st_mtime_ns != known:
        raise PathRefused(f"Error: {raw} changed on disk since you read it. Read it again before changing it.")


def _record_change(session: SessionState, tool_name: str, path: str, backup: str, description: str) -> None:
    """Note the file's new state, and how to put it back."""
    mtime = os.stat(path).st_mtime_ns
    session.files.read_mtimes[path] = mtime
    session.mutation_log.append(
        MutationRecord(
            tool_name=tool_name,
            arguments={"file_path": path},
            reverse_tool="restore_file_backup",
            reverse_arguments={"file_path": path, "backup": backup, "written_mtime_ns": mtime},
            description=description,
        )
    )


@tool(
    category=FILESYSTEM_CATEGORY,
    description=(
        "Create a file, or replace one you have already read. Parent directories are created. "
        "Use edit_file to change part of an existing file. New scripts and notes belong under scratch/."
    ),
)
@_refusals_as_text
def write_file(
    file_path: Annotated[str, "File to write, relative to the binary's directory or under scratch/"],
    content: Annotated[str, "The complete new contents of the file"],
) -> str:
    """Create or replace a file."""
    session = _session()
    files = session.files
    workspace = _workspace(files)
    path = workspace.resolve(file_path, for_write=True)
    data = content.encode("utf-8")
    require_writable_size(data)

    existed = os.path.exists(path)
    backup = ""
    if existed:
        _require_seen(files, path, file_path)
        backup = workspace.backup_file(path)
    atomic_write(path, data)

    line_count = len(_file_lines(content)) if content else 0
    verb = "Replaced" if existed else "Created"
    _record_change(session, "write_file", path, backup, f"{verb} {file_path}")
    return f"{verb} {file_path} ({line_count} lines)."


def _uses_crlf(text: str) -> bool:
    crlf = text.count("\r\n")
    return crlf > text.count("\n") - crlf


def _to_crlf(text: str) -> str:
    return text.replace("\r\n", "\n").replace("\n", "\r\n")


def _line_numbers_of(text: str, needle: str) -> list[int]:
    numbers = []
    start = text.find(needle)
    while start != -1:
        numbers.append(text.count("\n", 0, start) + 1)
        start = text.find(needle, start + len(needle))
    return numbers


def _no_match_message(raw: str, text: str, old: str) -> str:
    """Say what most likely went wrong. One diagnosis, never a fuzzy application."""
    message = f"Error: old_string was not found in {raw}."
    stripped = _LINE_NUMBER_PREFIX.sub("", old)
    if stripped != old and stripped in text:
        return f"{message} It includes line numbers from read_file; leave out each line's number and tab."
    squeeze = lambda value: re.sub(r"\s+", " ", value).strip()  # noqa: E731
    if squeeze(old) and squeeze(old) in squeeze(text):
        return (
            f"{message} The same text exists with different whitespace "
            "(often tabs against spaces). Read those lines again and copy them exactly."
        )
    return f"{message} Read the file again; it may not contain what you expect."


def _edit_snippet(text: str, position: int, new: str) -> str:
    """The edited lines with a little context, so a wrong edit is visible at once."""
    lines = text.split("\n")
    first = text.count("\n", 0, position)
    start = max(0, first - _SNIPPET_CONTEXT_LINES)
    end = min(len(lines), first + new.count("\n") + 1 + _SNIPPET_CONTEXT_LINES)
    shown = lines[start : min(end, start + _SNIPPET_MAX_LINES)]
    rendered = [number_line(start + i + 1, line.rstrip("\r")[:_SNIPPET_LINE_CHARS]) for i, line in enumerate(shown)]
    if start + len(shown) < end:
        rendered.append("…")
    return "\n".join(rendered)


@tool(
    category=FILESYSTEM_CATEGORY,
    description=(
        "Replace exact text in a file you have read. old_string must match byte for byte, indentation "
        "included, and be unique in the file unless replace_all is set: include surrounding lines to make "
        "it unique. Do not include the line numbers read_file shows. Fails rather than guessing."
    ),
)
@_refusals_as_text
def edit_file(
    file_path: Annotated[str, "File to edit, relative to the binary's directory or under scratch/"],
    old_string: Annotated[str, "The exact text to replace"],
    new_string: Annotated[str, "The text to put in its place"],
    replace_all: Annotated[bool, "Replace every occurrence instead of requiring exactly one"] = False,
) -> str:
    """Replace exact text in a file."""
    session = _session()
    files = session.files
    workspace = _workspace(files)
    path = workspace.resolve(file_path, for_write=True)
    if not old_string:
        return "Error: old_string is empty. Use write_file to create a file."
    if old_string == new_string:
        return "Error: old_string and new_string are identical, so this would change nothing."
    require_readable_text(path)
    _require_seen(files, path, file_path)

    with open(path, "rb") as f:
        raw_bytes = f.read()
    try:
        text = raw_bytes.decode("utf-8")
    except UnicodeDecodeError:
        return f"Error: {file_path} is not valid UTF-8, so it cannot be edited without damaging it."

    # read_file shows CRLF files without the \r, so the model's strings have none.
    # Matching them as CRLF keeps the file's line endings instead of mixing them.
    old, new = (_to_crlf(old_string), _to_crlf(new_string)) if _uses_crlf(text) else (old_string, new_string)

    occurrences = text.count(old)
    if occurrences == 0:
        return _no_match_message(file_path, text, old)
    if occurrences > 1 and not replace_all:
        numbers = _line_numbers_of(text, old)
        listed = ", ".join(str(n) for n in numbers[:_MAX_REPORTED_MATCHES])
        more = f" and {len(numbers) - _MAX_REPORTED_MATCHES} more" if len(numbers) > _MAX_REPORTED_MATCHES else ""
        return (
            f"Error: old_string occurs {occurrences} times in {file_path}, at lines {listed}{more}. "
            "Include more surrounding lines to pick one, or set replace_all."
        )

    position = text.index(old)
    updated = text.replace(old, new) if replace_all else text.replace(old, new, 1)
    data = updated.encode("utf-8")
    require_writable_size(data)

    backup = workspace.backup_file(path)
    atomic_write(path, data)
    _record_change(session, "edit_file", path, backup, f"Edited {file_path}")

    summary = f"Edited {file_path}" + (f" ({occurrences} replacements)." if occurrences > 1 else ".")
    return f"{summary}\n{_edit_snippet(updated, position, new)}"


@tool(category=FILESYSTEM_CATEGORY, internal=True)
def restore_file_backup(
    file_path: Annotated[str, "The file a change was made to"],
    backup: Annotated[str, "Token of the backup to restore. Empty means the change created the file"],
    written_mtime_ns: Annotated[int, "The file's mtime right after that change"],
) -> str:
    """Undo a write_file or edit_file. Called by /undo, never by the model."""
    session = _session()
    workspace = _workspace(session.files)
    path = workspace.resolve(file_path, for_write=True)

    exists = os.path.exists(path)
    if exists and os.stat(path).st_mtime_ns != written_mtime_ns:
        raise UndoNoLongerPossible(f"{file_path} has changed since, and undoing would destroy that newer content")

    if not backup:
        if exists:
            os.remove(path)
        session.files.read_mtimes.pop(path, None)
        return f"Removed {file_path}"

    try:
        backup_path = workspace.backup_path(backup)
    except PathRefused as e:
        raise UndoNoLongerPossible(str(e)) from e
    with open(backup_path, "rb") as f:
        atomic_write(path, f.read())
    # The backup's mtime is what the change before this one recorded as "written",
    # so restoring it is what lets a chain of edits undo one after another.
    shutil.copystat(backup_path, path)
    session.files.read_mtimes[path] = os.stat(path).st_mtime_ns
    return f"Restored {file_path}"

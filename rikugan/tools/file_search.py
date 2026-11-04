"""search_files: find text and files in the workspace.

Uses ripgrep when it is installed and a Python scan when it is not, with the same
result shape from both. The two regex dialects differ, so the result names the
engine: the model needs to know which one refused its pattern. Neither follows
symlinks, which is what keeps a search inside the workspace.
"""

from __future__ import annotations

import fnmatch
import os
import re
import shutil
import subprocess
import threading
import time
from collections.abc import Callable
from dataclasses import dataclass, field
from typing import TYPE_CHECKING, Annotated

from ..constants import FILESYSTEM_CATEGORY
from ..core.artifacts import INLINE_RESULT_BUDGET, format_large_result
from ..core.chat_context import current_session
from ..core.workspace import BACKUPS_DIR_NAME, BINARY_SNIFF_BYTES, PathRefused
from .base import tool
from .filesystem import refusals_as_text
from .pagination import DEFAULT_PAGE_LIMIT, format_page

if TYPE_CHECKING:
    from ..state.session import FileAccess

MODE_CONTENT = "content"
MODE_FILES_WITH_MATCHES = "files_with_matches"
MODE_FILES = "files"
_MODES = (MODE_CONTENT, MODE_FILES_WITH_MATCHES, MODE_FILES)

ENGINE_RIPGREP = "ripgrep"
ENGINE_PYTHON = "python"

_SKIPPED_DIRS = (".git", "node_modules", BACKUPS_DIR_NAME)
_MAX_FILE_BYTES = 20 * 1024 * 1024
_MAX_FILE_BYTES_ARG = "20M"
_MAX_SCANNED_FILES = 20_000
_MAX_OUTPUT_BYTES = 8 * 1024 * 1024
_MAX_ROW_CHARS = 500
_MAX_CONTEXT_LINES = 10
_SEARCH_SECONDS = 25.0
_READ_CHUNK = 64 * 1024

_BINARY_NOTICE = "binary file matches"


@dataclass
class _Found:
    rows: list[str] = field(default_factory=list)
    # Why the search did not cover everything. Empty when it did.
    stopped_early: str = ""
    error: str = ""


def _ripgrep_argv(pattern: str, mode: str, glob: str, case_insensitive: bool, context_lines: int) -> list[str]:
    argv = ["rg", "--no-messages", "--color", "never", "--no-heading", "--sort", "path", "--path-separator", "/"]
    # The Python scan has no idea what a .gitignore says, and a sample directory
    # is not a project. Both engines look at the same files.
    argv += ["--no-ignore", "--hidden", "--max-filesize", _MAX_FILE_BYTES_ARG]
    if glob:
        argv += ["--glob", glob]
    for name in _SKIPPED_DIRS:
        argv += ["--glob", f"!**/{name}/**"]
    if mode == MODE_FILES:
        return [*argv, "--files", "./"]
    if case_insensitive:
        argv.append("--ignore-case")
    if mode == MODE_FILES_WITH_MATCHES:
        argv.append("--files-with-matches")
    else:
        argv += ["--line-number", "--with-filename"]
        if context_lines > 0:
            argv += ["--context", str(context_lines)]
    # -- ends the options: a pattern starting with a dash must stay a pattern.
    return [*argv, "--", pattern, "./"]


def _capture_bounded(argv: list[str], cwd: str) -> tuple[bytes, str, int | None, str]:
    """Run *argv* and return (stdout, stderr, returncode, why it was cut short)."""
    process = subprocess.Popen(
        argv,
        cwd=cwd,
        shell=False,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        # Otherwise a GUI host on Windows flashes a console window on every search.
        creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
    )
    reasons: list[str] = []

    def expire() -> None:
        reasons.append(f"it ran past the {_SEARCH_SECONDS:.0f}s limit")
        process.kill()

    timer = threading.Timer(_SEARCH_SECONDS, expire)
    timer.start()
    chunks: list[bytes] = []
    size = 0
    try:
        assert process.stdout is not None
        while chunk := process.stdout.read(_READ_CHUNK):
            chunks.append(chunk)
            size += len(chunk)
            if size >= _MAX_OUTPUT_BYTES:
                reasons.append(f"it matched over {_MAX_OUTPUT_BYTES // (1024 * 1024)}MB")
                process.kill()
                break
        _, stderr = process.communicate()
    finally:
        timer.cancel()
    return b"".join(chunks), stderr.decode("utf-8", "replace"), process.returncode, " and ".join(reasons)


def _search_with_ripgrep(root: str, pattern: str, mode: str, glob: str, nocase: bool, context: int) -> _Found:
    is_file = os.path.isfile(root)
    if is_file and mode == MODE_FILES:
        return _Found(rows=[os.path.basename(root)])
    argv = _ripgrep_argv(pattern, mode, glob, nocase, context)
    # Run from beside a single file, so its path comes back relative as well.
    if is_file:
        argv[-1] = os.path.basename(root)
    stdout, stderr, returncode, stopped = _capture_bounded(argv, os.path.dirname(root) if is_file else root)
    if returncode not in (0, 1) and not stopped:
        return _Found(error=f"Error: ripgrep refused this search: {stderr.strip()}")
    # Everything before the final newline. A cut-off read can end mid-line, and
    # that partial row goes the same way as the empty string after a whole one.
    lines = stdout.decode("utf-8", "replace").split("\n")[:-1]
    rows = [_tidy_row(line) for line in lines if _BINARY_NOTICE not in line]
    return _Found(rows=rows, stopped_early=stopped)


def _tidy_row(row: str) -> str:
    row = row.removesuffix("\r").removeprefix("./")
    if len(row) <= _MAX_ROW_CHARS:
        return row
    return f"{row[:_MAX_ROW_CHARS]}… [line continues]"


def _expand_braces(glob: str) -> list[str]:
    match = re.search(r"\{([^{}]*)\}", glob)
    if match is None:
        return [glob]
    expanded: list[str] = []
    for option in match.group(1).split(","):
        expanded += _expand_braces(glob[: match.start()] + option + glob[match.end() :])
    return expanded


def _glob_matcher(glob: str) -> Callable[[str], bool]:
    """Match as ripgrep does: no slash means the file name, at any depth."""
    if not glob:
        return lambda _path: True
    negated = glob.startswith("!")
    patterns = _expand_braces(glob[1:] if negated else glob)

    def matches(path: str) -> bool:
        name = path.rsplit("/", 1)[-1]
        for pattern in patterns:
            if "/" not in pattern:
                if fnmatch.fnmatchcase(name, pattern):
                    return True
            elif fnmatch.fnmatchcase(path, pattern.lstrip("/")) or fnmatch.fnmatchcase(
                path, pattern.replace("**/", "")
            ):
                return True
        return False

    return (lambda path: not matches(path)) if negated else matches


def _list_files(root: str) -> tuple[list[str], bool]:
    """Relative paths in ripgrep's --sort path order, and whether the list was cut.

    Entries sort within their directory whether file or folder, so this is not
    the files-then-subdirectories order os.walk would naturally produce.
    """
    if os.path.isfile(root):
        return [os.path.basename(root)], False
    found: list[tuple[str, ...]] = []
    truncated = False
    for directory, subdirs, names in os.walk(root, followlinks=False):
        subdirs[:] = [name for name in subdirs if name not in _SKIPPED_DIRS]
        relative = os.path.relpath(directory, root)
        parts = () if relative == "." else tuple(relative.split(os.sep))
        found.extend((*parts, name) for name in names if not os.path.islink(os.path.join(directory, name)))
        if len(found) > _MAX_SCANNED_FILES:
            del found[_MAX_SCANNED_FILES:]
            truncated = True
            break
    return ["/".join(path) for path in sorted(found)], truncated


def _readable_lines(path: str) -> list[str] | None:
    """A file's lines, or None for the files both engines skip."""
    try:
        if os.path.getsize(path) > _MAX_FILE_BYTES:
            return None
        with open(path, "rb") as f:
            data = f.read()
    except OSError:
        return None
    if b"\x00" in data[:BINARY_SNIFF_BYTES]:
        return None
    lines = data.decode("utf-8", "replace").split("\n")
    if lines[-1] == "":
        lines.pop()
    return lines


def _content_rows(relative: str, lines: list[str], regex: re.Pattern[str], context: int) -> list[str]:
    """Render matches as ripgrep does: path:12:text, with context as path-12-text."""
    hits = [number for number, line in enumerate(lines) if regex.search(line)]
    if not hits:
        return []
    hit_set = set(hits)
    wanted = sorted({n for hit in hits for n in range(max(0, hit - context), min(len(lines), hit + context + 1))})
    rows: list[str] = []
    previous = None
    for number in wanted:
        if context > 0 and previous is not None and number != previous + 1:
            rows.append("--")
        separator = ":" if number in hit_set else "-"
        rows.append(_tidy_row(f"{relative}{separator}{number + 1}{separator}{lines[number]}"))
        previous = number
    return rows


def _search_with_python(root: str, pattern: str, mode: str, glob: str, nocase: bool, context: int) -> _Found:
    regex = None
    if mode != MODE_FILES:
        try:
            regex = re.compile(pattern, re.IGNORECASE if nocase else 0)
        except re.error as e:
            return _Found(error=f"Error: Python's re module refused this pattern: {e}")

    base = os.path.dirname(root) if os.path.isfile(root) else root
    matches_glob = _glob_matcher(glob)
    found = _Found()
    deadline = time.monotonic() + _SEARCH_SECONDS
    size = 0
    relative_paths, truncated = _list_files(root)
    if truncated:
        found.stopped_early = f"it reached the {_MAX_SCANNED_FILES:,} file limit"
    for relative in relative_paths:
        if not matches_glob(relative):
            continue
        if mode == MODE_FILES:
            found.rows.append(relative)
            continue
        if time.monotonic() > deadline:
            found.stopped_early = f"it ran past the {_SEARCH_SECONDS:.0f}s limit"
            break
        lines = _readable_lines(os.path.join(base, relative))
        if lines is None or regex is None:
            continue
        rows = _content_rows(relative, lines, regex, context)
        if not rows:
            continue
        if mode == MODE_FILES_WITH_MATCHES:
            found.rows.append(relative)
        else:
            if found.rows and context > 0:
                found.rows.append("--")
            found.rows.extend(rows)
        size += sum(len(row) for row in rows)
        if size >= _MAX_OUTPUT_BYTES:
            found.stopped_early = f"it matched over {_MAX_OUTPUT_BYTES // (1024 * 1024)}MB"
            break
    return found


def _filter_listed_paths(found: _Found, pattern: str, nocase: bool) -> None:
    """In files mode the pattern narrows the listed paths, the same in either engine."""
    if not pattern or found.error:
        return
    try:
        regex = re.compile(pattern, re.IGNORECASE if nocase else 0)
    except re.error as e:
        found.error = f"Error: this mode filters paths with Python's re module, which refused the pattern: {e}"
        return
    found.rows = [row for row in found.rows if regex.search(row)]


def _file_access() -> FileAccess:
    session = current_session()
    if session is None or session.files.workspace is None:
        raise PathRefused("Error: no workspace is open, so there is nothing to search.")
    return session.files


@tool(
    category=FILESYSTEM_CATEGORY,
    timeout=_SEARCH_SECONDS + 5,
    result_limit=INLINE_RESULT_BUDGET,
    description=(
        "Search files with a regular expression, or list files. mode='content' (default) gives "
        "path:line:text rows, mode='files_with_matches' just the paths, and mode='files' every file, with "
        "pattern optionally filtering their paths. glob narrows by name, like '*.py' or 'src/**/*.{c,h}'. "
        "Prefer this to reading whole trees. The result names the regex engine in use."
    ),
)
@refusals_as_text
def search_files(
    pattern: Annotated[str, "Regular expression. Optional in mode='files'"] = "",
    path: Annotated[str, "File or directory to search. Defaults to the binary's directory; scratch/ also works"] = "",
    glob: Annotated[str, "Only files matching this, like '*.py' or '!*.log'"] = "",
    case_insensitive: Annotated[bool, "Ignore case"] = False,
    context_lines: Annotated[int, "Lines to show around each match, in content mode"] = 0,
    mode: Annotated[str, "content, files_with_matches or files"] = MODE_CONTENT,
    offset: Annotated[int, "Result rows to skip, to continue an earlier search"] = 0,
    limit: Annotated[int, "Most rows to return"] = DEFAULT_PAGE_LIMIT,
) -> str:
    """Search files or list them."""
    files = _file_access()
    assert files.workspace is not None
    if mode not in _MODES:
        return f"Error: mode must be one of {', '.join(_MODES)}."
    if mode != MODE_FILES and not pattern:
        return "Error: pattern is required unless mode='files'."

    root = files.workspace.resolve(path) if path else files.workspace.default_root
    if not os.path.exists(root):
        return f"Error: {path or root} does not exist."

    context = max(0, min(context_lines, _MAX_CONTEXT_LINES))
    use_ripgrep = shutil.which("rg") is not None
    engine = ENGINE_RIPGREP if use_ripgrep else ENGINE_PYTHON
    if use_ripgrep:
        found = _search_with_ripgrep(root, pattern, mode, glob, case_insensitive, context)
    else:
        found = _search_with_python(root, pattern, mode, glob, case_insensitive, context)
    if mode == MODE_FILES:
        _filter_listed_paths(found, pattern, case_insensitive)
    if found.error:
        return found.error

    what = f"files in {path or '.'}" if mode == MODE_FILES else f"/{pattern}/ in {path or '.'}"
    header = f"search_files [{engine}]: {what}"
    if not found.rows:
        return f"=== {header} ===\n{_stopped_note(found)}No matches."

    body = format_page(found.rows, offset=offset, limit=limit, title="Rows")
    return format_large_result(header, _stopped_note(found) + body, files.artifacts)


def _stopped_note(found: _Found) -> str:
    if not found.stopped_early:
        return ""
    return f"⟦This search stopped early because {found.stopped_early}, so there may be more. Narrow it.⟧\n"

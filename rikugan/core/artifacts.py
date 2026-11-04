"""Oversized tool output: show both ends, keep the rest readable.

Cutting output at a limit silently teaches the model that a file ended where the
cap fell, and that a command printed nothing after it. So an oversized result
shows its head and its tail, states exactly what was elided, and saves the
output under an ``artifact://N`` handle that ``read_file`` can page through. The
tail matters as much as the head: it holds the summary and the final error.

One-shot output needs this most, since a command's stdout exists nowhere to read
again. Everything here is held in memory and lasts as long as the chat.
"""

from __future__ import annotations

import re
import threading
from collections import OrderedDict
from collections.abc import Iterable
from dataclasses import dataclass

from .workspace import ARTIFACT_SCHEME, PathRefused

# What one tool result may hold, in characters. Whatever renders here must fit,
# or a later cap would cut off the tail and the notice saying what is missing.
INLINE_RESULT_BUDGET = 20_000
# Roughly what the title and notice take. Kept apart from the body's budget.
_NOTICE_RESERVE = 500
_TITLE_MAX_CHARS = 80
_HEAD_SHARE = 0.6

MAX_LINE_CHARS = 2_000
DEFAULT_PAGE_LINES = 400

MAX_ARTIFACT_CHARS = 2_000_000
MAX_STORE_CHARS = 8_000_000
MAX_ARTIFACTS = 32

_ARTIFACT_REF = re.compile(re.escape(ARTIFACT_SCHEME) + r"(\d+)")


def is_artifact_ref(path: str) -> bool:
    return isinstance(path, str) and path.startswith(ARTIFACT_SCHEME)


def number_line(number: int, line: str) -> str:
    return f"{number}\t{line}"


def format_size(n_bytes: int) -> str:
    if n_bytes >= 1024 * 1024:
        return f"{n_bytes / (1024 * 1024):.1f}MB"
    if n_bytes >= 1024:
        return f"{n_bytes / 1024:.1f}KB"
    return f"{n_bytes}B"


def _short_title(title: str) -> str:
    flat = " ".join(title.split())
    return flat if len(flat) <= _TITLE_MAX_CHARS else flat[: _TITLE_MAX_CHARS - 1] + "…"


def _split_lines(text: str) -> list[str]:
    """Split into lines no longer than MAX_LINE_CHARS.

    Output that is one huge line (minified JSON, say) still has to be pageable,
    so a long line is wrapped and each piece counts as a line. Line numbers here
    address the saved output; they are not a real file's numbers.
    """
    lines: list[str] = []
    for raw in text.split("\n"):
        line = raw[:-1] if raw.endswith("\r") else raw
        if len(line) <= MAX_LINE_CHARS:
            lines.append(line)
        else:
            lines.extend(line[i : i + MAX_LINE_CHARS] for i in range(0, len(line), MAX_LINE_CHARS))
    if lines and lines[-1] == "":
        lines.pop()
    return lines


def _byte_size(lines: Iterable[str]) -> int:
    return sum(len(line.encode("utf-8", "replace")) + 1 for line in lines)


def take_within_budget(lines: list[str], first_number: int, budget: int) -> list[str]:
    """Number *lines* and return as many leading ones as fit in *budget* chars."""
    taken: list[str] = []
    used = 0
    for number, line in enumerate(lines, start=first_number):
        rendered = number_line(number, line)
        used += len(rendered) + 1
        if used > budget and taken:
            break
        taken.append(rendered)
    return taken


@dataclass
class Artifact:
    """Saved output. Past the size cap only both ends are kept, as on screen."""

    id: int
    title: str
    head: list[str]
    tail: list[str]
    total_lines: int
    unsaved_bytes: int = 0

    @property
    def ref(self) -> str:
        return f"{ARTIFACT_SCHEME}{self.id}"

    @property
    def chars(self) -> int:
        return sum(len(line) + 1 for line in self.head) + sum(len(line) + 1 for line in self.tail)

    @property
    def tail_first_line(self) -> int:
        return self.total_lines - len(self.tail) + 1

    @property
    def has_gap(self) -> bool:
        return self.tail_first_line > len(self.head) + 1

    def read_page(self, offset: int = 1, limit: int = DEFAULT_PAGE_LINES) -> str:
        """Render lines from *offset*, numbered as in the original output."""
        offset = max(1, offset)
        limit = max(1, limit)
        header = f"=== {self.ref}: {self.title} ==="
        if self.total_lines == 0:
            return f"{header}\n⟦This output was empty.⟧"
        if offset > self.total_lines:
            return f"{header}\n⟦offset {offset} is past the end: it has {self.total_lines} lines.⟧"
        if len(self.head) < offset < self.tail_first_line:
            return (
                f"{header}\n⟦Lines {len(self.head) + 1}-{self.tail_first_line - 1} were never saved: "
                f"{format_size(self.unsaved_bytes)} from the middle was over the size limit. Saved: "
                f"1-{len(self.head)} and {self.tail_first_line}-{self.total_lines}. "
                f'Continue with read_file("{self.ref}", offset={self.tail_first_line}).⟧'
            )
        if offset <= len(self.head):
            available = self.head[offset - 1 :]
        else:
            available = self.tail[offset - self.tail_first_line :]
        taken = take_within_budget(available[:limit], offset, INLINE_RESULT_BUDGET - _NOTICE_RESERVE)
        last = offset + len(taken) - 1
        return "\n".join([header, self._page_notice(offset, last), *taken])

    def _page_notice(self, first: int, last: int) -> str:
        where = f"Lines {first}-{last} of {self.total_lines}"
        if last >= self.total_lines:
            return f"⟦{where}: the end.⟧"
        if last == len(self.head) and self.has_gap:
            return (
                f"⟦{where}. The next {self.tail_first_line - last - 1} lines were never saved. "
                f'Continue with read_file("{self.ref}", offset={self.tail_first_line}).⟧'
            )
        return f'⟦{where}. Continue with read_file("{self.ref}", offset={last + 1}).⟧'


class ArtifactStore:
    """One chat's saved output, least recently used dropped first.

    Ids are never reused, so an old handle can only say it is gone rather than
    silently return different output.
    """

    def __init__(self) -> None:
        self._artifacts: OrderedDict[int, Artifact] = OrderedDict()
        self._next_id = 1
        self._lock = threading.Lock()

    def save(self, title: str, lines: list[str]) -> Artifact:
        head, tail, unsaved = _fit_to_artifact(lines)
        with self._lock:
            artifact = Artifact(self._next_id, _short_title(title), head, tail, len(lines), unsaved)
            self._next_id += 1
            self._artifacts[artifact.id] = artifact
            self._evict(keep=artifact.id)
        return artifact

    def get(self, artifact_id: int) -> Artifact:
        with self._lock:
            artifact = self._artifacts.get(artifact_id)
            if artifact is None:
                ref = f"{ARTIFACT_SCHEME}{artifact_id}"
                if 0 < artifact_id < self._next_id:
                    raise PathRefused(f"Error: {ref} was dropped to make room. Run what produced it again.")
                raise PathRefused(f"Error: {ref} does not exist.")
            self._artifacts.move_to_end(artifact_id)
            return artifact

    def read(self, ref: str, offset: int = 1, limit: int = DEFAULT_PAGE_LINES) -> str:
        match = _ARTIFACT_REF.fullmatch(ref)
        if match is None:
            raise PathRefused(f"Error: '{ref}' is not an artifact handle. They look like {ARTIFACT_SCHEME}12.")
        return self.get(int(match.group(1))).read_page(offset, limit)

    def _evict(self, keep: int) -> None:
        total = sum(a.chars for a in self._artifacts.values())
        for artifact_id in list(self._artifacts):
            if len(self._artifacts) <= MAX_ARTIFACTS and total <= MAX_STORE_CHARS:
                return
            if artifact_id != keep:
                total -= self._artifacts.pop(artifact_id).chars


def _fit_to_artifact(lines: list[str]) -> tuple[list[str], list[str], int]:
    """Split *lines* into a saved head and tail, plus the bytes left unsaved."""
    if sum(len(line) + 1 for line in lines) <= MAX_ARTIFACT_CHARS:
        return lines, [], 0
    head = _take_chars(lines, int(MAX_ARTIFACT_CHARS * _HEAD_SHARE))
    remaining = lines[len(head) :]
    tail = _take_chars(remaining[::-1], MAX_ARTIFACT_CHARS - int(MAX_ARTIFACT_CHARS * _HEAD_SHARE))[::-1]
    middle = remaining[: len(remaining) - len(tail)]
    return head, tail, _byte_size(middle)


def _take_chars(lines: list[str], budget: int) -> list[str]:
    taken: list[str] = []
    used = 0
    for line in lines:
        used += len(line) + 1
        if used > budget:
            break
        taken.append(line)
    return taken


def format_large_result(
    title: str,
    text: str,
    store: ArtifactStore,
    *,
    footer: str = "",
    budget: int = INLINE_RESULT_BUDGET,
) -> str:
    """Render *text* for the model, within budget however large it is.

    Output that fits comes back as it is. Anything larger is saved to *store*
    and shown as numbered head and tail, with a notice directly under the title:
    a later cap may shorten this result, and the notice must survive that.
    """
    header = f"=== {_short_title(title)} ==="
    closing = f"\n— {footer} —" if footer else ""
    if len(header) + 1 + len(text) + len(closing) <= budget:
        return f"{header}\n{text.rstrip(chr(10))}{closing}"

    lines = _split_lines(text)
    artifact = store.save(title, lines)
    body_budget = budget - _NOTICE_RESERVE - len(header) - len(closing)
    head = take_within_budget(lines, 1, int(body_budget * _HEAD_SHARE))
    tail_budget = body_budget - sum(len(line) + 1 for line in head)
    # Taken from the far end, and never reaching back over what the head shows.
    tail_count = _count_from_end(lines[len(head) :], len(lines), tail_budget)
    tail_first = len(lines) - tail_count + 1
    tail = [number_line(n, lines[n - 1]) for n in range(tail_first, len(lines) + 1)]

    notice = _inline_notice(artifact, lines, len(head), tail_first)
    return "\n".join([header, notice, *head, *tail]) + closing


def _count_from_end(lines: list[str], last_number: int, budget: int) -> int:
    used = 0
    count = 0
    for offset, line in enumerate(reversed(lines)):
        used += len(number_line(last_number - offset, line)) + 1
        if used > budget:
            break
        count += 1
    return count


def _inline_notice(artifact: Artifact, lines: list[str], head_count: int, tail_first: int) -> str:
    total = len(lines)
    elided_lines = tail_first - head_count - 1
    shown = f"Showing lines 1-{head_count}"
    if tail_first <= total:
        shown += f" and {tail_first}-{total}"
    elided = lines[head_count : tail_first - 1]
    size = format_size(_byte_size(elided))
    if artifact.unsaved_bytes:
        where = (
            f"Read {artifact.ref} for a head/tail sample; "
            f"{format_size(artifact.unsaved_bytes)} from its middle was not saved"
        )
    else:
        where = f'Continue with read_file("{artifact.ref}", offset={head_count + 1})'
    return f"⟦{shown} of {total}; {elided_lines:,} middle lines ({size}) elided. {where}⟧"

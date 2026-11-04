"""Where the agent may read and write files.

A path from the model is untrusted: text inside a binary can steer the model, so
every filesystem tool resolves its path here before touching the disk. Refusals
carry the fix, because the model reads them and has to be able to recover.

This confines the file tools. It does not sandbox the machine: ``run_shell``
runs real commands behind the same user consent, and nothing here limits them.
"""

from __future__ import annotations

import os
import re
import shutil
import time
import uuid
from collections.abc import Iterable
from dataclasses import dataclass, field

from .errors import ToolError

SCRATCH_ALIAS = "scratch"
BACKUPS_DIR_NAME = ".backups"
MEMORY_FILE_NAME = "RIKUGAN.md"
ARTIFACT_SCHEME = "artifact://"

MAX_READ_BYTES = 50 * 1024 * 1024
MAX_WRITE_BYTES = 5 * 1024 * 1024
BINARY_SNIFF_BYTES = 8192

MAX_BACKUP_FILES = 200
MAX_BACKUP_BYTES = 100 * 1024 * 1024

# Never written, whichever database they belong to: a write would corrupt one.
_DATABASE_SUFFIXES = frozenset({".idb", ".i64", ".bndb"})
# Only meaningful beside the open database. Elsewhere .til and .nam are ordinary.
_DATABASE_COMPANION_SUFFIXES = (".id0", ".id1", ".id2", ".nam", ".til")

_BACKUP_TOKEN = re.compile(r"[0-9a-f]{16}-[0-9a-f]{8}")
_UNSAFE_ID_CHARS = re.compile(r"[^A-Za-z0-9_-]")


class PathRefused(ToolError):
    """The path or file cannot be used. The message tells the model what to do."""


def _within(path: str, root: str) -> bool:
    """Whether *path* is *root* or inside it.

    ``commonpath`` compares whole components, so /work/bin_evil is not inside
    /work/bin — which a prefix check would accept.
    """
    path, root = os.path.normcase(path), os.path.normcase(root)
    try:
        return os.path.commonpath([path, root]) == root
    except ValueError:  # different Windows drives, or mixing absolute and relative
        return False


def _usable_binary_root(database_path: str, home: str) -> str:
    """The binary's directory, unless that would expose the user's whole home.

    A sample opened from ~ or / would otherwise make ~/.ssh and every dotfile
    readable and writable, which nobody meant by "the folder the sample is in".
    """
    if not database_path:
        return ""
    directory = os.path.dirname(os.path.realpath(database_path))
    if not directory or _within(os.path.realpath(home), directory):
        return ""
    return directory


def _protected_paths(paths: Iterable[str]) -> frozenset[str]:
    """Case-folded paths never to write: the given files and database companions."""
    protected: set[str] = set()
    for path in paths:
        if not path:
            continue
        real = os.path.realpath(path)
        protected.add(real.casefold())
        stem = os.path.splitext(real)[0]
        protected.update((stem + suffix).casefold() for suffix in _DATABASE_COMPANION_SUFFIXES)
    return frozenset(protected)


@dataclass(frozen=True)
class Workspace:
    """The directories the agent may use, and the files in them it must not write."""

    scratch_root: str
    binary_root: str = ""
    extra_roots: tuple[str, ...] = ()
    protected: frozenset[str] = field(default_factory=frozenset)

    @classmethod
    def for_database(
        cls,
        database_path: str,
        config_dir: str,
        instance_id: str = "",
        input_path: str = "",
        extra_roots: Iterable[str] = (),
        home: str = "",
    ) -> Workspace:
        """Build the workspace for an open database.

        Scratch sits under the config directory rather than beside the binary:
        sample directories are often read-only, scanned, or tracked for
        provenance, and a stray file there can matter.
        """
        safe_id = _UNSAFE_ID_CHARS.sub("_", instance_id) or "default"
        return cls(
            scratch_root=os.path.realpath(os.path.join(config_dir, "scratch", safe_id)),
            binary_root=_usable_binary_root(database_path, home or os.path.expanduser("~")),
            extra_roots=tuple(os.path.realpath(os.path.expanduser(root)) for root in extra_roots if root),
            protected=_protected_paths((database_path, input_path)),
        )

    @property
    def default_root(self) -> str:
        """Where a relative path and a command's working directory start."""
        return self.binary_root or self.scratch_root

    @property
    def roots(self) -> tuple[str, ...]:
        binary = (self.binary_root,) if self.binary_root else ()
        return (*binary, self.scratch_root, *self.extra_roots)

    def describe_roots(self) -> str:
        parts = []
        if self.binary_root:
            parts.append(f"{self.binary_root} (binary directory)")
        parts.append(f"{SCRATCH_ALIAS}/ (agent scratch, at {self.scratch_root})")
        parts.extend(f"{root} (configured)" for root in self.extra_roots)
        return "Allowed: " + ", ".join(parts) + "."

    def describe_for_prompt(self) -> str:
        """Where things really are, so the model has one picture of the disk."""
        lines = []
        if self.binary_root:
            lines.append(f"Binary directory: {self.binary_root} (relative paths and commands start here)")
        lines.append(f"Scratch directory: {self.scratch_root}")
        lines.extend(f"Also allowed: {root}" for root in self.extra_roots)
        return "\n".join(lines)

    def contains(self, path: str) -> bool:
        return any(_within(path, root) for root in self.roots)

    def resolve(self, raw: str, *, for_write: bool = False) -> str:
        """Return the real absolute path for *raw*, or raise PathRefused."""
        candidate = self._candidate(raw)
        if for_write:
            return self._resolve_for_write(raw, candidate)
        resolved = os.path.realpath(candidate)
        self._require_inside(raw, resolved)
        return resolved

    def _candidate(self, raw: str) -> str:
        if not isinstance(raw, str) or not raw.strip():
            raise PathRefused("Error: path refused — it is empty.")
        if "\x00" in raw:
            raise PathRefused("Error: path refused — it contains a NUL byte.")
        if raw.startswith(ARTIFACT_SCHEME):
            raise PathRefused(f"Error: path refused — {raw} is a saved result, and only read_file can open one.")
        if raw.startswith("~"):
            raise PathRefused(f"Error: path refused — '~' is not expanded. {self.describe_roots()}")
        if os.path.isabs(raw):
            return raw
        head, _, tail = raw.replace(os.sep, "/").partition("/")
        if head == SCRATCH_ALIAS:
            return os.path.join(self.scratch_root, tail)
        return os.path.join(self.default_root, raw)

    def _require_inside(self, raw: str, resolved: str) -> None:
        if not self.contains(resolved):
            raise PathRefused(f"Error: path refused — '{raw}' is outside the workspace.\n{self.describe_roots()}")

    def _resolve_for_write(self, raw: str, candidate: str) -> str:
        # Only the parent is resolved: the file may not exist yet, and following
        # a final symlink would write through it to wherever it points.
        name = os.path.basename(candidate.rstrip("/" + os.sep))
        if name in ("", ".", ".."):
            raise PathRefused(f"Error: path refused — '{raw}' does not name a file.")
        resolved = os.path.join(os.path.realpath(os.path.dirname(candidate.rstrip("/" + os.sep))), name)
        self._require_inside(raw, resolved)
        if os.path.islink(resolved):
            raise PathRefused(f"Error: path refused — '{raw}' is a symlink. Write to the file it points at instead.")
        if os.path.isdir(resolved):
            raise PathRefused(f"Error: path refused — '{raw}' is a directory.")
        self._require_writable(raw, resolved)
        return resolved

    def _require_writable(self, raw: str, resolved: str) -> None:
        folded = resolved.casefold()
        if folded in self.protected or os.path.splitext(folded)[1] in _DATABASE_SUFFIXES:
            raise PathRefused(
                f"Error: path refused — '{raw}' is the analysed binary or a database. "
                "Change the database with the analysis tools; put new files in scratch/."
            )
        if os.path.basename(folded) == MEMORY_FILE_NAME.casefold():
            raise PathRefused(f"Error: path refused — {MEMORY_FILE_NAME} is written with save_memory, not directly.")
        if BACKUPS_DIR_NAME in folded.split(os.sep):
            raise PathRefused(f"Error: path refused — {BACKUPS_DIR_NAME}/ holds undo backups and cannot be changed.")

    # --- Backups -----------------------------------------------------------

    @property
    def backups_dir(self) -> str:
        return os.path.join(self.scratch_root, BACKUPS_DIR_NAME)

    def backup_file(self, path: str) -> str:
        """Copy *path* aside for undo and return the token that restores it."""
        os.makedirs(self.backups_dir, exist_ok=True)
        token = f"{time.time_ns():016x}-{uuid.uuid4().hex[:8]}"
        shutil.copy2(path, os.path.join(self.backups_dir, token))
        self._prune_backups(keep=token)
        return token

    def backup_path(self, token: str) -> str:
        """Where *token*'s backup is stored, or raise if there is none."""
        if not _BACKUP_TOKEN.fullmatch(token):
            raise PathRefused(f"Error: '{token}' is not a backup token.")
        path = os.path.join(self.backups_dir, token)
        if not os.path.isfile(path):
            raise PathRefused("Error: the backup for this change has been discarded, so it cannot be restored.")
        return path

    def _prune_backups(self, keep: str) -> None:
        """Hold backups to their caps, oldest first. Tokens sort by creation time."""
        tokens = sorted(name for name in os.listdir(self.backups_dir) if _BACKUP_TOKEN.fullmatch(name))
        sizes = {name: os.path.getsize(os.path.join(self.backups_dir, name)) for name in tokens}
        total = sum(sizes.values())
        for name in tokens:
            if name == keep or (len(sizes) <= MAX_BACKUP_FILES and total <= MAX_BACKUP_BYTES):
                continue
            os.remove(os.path.join(self.backups_dir, name))
            total -= sizes.pop(name)


def require_readable_text(path: str) -> None:
    """Raise PathRefused unless *path* is a regular file reasonable to read as text."""
    if not os.path.exists(path):
        raise PathRefused(f"Error: {path} does not exist.")
    if os.path.isdir(path):
        raise PathRefused(f"Error: {path} is a directory. Use search_files to look through it.")
    # A FIFO or device passes an existence check and then blocks open() for ever.
    if not os.path.isfile(path):
        raise PathRefused(f"Error: {path} is not a regular file.")
    size = os.path.getsize(path)
    if size > MAX_READ_BYTES:
        raise PathRefused(
            f"Error: {path} is {size:,} bytes, over the {MAX_READ_BYTES:,} byte limit for reading. "
            "Use search_files to find the part you need."
        )
    with open(path, "rb") as f:
        if b"\x00" in f.read(BINARY_SNIFF_BYTES):
            raise PathRefused(f"Error: {path} looks binary. Use the analysis tools, or run_shell with a hex dumper.")


def require_writable_size(data: bytes) -> None:
    if len(data) > MAX_WRITE_BYTES:
        raise PathRefused(
            f"Error: refusing to write {len(data):,} bytes in one call; the limit is {MAX_WRITE_BYTES:,}."
        )


def atomic_write(path: str, data: bytes) -> None:
    """Replace *path* with *data* so a failure leaves the old content intact."""
    directory = os.path.dirname(path)
    os.makedirs(directory, exist_ok=True)
    tmp_path = os.path.join(directory, f".rikugan-{uuid.uuid4().hex}.tmp")
    # os.open applies the umask itself. mkstemp would leave a new file private,
    # and reading the umask to correct that means briefly changing it for every
    # thread in the host process.
    fd = os.open(tmp_path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o666)
    try:
        with os.fdopen(fd, "wb") as f:
            f.write(data)
            f.flush()
            os.fsync(f.fileno())
        if os.path.exists(path):
            shutil.copymode(path, tmp_path)
        os.replace(tmp_path, path)
    except BaseException:
        if os.path.exists(tmp_path):
            os.remove(tmp_path)
        raise

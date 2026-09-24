"""Parse ``get_binary_info`` output into a compact card summary.

Both host implementations of the ``get_binary_info`` tool emit the same
``Key: value`` line format, so a single parser serves IDA and Binary Ninja.
Everything here is pure string handling — no Qt, no host APIs — so the welcome
card can be unit tested without a running host.
"""

from __future__ import annotations

import os
import re
from dataclasses import dataclass, field

_INFO_LINE_RE = re.compile(r"^\s*([A-Za-z][A-Za-z ]*?)\s*:\s*(.+?)\s*$")
_TOTAL_COUNT_RE = re.compile(r"\bof\s+(\d+)\s*:")
_TRAILING_PAREN_RE = re.compile(r"\s*\(([^()]{1,12})\)\s*$")
_HEX_RE = re.compile(r"0x[0-9a-fA-F]+")

# IDA reports the x86 family as "metapc"; the bit width lives on its own line.
_PROCESSOR_ALIASES: dict[tuple[str, str], str] = {
    ("metapc", "64"): "x86_64",
    ("metapc", "32"): "x86",
    ("metapc", "16"): "x86_16",
    ("arm", "64"): "arm64",
    ("arm", "32"): "arm32",
    ("ppc", "64"): "ppc64",
    ("mips", "64"): "mips64",
}

_MAX_FILE_TYPE_CHARS = 14


@dataclass
class BinarySummary:
    """Display model for the welcome screen's binary card."""

    name: str = ""
    chips: list[str] = field(default_factory=list)

    @property
    def is_empty(self) -> bool:
        return not self.name and not self.chips

    @property
    def short_name(self) -> str:
        """File name without its directory, for the width the card has."""
        return os.path.basename(self.name) if self.name else ""


def parse_info_lines(raw: str) -> dict[str, str]:
    """Split ``Key: value`` tool output into a lowercase-keyed mapping."""
    fields: dict[str, str] = {}
    for line in (raw or "").splitlines():
        match = _INFO_LINE_RE.match(line)
        if match:
            fields.setdefault(match.group(1).strip().lower(), match.group(2).strip())
    return fields


def parse_total_count(raw: str) -> int | None:
    """Read the ``<Title> 0-1 of <N>:`` total out of a paginated tool result."""
    match = _TOTAL_COUNT_RE.search(raw or "")
    if match is None:
        return None
    try:
        return int(match.group(1))
    except ValueError:
        return None


def format_processor(processor: str, bits: str) -> str:
    """Normalize processor/bit-width pairs into one short architecture chip."""
    proc = (processor or "").strip()
    if not proc:
        return ""
    alias = _PROCESSOR_ALIASES.get((proc.lower(), (bits or "").strip()))
    if alias:
        return alias
    if bits and bits not in proc:
        return f"{proc}{bits}"
    return proc


def format_file_type(file_type: str) -> str:
    """Shorten a verbose file-type string, preferring a trailing abbreviation."""
    value = (file_type or "").strip()
    if not value:
        return ""
    abbrev = ""
    paren = _TRAILING_PAREN_RE.search(value)
    if paren:
        candidate = paren.group(1).strip()
        if len(candidate) <= 5:
            abbrev = candidate
        value = value[: paren.start()].strip()
    for suffix in (" file", " executable", " binary"):
        if value.lower().endswith(suffix):
            value = value[: -len(suffix)].strip()
            break
    if len(value) <= _MAX_FILE_TYPE_CHARS:
        return value
    if abbrev:
        return abbrev
    return value[: _MAX_FILE_TYPE_CHARS - 1] + "…"


def format_address(value: str) -> str:
    """Return the first hex address in *value*, lowercased."""
    match = _HEX_RE.search(value or "")
    return match.group(0).lower() if match else ""


def _count_chip(value: str, unit: str) -> str:
    try:
        count = int(value.replace(",", "").strip())
    except (AttributeError, ValueError):
        return ""
    return f"{count:,} {unit}"


def build_binary_summary(raw_info: str, string_count: int | None = None) -> BinarySummary:
    """Build the welcome card model from ``get_binary_info`` output.

    ``string_count`` comes from a separate paginated ``list_strings`` probe and
    is omitted from the chips when unavailable.
    """
    fields = parse_info_lines(raw_info)
    if not fields:
        return BinarySummary()

    name = fields.get("file", "")
    chips: list[str] = []

    arch = format_processor(fields.get("processor", ""), fields.get("bits", ""))
    if arch:
        chips.append(arch)

    file_type = format_file_type(fields.get("file type", ""))
    if file_type:
        chips.append(file_type)

    functions = _count_chip(fields.get("functions", ""), "fn")
    if functions:
        chips.append(functions)

    if string_count is not None and string_count >= 0:
        chips.append(f"{string_count:,} str")

    entry = format_address(fields.get("entry point", ""))
    if entry:
        chips.append(f"entry {entry}")

    return BinarySummary(name=name, chips=chips)

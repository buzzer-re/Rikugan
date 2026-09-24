"""System prompt builder with binary context awareness."""

from __future__ import annotations

import os
from typing import TYPE_CHECKING

from ..core.logging import log_debug
from ..core.profile import IOC_FILTER_CATEGORIES
from ..core.sanitize import sanitize_binary_context, sanitize_memory
from .prompts.binja import BINJA_BASE_PROMPT, NATIVE_MCP_SECTION, NATIVE_MCP_TOOL_PREFIX
from .prompts.ida import IDA_BASE_PROMPT

_PERSISTENT_MEMORY_CACHE: dict[str, tuple[tuple[int | None, int | None], str | None]] = {}

if TYPE_CHECKING:
    from ..core.profile import AnalysisProfile

_HOST_PROMPTS = {"IDA Pro": IDA_BASE_PROMPT, "Binary Ninja": BINJA_BASE_PROMPT}
_BASE_PROMPT = IDA_BASE_PROMPT  # backward compat alias

# Maximum number of lines to load from RIKUGAN.md
_MAX_MEMORY_LINES = 200


def _load_persistent_memory(idb_dir: str = "") -> str | None:
    """Load RIKUGAN.md from the IDB/BNDB directory (first 200 lines).

    The file acts as persistent cross-session memory for the agent.
    """
    if not idb_dir:
        return None

    md_path = os.path.join(idb_dir, "RIKUGAN.md")
    if not os.path.isfile(md_path):
        _PERSISTENT_MEMORY_CACHE.pop(md_path, None)
        return None

    try:
        stat_result = os.stat(md_path)
        version = (
            getattr(stat_result, "st_mtime_ns", None),
            getattr(stat_result, "st_size", None),
        )
    except OSError as e:
        log_debug(f"Failed to stat RIKUGAN.md: {e}")
        _PERSISTENT_MEMORY_CACHE.pop(md_path, None)
        return None

    cached = _PERSISTENT_MEMORY_CACHE.get(md_path)
    if cached is not None and cached[0] == version:
        return cached[1]

    try:
        with open(md_path, encoding="utf-8") as f:
            lines = []
            for i, line in enumerate(f):
                if i >= _MAX_MEMORY_LINES:
                    lines.append(f"\n... (truncated at {_MAX_MEMORY_LINES} lines)")
                    break
                lines.append(line)
        content = "".join(lines).strip() or None
        _PERSISTENT_MEMORY_CACHE[md_path] = (version, content)
        if content:
            log_debug(f"Loaded persistent memory from {md_path} ({len(lines)} lines)")
            return content
    except OSError as e:
        log_debug(f"Failed to load RIKUGAN.md: {e}")
        _PERSISTENT_MEMORY_CACHE.pop(md_path, None)

    return None


def build_system_prompt(
    host_name: str = "IDA Pro",
    binary_info: str | None = None,
    current_function: str | None = None,
    current_address: str | None = None,
    extra_context: str | None = None,
    tool_names: list[str] | None = None,
    skill_summary: str | None = None,
    idb_dir: str | None = None,
    profile: AnalysisProfile | None = None,
) -> str:
    """Build the full system prompt with optional binary context."""
    base_prompt = _HOST_PROMPTS.get(host_name, IDA_BASE_PROMPT)
    parts = [base_prompt]

    # Persistent memory — loaded early so it's part of the cached prefix.
    # Sanitized to prevent poisoned memory files from injecting instructions.
    memory = _load_persistent_memory(idb_dir or "")
    if memory:
        parts.append(f"\n## Persistent Memory (RIKUGAN.md)\n{sanitize_memory(memory)}")

    # Binary context is untrusted — function names, strings, and metadata
    # originate from the analyzed binary and could contain adversarial content.
    # When profile.hide_binary_metadata is set, skip binary context entirely.
    if profile and profile.hide_binary_metadata:
        log_debug("Profile: hiding binary metadata from system prompt")
    else:
        if binary_info:
            parts.append(f"\n## Current Binary\n{sanitize_binary_context(binary_info, 'binary_info')}")

        if current_address:
            parts.append(
                f"\n## Current Position\nAddress: {sanitize_binary_context(current_address, 'cursor_address')}"
            )
            if current_function:
                parts.append(f"Function: {sanitize_binary_context(current_function, 'cursor_function')}")

    if tool_names:
        parts.append(f"\n## Available Tools\n{', '.join(tool_names)}")
        # Only stated when the host's own MCP tools are actually registered, so
        # the agent is never told to prefer tools it does not have.
        if any(name.startswith(NATIVE_MCP_TOOL_PREFIX) for name in tool_names):
            parts.append(NATIVE_MCP_SECTION)

    if skill_summary:
        parts.append(f"\n## Skills\n{skill_summary}")

    if extra_context:
        parts.append(f"\n## Additional Context\n{extra_context}")

    # Profile-driven prompt additions
    if profile:
        if profile.singular_analysis:
            parts.append(
                "\n## Analysis Constraint\n"
                "You are operating in singular analysis mode. "
                "Focus only on the specific question asked. "
                "Do not reference or cross-correlate with other binaries, "
                "samples, or external threat intelligence."
            )
        if profile.custom_filters:
            parts.append("\n## Profile Instructions\n" + "\n".join(profile.custom_filters))
        if profile.denied_functions:
            parts.append(
                "\n## Restricted Functions\n"
                "Do NOT call or reference the following functions in your analysis:\n"
                + "\n".join(f"- {fn}" for fn in profile.denied_functions)
            )

        # Profile awareness — tell the agent about the active profile
        if profile.name != "default":
            section = f"\n## Active Profile: {profile.name}\n"
            if profile.description:
                section += f"{profile.description}\n\n"
            section += (
                "You are operating under this analysis profile. "
                "The user has configured specific constraints and data filters. "
                "Respect these constraints in your analysis and output.\n"
            )
            if profile.has_any_ioc_filter:
                active = [
                    IOC_FILTER_CATEGORIES[k] for k, v in profile.ioc_filters.items() if v and k in IOC_FILTER_CATEGORIES
                ]
                if active:
                    section += (
                        "\nIOC filtering is active — the following are automatically redacted:\n"
                        + "\n".join(f"- {f}" for f in active)
                        + "\n\nIMPORTANT CONSTRAINTS:\n"
                        "- Do NOT attempt to reconstruct or reference original values "
                        "behind redaction markers.\n"
                        "- Hex-encoded data (hexdumps, raw bytes) is also sanitized — "
                        "do NOT decode hex bytes to recover filtered IOC data.\n"
                        "- Do NOT use read_bytes or memory dumps to circumvent IOC filters.\n"
                        "- If a value has been redacted, treat it as permanently unavailable.\n"
                    )
            parts.append(section)

    return "\n".join(parts)

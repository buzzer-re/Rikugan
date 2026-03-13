"""Subagent perks: optional analysis instructions that augment agent prompts."""

from __future__ import annotations

SUBAGENT_PERKS: dict[str, str] = {
    "deep_decompilation": (
        "When analyzing functions, always check callers and callees up to 3 "
        "levels deep. Decompile every function you reference."
    ),
    "string_harvesting": (
        "List ALL string references in every function you analyze. Include cross-references to those strings."
    ),
    "import_mapping": (
        "Map every imported API call. Note which functions call which imports and what arguments they pass."
    ),
    "memory_layout": (
        "Analyze stack frame layouts, global variable accesses, and "
        "structure field offsets for every function you examine."
    ),
    "hypothesis_mode": (
        "After initial analysis, generate 3 hypotheses about the code's "
        "purpose. Then systematically test each hypothesis using the "
        "available tools. Report which hypotheses were confirmed or rejected."
    ),
}


def build_perks_addendum(perks: list[str]) -> str:
    """Build the combined system prompt addendum from selected perks."""
    parts = [SUBAGENT_PERKS[p] for p in perks if p in SUBAGENT_PERKS]
    if not parts:
        return ""
    return "Additional analysis instructions:\n\n" + "\n\n".join(parts)

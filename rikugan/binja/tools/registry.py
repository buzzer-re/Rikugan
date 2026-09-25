"""Binary Ninja tool registry.

Wires Binary Ninja-specific tool modules into the shared ToolRegistry.
"""

from __future__ import annotations

from ...core.thread_safety import idasync
from ...tools.registry import ToolRegistry
from . import (  # type: ignore[assignment]
    annotations,
    database,
    decompiler,
    disassembly,
    functions,
    il,
    il_analysis,
    il_transform,
    navigation,
    scripting,
    strings,
    types_tools,
    xrefs,
)

_TOOL_MODULES = (
    navigation,
    functions,
    strings,
    database,
    disassembly,
    decompiler,
    xrefs,
    annotations,
    types_tools,
    scripting,
    il,
    il_analysis,
    il_transform,
)


# Tools that drive Binary Ninja's UI rather than read its analysis. These must
# stay on the main thread whatever the setting.
_UI_THREAD_TOOLS = frozenset({"jump_to", "navigate_to", "execute_python"})


def _needs_main_thread(defn) -> bool:
    """Whether *defn* has to be marshalled onto Binary Ninja's UI thread.

    BinaryView analysis is thread-safe, so reads can run on the agent's
    background thread. Marshalling them made every decompile freeze the whole
    window — no repaints, no event pump, no spinners — for its full duration.
    Anything that writes to the database or drives the UI still goes through
    the main thread.
    """
    return bool(getattr(defn, "mutating", False)) or defn.name in _UI_THREAD_TOOLS


def create_default_registry(config=None) -> ToolRegistry:
    """Create a Binary Ninja registry with all built-in BN tools.

    *config* is optional so existing no-argument callers keep working; without
    it the conservative main-thread behavior is used.
    """
    background_reads = bool(getattr(config, "binja_background_tools", False))
    registry = ToolRegistry(
        dispatch_wrapper=idasync,
        marshal_policy=_needs_main_thread if background_reads else None,
    )
    for mod in _TOOL_MODULES:
        registry.register_module(mod)
    return registry

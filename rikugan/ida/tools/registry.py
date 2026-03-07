"""IDA tool registry: wires IDA-specific tool modules into the shared ToolRegistry."""

from __future__ import annotations

from rikugan.constants import HAS_HEXRAYS
from rikugan.tools.registry import ToolRegistry
from rikugan.tools import (
    navigation, functions, strings, database,
    disassembly, decompiler, xrefs, annotations,
    types_tools, scripting, microcode,
)

_TOOL_MODULES = (
    navigation, functions, strings, database,
    disassembly, decompiler, xrefs, annotations,
    types_tools, scripting, microcode,
)


def create_default_registry() -> ToolRegistry:
    """Create a registry with all built-in IDA tools."""
    registry = ToolRegistry()
    registry.set_capabilities({"hexrays": HAS_HEXRAYS})
    for mod in _TOOL_MODULES:
        registry.register_module(mod)
    return registry

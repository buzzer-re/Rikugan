"""IDA tool registry: wires IDA-specific tool modules into the shared ToolRegistry."""

from __future__ import annotations

from rikugan.core.host import HAS_HEXRAYS
from rikugan.core.thread_safety import idasync
from rikugan.tools.registry import ToolRegistry

from . import (
    annotations,
    database,
    decompiler,
    disassembly,
    functions,
    microcode,
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
    microcode,
)


def create_default_registry(config=None) -> ToolRegistry:
    """Create a registry with all built-in IDA tools.

    *config* is accepted for a uniform factory signature and ignored: IDA
    requires every API call on the main thread, so all tools are marshalled.
    """
    del config
    registry = ToolRegistry(dispatch_wrapper=idasync)
    registry.set_capabilities({"hexrays": HAS_HEXRAYS})
    for mod in _TOOL_MODULES:
        registry.register_module(mod)
    return registry

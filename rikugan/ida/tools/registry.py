"""IDA tool registry: wires IDA-specific tool modules into the shared ToolRegistry."""

from __future__ import annotations

from rikugan.constants import FILESYSTEM_CATEGORY
from rikugan.core.host import HAS_HEXRAYS
from rikugan.core.thread_safety import idasync
from rikugan.tools import file_search, filesystem, shell
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
    filesystem,
    file_search,
    shell,
)


def _needs_main_thread(defn) -> bool:
    """Whether *defn* touches the IDA API, which is main-thread only.

    File tools do not, and they can take a while: marshalled, a slow one would
    freeze all of IDA, and the registry's timeout could not rescue it.
    """
    return defn.category != FILESYSTEM_CATEGORY


def create_default_registry(config=None) -> ToolRegistry:
    """Create a registry with all built-in IDA tools.

    *config* is accepted for a uniform factory signature and ignored.
    """
    del config
    registry = ToolRegistry(dispatch_wrapper=idasync, marshal_policy=_needs_main_thread)
    registry.set_capabilities({"hexrays": HAS_HEXRAYS})
    for mod in _TOOL_MODULES:
        registry.register_module(mod)
    return registry

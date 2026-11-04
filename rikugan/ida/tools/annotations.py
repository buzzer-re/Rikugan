"""Mutation tools: rename, comment, set type."""

from __future__ import annotations

import importlib
from typing import Annotated

from ...core.errors import ToolError
from ...core.host import HAS_HEXRAYS as _HAS_HEXRAYS
from ...core.logging import log_debug
from ...tools.base import parse_addr, tool

ida_funcs = ida_hexrays = ida_name = idc = None
try:
    ida_funcs = importlib.import_module("ida_funcs")
    ida_hexrays = importlib.import_module("ida_hexrays")
    ida_name = importlib.import_module("ida_name")
    idc = importlib.import_module("idc")
except ImportError:
    log_debug("IDA annotation modules not available — annotation tools will use guard checks")


@tool(category="annotations", mutating=True)
def rename_function(
    address: Annotated[str, "Function address (hex string)"],
    new_name: Annotated[str, "New function name"],
) -> str:
    """Rename a function."""

    ea = parse_addr(address)
    func = ida_funcs.get_func(ea)
    if func is None:
        return f"No function at 0x{ea:x}"

    old_name = ida_name.get_name(func.start_ea)
    ok = ida_name.set_name(func.start_ea, new_name, ida_name.SN_NOWARN | ida_name.SN_NOCHECK)
    if ok:
        return f"Renamed 0x{func.start_ea:x}: {old_name} \u2192 {new_name}"
    return f"Failed to rename function at 0x{func.start_ea:x}"


@tool(category="annotations", mutating=True)
def rename_variable(
    func_address: Annotated[str, "Function address (hex string)"],
    old_name: Annotated[str, "Current variable name"],
    new_name: Annotated[str, "New variable name"],
) -> str:
    """Rename a local variable in a decompiled function."""
    if not _HAS_HEXRAYS:
        return "Hex-Rays decompiler not available"

    ea = parse_addr(func_address)
    try:
        cfunc = ida_hexrays.decompile(ea)
    except Exception as e:
        return f"Decompilation failed: {e}"

    if cfunc is None:
        return f"No decompilation for 0x{ea:x}"

    lvars = cfunc.get_lvars()
    for lv in lvars:
        if lv.name == old_name:
            ok = ida_hexrays.rename_lvar(cfunc.entry_ea, lv.name, new_name)
            if ok:
                return f"Renamed variable: {old_name} \u2192 {new_name}"
            return f"Failed to rename variable {old_name}"

    return f"Variable '{old_name}' not found in function at 0x{ea:x}"


@tool(category="annotations", mutating=True)
def set_comment(
    address: Annotated[str, "Address (hex string)"],
    comment: Annotated[str, "Comment text"],
    repeatable: Annotated[bool, "Set as repeatable comment"] = False,
) -> str:
    """Set a comment at the given address."""

    ea = parse_addr(address)
    ok = idc.set_cmt(ea, comment, repeatable)
    if ok:
        kind = "repeatable " if repeatable else ""
        return f"Set {kind}comment at 0x{ea:x}: {comment}"
    return f"Failed to set comment at 0x{ea:x}"


@tool(category="annotations", mutating=True)
def set_function_comment(
    address: Annotated[str, "Function address (hex string)"],
    comment: Annotated[str, "Comment text"],
    repeatable: Annotated[bool, "Set as repeatable comment"] = False,
) -> str:
    """Set a function-level comment."""

    ea = parse_addr(address)
    func = ida_funcs.get_func(ea)
    if func is None:
        return f"No function at 0x{ea:x}"

    ok = ida_funcs.set_func_cmt(func, comment, repeatable)
    if ok:
        return f"Set function comment at 0x{func.start_ea:x}"
    return f"Failed to set function comment at 0x{func.start_ea:x}"


@tool(category="annotations", mutating=True)
def rename_address(
    address: Annotated[str, "Address (hex string)"],
    new_name: Annotated[str, "New name/label"],
) -> str:
    """Set or change the name/label at an address."""

    ea = parse_addr(address)
    old = ida_name.get_name(ea)
    ok = ida_name.set_name(ea, new_name, ida_name.SN_NOWARN | ida_name.SN_NOCHECK)
    if ok:
        return f"Named 0x{ea:x}: {old or '(unnamed)'} \u2192 {new_name}"
    return f"Failed to set name at 0x{ea:x}"


@tool(category="annotations", mutating=True)
def set_type(
    address: Annotated[str, "Address (hex string)"],
    type_string: Annotated[str, "C type string (e.g. 'int __fastcall(void*, int)')"],
) -> str:
    """Set the type of a function or data item."""

    ea = parse_addr(address)
    ok = idc.SetType(ea, type_string)
    if ok:
        return f"Set type at 0x{ea:x}: {type_string}"
    return f"Failed to set type at 0x{ea:x}. Check syntax: {type_string}"


# Undo's readers. Internal, so they add nothing to what the model is sent on each
# request, and raw, because /undo passes the value straight back to the setter.


@tool(category="annotations", internal=True)
def read_function_name(address: Annotated[str, "Address in the function (hex string)"]) -> str:
    """Read the name rename_function would replace."""
    ea = parse_addr(address)
    func = ida_funcs.get_func(ea)
    if func is None:
        raise ToolError(f"No function at 0x{ea:x}", tool_name="read_function_name")
    return ida_name.get_name(func.start_ea) or ""


@tool(category="annotations", internal=True)
def read_comment(
    address: Annotated[str, "Address (hex string)"],
    repeatable: Annotated[bool, "Read the repeatable comment"] = False,
) -> str:
    """Read the comment set_comment would replace."""
    return idc.get_cmt(parse_addr(address), repeatable) or ""


@tool(category="annotations", internal=True)
def read_function_comment(
    address: Annotated[str, "Function address (hex string)"],
    repeatable: Annotated[bool, "Read the repeatable comment"] = False,
) -> str:
    """Read the comment set_function_comment would replace."""
    ea = parse_addr(address)
    func = ida_funcs.get_func(ea)
    if func is None:
        raise ToolError(f"No function at 0x{ea:x}", tool_name="read_function_comment")
    return ida_funcs.get_func_cmt(func, repeatable) or ""


@tool(category="annotations", internal=True)
def read_function_prototype(address: Annotated[str, "Function address (hex string)"]) -> str:
    """Read the prototype set_function_prototype would replace. Empty if it has none."""
    return idc.get_type(parse_addr(address)) or ""

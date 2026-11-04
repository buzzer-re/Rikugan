"""Mutation tools for Binary Ninja: rename, comment, set type."""

from __future__ import annotations

from typing import Annotated

from ...core.errors import ToolError
from ...core.logging import log_debug
from ...tools.base import tool
from .comment_utils import get_comment_at, set_comment_at
from .compat import parse_addr_like, py_signature_accepts, require_bv
from .fn_utils import get_function_at, get_function_name
from .sym_utils import rename_symbol_at
from .type_utils import parse_type_string


def _set_function_type(func, t) -> bool:
    for name in ("set_user_type", "setUserType"):
        meth = getattr(func, name, None)
        if callable(meth):
            try:
                meth(t)
                return True
            except Exception as e:
                log_debug(f"_set_function_type {name} failed: {e}")
                continue
    try:
        func.type = t
        return True
    except Exception as e:
        log_debug(f"_set_function_type setattr failed: {e}")
        return False


@tool(category="annotations", mutating=True)
def rename_function(
    address: Annotated[str, "Function address (hex string)"],
    new_name: Annotated[str, "New function name"],
) -> str:
    """Rename a function."""
    bv = require_bv()
    ea = parse_addr_like(address)
    func = get_function_at(bv, ea)
    if func is None:
        return f"No function at 0x{ea:x}"

    start = int(getattr(func, "start", ea))
    old_name = get_function_name(func)
    if rename_symbol_at(bv, start, new_name):
        return f"Renamed 0x{start:x}: {old_name} \u2192 {new_name}"

    # Fallback on direct property if available.
    try:
        func.name = new_name
        return f"Renamed 0x{start:x}: {old_name} \u2192 {new_name}"
    except Exception:
        return f"Failed to rename function at 0x{start:x}"


@tool(category="annotations", mutating=True)
def rename_variable(
    func_address: Annotated[str, "Function address (hex string)"],
    old_name: Annotated[str, "Current variable name"],
    new_name: Annotated[str, "New variable name"],
) -> str:
    """Rename a local variable in a function."""
    bv = require_bv()
    ea = parse_addr_like(func_address)
    func = get_function_at(bv, ea)
    if func is None:
        return f"No function at 0x{ea:x}"

    vars_obj = getattr(func, "vars", None)
    if vars_obj is None:
        hlil = getattr(func, "hlil", None)
        vars_obj = getattr(hlil, "vars", None) if hlil is not None else None
    if vars_obj is None:
        return "Variable renaming not available for this function"

    target = None
    for v in list(vars_obj):
        if getattr(v, "name", None) == old_name:
            target = v
            break
    if target is None:
        return f"Variable '{old_name}' not found in function at 0x{int(getattr(func, 'start', ea)):x}"

    for meth_name in ("set_user_var_name", "setUserVarName"):
        meth = getattr(func, meth_name, None)
        if callable(meth):
            try:
                meth(target, new_name)
                return f"Renamed variable: {old_name} \u2192 {new_name}"
            except Exception as e:
                log_debug(f"rename_variable {meth_name} failed: {e}")
                continue

    # Older APIs sometimes only expose create_user_var(var, type, name).
    for meth_name in ("create_user_var", "createUserVar"):
        meth = getattr(func, meth_name, None)
        if not callable(meth):
            continue
        t = getattr(target, "type", None)
        if callable(t):
            try:
                t = t()
            except Exception as e:
                log_debug(f"rename_variable type() call failed: {e}")
                t = None
        try:
            if py_signature_accepts(meth, 3):
                meth(target, t, new_name)
                return f"Renamed variable: {old_name} \u2192 {new_name}"
        except Exception as e:
            log_debug(f"rename_variable {meth_name} positional call failed: {e}")
            continue

    return "Variable renaming is not supported by this Binary Ninja API version"


@tool(category="annotations", mutating=True)
def set_comment(
    address: Annotated[str, "Address (hex string)"],
    comment: Annotated[str, "Comment text"],
    repeatable: Annotated[bool, "Set as repeatable comment"] = False,
) -> str:
    """Set a comment at the given address."""
    bv = require_bv()
    ea = parse_addr_like(address)
    # Binary Ninja comments are effectively repeatable at address granularity.
    _ = repeatable
    ok = set_comment_at(bv, ea, comment)
    if ok:
        return f"Set comment at 0x{ea:x}: {comment}"
    return f"Failed to set comment at 0x{ea:x}"


@tool(category="annotations", mutating=True)
def set_function_comment(
    address: Annotated[str, "Function address (hex string)"],
    comment: Annotated[str, "Comment text"],
    repeatable: Annotated[bool, "Set as repeatable comment"] = False,
) -> str:
    """Set a function-level comment."""
    bv = require_bv()
    ea = parse_addr_like(address)
    func = get_function_at(bv, ea)
    if func is None:
        return f"No function at 0x{ea:x}"
    _ = repeatable

    for name in ("set_comment", "setComment"):
        meth = getattr(func, name, None)
        if callable(meth):
            try:
                meth(comment)
                return f"Set function comment at 0x{int(getattr(func, 'start', ea)):x}"
            except Exception as e:
                log_debug(f"set_function_comment {name} failed: {e}")
                continue

    try:
        func.comment = comment
        return f"Set function comment at 0x{int(getattr(func, 'start', ea)):x}"
    except Exception as e:
        log_debug(f"set_function_comment setattr failed: {e}")
        return f"Failed to set function comment at 0x{int(getattr(func, 'start', ea)):x}"


@tool(category="annotations", mutating=True)
def rename_address(
    address: Annotated[str, "Address (hex string)"],
    new_name: Annotated[str, "New name/label"],
) -> str:
    """Set or change the name/label at an address."""
    bv = require_bv()
    ea = parse_addr_like(address)
    old = ""
    sym = getattr(bv, "get_symbol_at", lambda _ea: None)(ea) if hasattr(bv, "get_symbol_at") else None
    if sym is not None:
        old = getattr(sym, "full_name", None) or getattr(sym, "name", None) or ""
    ok = rename_symbol_at(bv, ea, new_name)
    if ok:
        return f"Named 0x{ea:x}: {old or '(unnamed)'} \u2192 {new_name}"
    return f"Failed to set name at 0x{ea:x}"


@tool(category="annotations", mutating=True)
def set_type(
    address: Annotated[str, "Address (hex string)"],
    type_string: Annotated[str, "C type string (e.g. 'int __fastcall(void*, int)')"],
) -> str:
    """Set the type of a function or data item."""
    bv = require_bv()
    ea = parse_addr_like(address)
    try:
        t, _parsed_name = parse_type_string(bv, type_string)
    except Exception as e:
        return f"Failed to parse type: {e}"

    func = get_function_at(bv, ea)
    if func is not None and int(getattr(func, "start", ea)) == ea:
        if _set_function_type(func, t):
            return f"Set type at 0x{ea:x}: {type_string}"

    for meth_name in ("define_user_data_var", "defineUserDataVar"):
        meth = getattr(bv, meth_name, None)
        if callable(meth):
            try:
                meth(ea, t)
                return f"Set type at 0x{ea:x}: {type_string}"
            except Exception as e:
                log_debug(f"set_type {meth_name} failed at 0x{ea:x}: {e}")
                continue

    return f"Failed to set type at 0x{ea:x}. Check syntax: {type_string}"


# Undo's readers. Internal, so they add nothing to what the model is sent on each
# request, and raw, because /undo passes the value straight back to the setter.


def _function_for_undo(address: str, tool_name: str):
    bv = require_bv()
    ea = parse_addr_like(address)
    func = get_function_at(bv, ea)
    if func is None:
        raise ToolError(f"No function at 0x{ea:x}", tool_name=tool_name)
    return func


@tool(category="annotations", internal=True)
def read_function_name(address: Annotated[str, "Address in the function (hex string)"]) -> str:
    """Read the name rename_function would replace."""
    return get_function_name(_function_for_undo(address, "read_function_name"))


@tool(category="annotations", internal=True)
def read_comment(
    address: Annotated[str, "Address (hex string)"],
    repeatable: Annotated[bool, "Ignored: Binary Ninja has one comment per address"] = False,
) -> str:
    """Read the comment set_comment would replace."""
    _ = repeatable
    return get_comment_at(require_bv(), parse_addr_like(address))


@tool(category="annotations", internal=True)
def read_function_comment(
    address: Annotated[str, "Function address (hex string)"],
    repeatable: Annotated[bool, "Ignored: Binary Ninja has one comment per function"] = False,
) -> str:
    """Read the comment set_function_comment would replace."""
    _ = repeatable
    return str(getattr(_function_for_undo(address, "read_function_comment"), "comment", "") or "")


@tool(category="annotations", internal=True)
def read_function_prototype(address: Annotated[str, "Function address (hex string)"]) -> str:
    """Read the prototype set_function_prototype would replace. Empty if it has none."""
    func = _function_for_undo(address, "read_function_prototype")
    function_type = getattr(func, "type", None) or getattr(func, "function_type", None)
    return str(function_type) if function_type is not None else ""

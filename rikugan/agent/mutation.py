"""Mutation tracking for reversible tool calls."""

from __future__ import annotations

from typing import Any

from ..core.logging import log_debug
from ..core.types import MutationRecord

# Re-exported: the record itself lives in core.types so SessionState can hold
# one without importing the agent package.
__all__ = ["MutationRecord", "build_reverse_record", "capture_pre_state"]


# ---------------------------------------------------------------------------
# Per-tool reverse-record builders
# ---------------------------------------------------------------------------


def _reverse_rename_function(args: dict[str, Any], pre: dict[str, Any]) -> MutationRecord:
    old_name = args.get("old_name", "")
    new_name = args.get("new_name", "")
    return MutationRecord(
        tool_name="rename_function",
        arguments=args,
        reverse_tool="rename_function",
        reverse_arguments={"old_name": new_name, "new_name": old_name},
        description=f"Rename function {old_name} → {new_name}",
    )


def _reverse_rename_variable(tool_name: str, args: dict[str, Any], pre: dict[str, Any]) -> MutationRecord:
    func = args.get("function_name", "")
    old_var = args.get("variable_name", "")
    new_var = args.get("new_name", "")
    return MutationRecord(
        tool_name=tool_name,
        arguments=args,
        reverse_tool=tool_name,
        reverse_arguments={
            "function_name": func,
            "variable_name": new_var,
            "new_name": old_var,
        },
        description=f"Rename variable {old_var} → {new_var} in {func}",
    )


def _reverse_comment(
    tool_name: str,
    delete_tool: str,
    key: str,
    args: dict[str, Any],
    pre: dict[str, Any],
) -> MutationRecord:
    """Build reverse record for comment-setting tools."""
    target = args.get(key, "")
    old_comment = pre.get("old_comment", "")
    if old_comment:
        return MutationRecord(
            tool_name=tool_name,
            arguments=args,
            reverse_tool=tool_name,
            reverse_arguments={key: target, "comment": old_comment},
            description=f"Set comment on {target}",
        )
    return MutationRecord(
        tool_name=tool_name,
        arguments=args,
        reverse_tool=delete_tool,
        reverse_arguments={key: target},
        description=f"Set comment on {target}",
    )


def _reverse_set_comment(args: dict[str, Any], pre: dict[str, Any]) -> MutationRecord:
    return _reverse_comment("set_comment", "delete_comment", "address", args, pre)


def _reverse_set_function_comment(args: dict[str, Any], pre: dict[str, Any]) -> MutationRecord:
    return _reverse_comment("set_function_comment", "delete_function_comment", "function_name", args, pre)


def _reverse_set_pseudocode_comment(args: dict[str, Any], pre: dict[str, Any]) -> MutationRecord:
    func_addr = args.get("func_address", "")
    target_addr = args.get("target_address", "")
    old_comment = pre.get("old_comment", "")
    reverse_args = {
        "func_address": func_addr,
        "target_address": target_addr,
        "comment": old_comment or "",
    }
    return MutationRecord(
        tool_name="set_pseudocode_comment",
        arguments=args,
        reverse_tool="set_pseudocode_comment",
        reverse_arguments=reverse_args,
        description=f"Set pseudocode comment at {target_addr}",
    )


def _reverse_rename_data(args: dict[str, Any], pre: dict[str, Any]) -> MutationRecord:
    address = args.get("address", "")
    old_name = pre.get("old_name", "")
    new_name = args.get("new_name", "")
    if old_name:
        return MutationRecord(
            tool_name="rename_data",
            arguments=args,
            reverse_tool="rename_data",
            reverse_arguments={"address": address, "new_name": old_name},
            description=f"Rename data at {address} → {new_name}",
        )
    return MutationRecord(
        tool_name="rename_data",
        arguments=args,
        reverse_tool="",
        reverse_arguments={},
        description=f"Rename data at {address} → {new_name}",
        reversible=False,
    )


def _reverse_set_function_prototype(args: dict[str, Any], pre: dict[str, Any]) -> MutationRecord | None:
    target = args.get("name_or_address", "")
    old_proto = pre.get("old_prototype", "")
    if old_proto:
        return MutationRecord(
            tool_name="set_function_prototype",
            arguments=args,
            reverse_tool="set_function_prototype",
            reverse_arguments={"name_or_address": target, "prototype": old_proto},
            description=f"Set prototype for {target}",
        )
    return None


def _reverse_retype_variable(args: dict[str, Any], pre: dict[str, Any]) -> MutationRecord | None:
    func = args.get("function_name", "")
    var = args.get("variable_name", "")
    old_type = pre.get("old_type", "")
    if old_type:
        return MutationRecord(
            tool_name="retype_variable",
            arguments=args,
            reverse_tool="retype_variable",
            reverse_arguments={
                "function_name": func,
                "variable_name": var,
                "type_str": old_type,
            },
            description=f"Retype {var} in {func}",
        )
    return None


# Dispatch table: tool_name → handler(args, pre) -> Optional[MutationRecord]
_REVERSE_BUILDERS: dict[str, Any] = {
    "rename_function": _reverse_rename_function,
    "rename_variable": lambda a, p: _reverse_rename_variable("rename_variable", a, p),
    "rename_single_variable": lambda a, p: _reverse_rename_variable("rename_single_variable", a, p),
    "set_comment": _reverse_set_comment,
    "set_function_comment": _reverse_set_function_comment,
    "set_pseudocode_comment": _reverse_set_pseudocode_comment,
    "rename_data": _reverse_rename_data,
    "set_function_prototype": _reverse_set_function_prototype,
    "retype_variable": _reverse_retype_variable,
}


def build_reverse_record(
    tool_name: str,
    arguments: dict[str, Any],
    pre_state: dict[str, Any] | None = None,
) -> MutationRecord | None:
    """Build a MutationRecord with reverse operation for a mutating tool call.

    Returns None if the tool is not reversible.
    """
    pre = pre_state or {}
    builder = _REVERSE_BUILDERS.get(tool_name)
    if builder is not None:
        result = builder(arguments, pre)
        if result is not None:
            return result

    # For tools we don't know how to reverse (execute_python, etc.)
    return MutationRecord(
        tool_name=tool_name,
        arguments=arguments,
        reverse_tool="",
        reverse_arguments={},
        description=f"Call {tool_name}",
        reversible=False,
    )


def capture_pre_state(
    tool_name: str,
    arguments: dict[str, Any],
    tool_executor,
) -> dict[str, Any]:
    """Capture pre-mutation state needed for undo.

    Calls getter tools where needed to record the current state
    before a mutation is applied.
    """
    pre: dict[str, Any] = {}

    try:
        if tool_name == "set_comment":
            address = arguments.get("address", "")
            if address:
                pre["old_comment"] = tool_executor("get_comment", {"address": address})
        elif tool_name == "set_function_comment":
            func = arguments.get("function_name", "")
            if func:
                pre["old_comment"] = tool_executor("get_function_comment", {"function_name": func})
        elif tool_name == "set_pseudocode_comment":
            func_addr = arguments.get("func_address", "")
            target_addr = arguments.get("target_address", "")
            if func_addr and target_addr:
                pre["old_comment"] = tool_executor(
                    "get_pseudocode_comment",
                    {"func_address": func_addr, "target_address": target_addr},
                )
        elif tool_name == "set_function_prototype":
            target = arguments.get("name_or_address", "")
            if target:
                pre["old_prototype"] = tool_executor("get_function_prototype", {"name_or_address": target})
        elif tool_name == "retype_variable":
            func = arguments.get("function_name", "")
            var = arguments.get("variable_name", "")
            if func and var:
                pre["old_type"] = tool_executor("get_variable_type", {"function_name": func, "variable_name": var})
    except Exception as e:
        log_debug(f"capture_pre_state failed for {tool_name}: {e}")

    return pre

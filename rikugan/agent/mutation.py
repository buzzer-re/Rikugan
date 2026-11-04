"""Mutation tracking for reversible tool calls."""

from __future__ import annotations

from typing import Any

from ..core.errors import ToolError
from ..core.logging import log_debug
from ..core.types import MutationRecord

# Re-exported: the record itself lives in core.types so SessionState can hold
# one without importing the agent package.
__all__ = ["MutationRecord", "build_reverse_record", "capture_pre_state", "mutation_succeeded"]


# ---------------------------------------------------------------------------
# Per-tool reverse-record builders
# ---------------------------------------------------------------------------


def _reverse_rename_function(args: dict[str, Any], pre: dict[str, Any]) -> MutationRecord | None:
    old_name = pre.get("old_name", "")
    new_name = args.get("new_name", "")
    if not old_name:
        return None
    return MutationRecord(
        tool_name="rename_function",
        arguments=args,
        reverse_tool="rename_function",
        reverse_arguments={"address": args.get("address", ""), "new_name": old_name},
        description=f"Rename function {old_name} → {new_name}",
    )


def _reverse_rename_variable(args: dict[str, Any], pre: dict[str, Any]) -> MutationRecord:
    func = args.get("func_address", "")
    old_var = args.get("old_name", "")
    new_var = args.get("new_name", "")
    return MutationRecord(
        tool_name="rename_variable",
        arguments=args,
        reverse_tool="rename_variable",
        reverse_arguments={"func_address": func, "old_name": new_var, "new_name": old_var},
        description=f"Rename variable {old_var} → {new_var} in {func}",
    )


def _reverse_comment(
    tool_name: str, description: str, args: dict[str, Any], pre: dict[str, Any]
) -> MutationRecord | None:
    """Put the old comment back. Writing an empty one is how both hosts remove it."""
    if "old_comment" not in pre:
        return None
    address = args.get("address", "")
    return MutationRecord(
        tool_name=tool_name,
        arguments=args,
        reverse_tool=tool_name,
        reverse_arguments={
            "address": address,
            "comment": pre["old_comment"],
            "repeatable": bool(args.get("repeatable", False)),
        },
        description=f"{description} {address}",
    )


def _reverse_set_comment(args: dict[str, Any], pre: dict[str, Any]) -> MutationRecord | None:
    return _reverse_comment("set_comment", "Set comment at", args, pre)


def _reverse_set_function_comment(args: dict[str, Any], pre: dict[str, Any]) -> MutationRecord | None:
    return _reverse_comment("set_function_comment", "Set function comment at", args, pre)


def _reverse_set_pseudocode_comment(args: dict[str, Any], pre: dict[str, Any]) -> MutationRecord | None:
    if "old_comment" not in pre:
        return None
    func_addr = args.get("func_address", "")
    target_addr = args.get("target_address", "")
    return MutationRecord(
        tool_name="set_pseudocode_comment",
        arguments=args,
        reverse_tool="set_pseudocode_comment",
        reverse_arguments={"func_address": func_addr, "target_address": target_addr, "comment": pre["old_comment"]},
        description=f"Set pseudocode comment at {target_addr}",
    )


def _reverse_rename_address(args: dict[str, Any], pre: dict[str, Any]) -> MutationRecord | None:
    address = args.get("address", "")
    old_name = pre.get("old_name", "")
    if not old_name:
        return None
    return MutationRecord(
        tool_name="rename_address",
        arguments=args,
        reverse_tool="rename_address",
        reverse_arguments={"address": address, "new_name": old_name},
        description=f"Rename {address} → {args.get('new_name', '')}",
    )


def _reverse_set_function_prototype(args: dict[str, Any], pre: dict[str, Any]) -> MutationRecord | None:
    address = args.get("address", "")
    old_prototype = pre.get("old_prototype", "")
    if not old_prototype:
        return None
    return MutationRecord(
        tool_name="set_function_prototype",
        arguments=args,
        reverse_tool="set_function_prototype",
        reverse_arguments={"address": address, "prototype": old_prototype},
        description=f"Set prototype at {address}",
    )


# Dispatch table: tool_name → handler(args, pre) -> Optional[MutationRecord]
# Mutating tools with no reverse, named deliberately rather than by omission.
# The contract test fails for any mutating tool that is in neither this set nor
# _REVERSE_BUILDERS, so adding one forces the author to decide.
#
# set_type, propagate_type and apply_type_to_variable need a type-at-address
# reader, which neither host has. Writing one would make them undoable; until then
# the log says so rather than implying an undo that silently no-ops.
_KNOWN_IRREVERSIBLE: frozenset[str] = frozenset(
    {
        "execute_python",  # arbitrary code; nothing to invert
        "set_type",
        "propagate_type",
        "apply_type_to_variable",
        "apply_struct_to_address",
        "create_struct",
        "create_enum",
        "create_typedef",
        "modify_struct",
        "modify_enum",
        "import_c_header",
        "import_type_from_library",
        "write_bytes",
        "nop_instructions",
        "nop_microcode",
        "patch_branch",
        "il_replace_expr",
        "il_set_condition",
        "il_nop_expr",
        "il_remove_block",
        "install_il_workflow",
        "install_microcode_optimizer",
        "remove_microcode_optimizer",
        "redecompile_function",
    }
)

_REVERSE_BUILDERS: dict[str, Any] = {
    "rename_function": _reverse_rename_function,
    "rename_variable": _reverse_rename_variable,
    "set_comment": _reverse_set_comment,
    "set_function_comment": _reverse_set_function_comment,
    "set_pseudocode_comment": _reverse_set_pseudocode_comment,
    "rename_address": _reverse_rename_address,
    "set_function_prototype": _reverse_set_function_prototype,
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

    # Either the tool has no reverse at all, or the state needed to build one
    # could not be read. Say which, on the record, so the mutation row can show
    # it at the time rather than leaving the user to find out at undo time.
    if tool_name in _KNOWN_IRREVERSIBLE:
        reason = "this tool has no reverse operation"
    else:
        reason = pre.get("_capture_failed") or "the previous value could not be read"
    return MutationRecord(
        tool_name=tool_name,
        arguments=arguments,
        reverse_tool="",
        reverse_arguments={},
        description=f"Call {tool_name}",
        reversible=False,
        irreversible_reason=reason,
    )


# How each reversible tool opens its message when it worked. These tools report
# failure by returning text rather than raising, so success has to be recognised:
# anything else counts as failed, which keeps a change in the log rather than
# claiming an undo that did not happen.
_SUCCESS_PREFIXES = ("Renamed", "Named", "Set ")


def mutation_succeeded(tool_name: str, result: Any) -> bool:
    """Whether a reversible tool really made its change. Others are taken at their word."""
    if tool_name not in _REVERSE_BUILDERS:
        return True
    return str(result).startswith(_SUCCESS_PREFIXES)


def _read_address_name(address: str, tool_executor) -> str:
    name = tool_executor("get_name_at", {"address": address})
    # Its message for "none". Kept as the old name, undo would rename to this text.
    if str(name).startswith("No name at"):
        raise ToolError("the address had no name to restore")
    return name


def _read_pseudocode_comment(args: dict[str, Any], tool_executor) -> str:
    comment = tool_executor(
        "get_pseudocode_comment",
        {"func_address": args.get("func_address", ""), "target_address": args.get("target_address", "")},
    )
    # A failed decompile comes back as text. Kept, undo would write it as a comment.
    if str(comment).startswith("Decompilation "):
        raise ToolError(str(comment))
    return comment


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
    address = arguments.get("address", "")
    repeatable = bool(arguments.get("repeatable", False))

    try:
        if tool_name == "rename_function":
            pre["old_name"] = tool_executor("read_function_name", {"address": address})
        elif tool_name == "set_comment":
            pre["old_comment"] = tool_executor("read_comment", {"address": address, "repeatable": repeatable})
        elif tool_name == "set_function_comment":
            pre["old_comment"] = tool_executor("read_function_comment", {"address": address, "repeatable": repeatable})
        elif tool_name == "set_pseudocode_comment":
            pre["old_comment"] = _read_pseudocode_comment(arguments, tool_executor)
        elif tool_name == "set_function_prototype":
            pre["old_prototype"] = tool_executor("read_function_prototype", {"address": address})
            if not pre["old_prototype"]:
                pre["_capture_failed"] = "the function had no prototype to restore"
        elif tool_name == "rename_address":
            pre["old_name"] = _read_address_name(address, tool_executor)
    except Exception as e:
        # The reverse builder turns this into a non-reversible record carrying
        # the reason, so a missing getter shows up on the mutation row instead
        # of silently producing an undo that does nothing.
        log_debug(f"capture_pre_state failed for {tool_name}: {e}")
        pre["_capture_failed"] = str(e)

    return pre

"""Prompt-generating command handlers for Binary Ninja UI actions."""

from __future__ import annotations

from typing import Any, Callable, Dict, Tuple


def build_context(
    bv: Any,
    ea: int,
    get_function_at: Callable[[Any, int], Any],
    get_function_name: Callable[[Any], str],
) -> Dict[str, Any]:
    ctx: Dict[str, Any] = {
        "ea": int(ea),
        "func_ea": None,
        "func_name": None,
        "selected_text": "",
    }
    func = get_function_at(bv, ea)
    if func is not None:
        ctx["func_ea"] = int(getattr(func, "start", ea))
        ctx["func_name"] = get_function_name(func)
    return ctx


def handle_send_to(ctx: Dict[str, Any]) -> str:
    sel = ctx["selected_text"]
    if sel:
        return sel
    name = ctx["func_name"]
    ea = ctx["ea"]
    if name:
        return f"Analyze the function {name} at 0x{ea:x}"
    return f"Analyze the code at 0x{ea:x}"


def handle_explain(ctx: Dict[str, Any]) -> str:
    name = ctx["func_name"] or f"sub_{ctx['ea']:x}"
    ea = ctx["func_ea"] or ctx["ea"]
    return (
        f"Explain the function {name} at 0x{ea:x}. "
        "Decompile it and provide a detailed analysis."
    )


def handle_rename(ctx: Dict[str, Any]) -> str:
    name = ctx["func_name"] or f"sub_{ctx['ea']:x}"
    ea = ctx["func_ea"] or ctx["ea"]
    return (
        f"Analyze the function {name} at 0x{ea:x}. "
        "Based on its behavior, suggest better names for the function "
        "and its local variables. Apply the renames."
    )


def handle_deobfuscate(ctx: Dict[str, Any]) -> str:
    name = ctx["func_name"] or f"sub_{ctx['ea']:x}"
    ea = ctx["func_ea"] or ctx["ea"]
    return (
        f"Deobfuscate the function {name} at 0x{ea:x}. "
        "Identify obfuscation patterns (opaque predicates, junk code, "
        "control-flow flattening, encrypted strings) and explain them. "
        "If possible, apply IL optimizations to clean the output."
    )


def handle_vuln_audit(ctx: Dict[str, Any]) -> str:
    name = ctx["func_name"] or f"sub_{ctx['ea']:x}"
    ea = ctx["func_ea"] or ctx["ea"]
    return (
        f"Audit the function {name} at 0x{ea:x} for security vulnerabilities. "
        "Check for buffer overflows, format strings, integer overflows, "
        "use-after-free, command injection, and other memory-safety issues. "
        "List each finding with severity and evidence."
    )


def handle_suggest_types(ctx: Dict[str, Any]) -> str:
    name = ctx["func_name"] or f"sub_{ctx['ea']:x}"
    ea = ctx["func_ea"] or ctx["ea"]
    return (
        f"Analyze the function {name} at 0x{ea:x} and infer types. "
        "Examine pointer dereference patterns to suggest structs, "
        "identify enum-like constants, and propose proper parameter types. "
        "Apply the type changes."
    )


def handle_annotate(ctx: Dict[str, Any]) -> str:
    name = ctx["func_name"] or f"sub_{ctx['ea']:x}"
    ea = ctx["func_ea"] or ctx["ea"]
    return (
        f"Annotate the function {name} at 0x{ea:x} with comments. "
        "Add a function-level comment summarizing its purpose, and "
        "add inline comments to key basic blocks explaining the logic."
    )


def handle_clean_mcode(ctx: Dict[str, Any]) -> str:
    name = ctx["func_name"] or f"sub_{ctx['ea']:x}"
    ea = ctx["func_ea"] or ctx["ea"]
    return (
        f"Clean the IL for {name} at 0x{ea:x}. "
        "Read the IL, identify junk or obfuscated instructions, "
        "NOP or patch them if needed, then redecompile to verify."
    )


def handle_xref_analysis(ctx: Dict[str, Any]) -> str:
    name = ctx["func_name"] or f"sub_{ctx['ea']:x}"
    ea = ctx["func_ea"] or ctx["ea"]
    return (
        f"Perform a deep cross-reference analysis on {name} at 0x{ea:x}. "
        "Trace all callers and callees, identify data references, "
        "and map out the call graph around this function."
    )


def handle_smart_patch(ctx: Dict[str, Any]) -> str:
    name = ctx["func_name"] or f"sub_{ctx['ea']:x}"
    ea = ctx["func_ea"] or ctx["ea"]
    return (
        f"/smart-patch Patch function {name} at 0x{ea:x}.\n"
        f"Desired behavior: "
    )


ACTION_DEFS: Tuple[Tuple[str, str, Callable[[Dict[str, Any]], str], bool], ...] = (
    ("Send to Rikugan", "Send selection or address to Rikugan input", handle_send_to, False),
    ("Explain this", "Explain the current function with Rikugan", handle_explain, True),
    ("Rename with Rikugan", "Analyze and rename the current function", handle_rename, True),
    ("Deobfuscate with Rikugan", "Deobfuscate the current function", handle_deobfuscate, True),
    ("Find vulnerabilities", "Audit the current function for security bugs", handle_vuln_audit, True),
    ("Suggest types", "Infer and apply types for the current function", handle_suggest_types, True),
    ("Annotate function", "Add comments to the current function", handle_annotate, True),
    ("Clean IL", "Clean IR IL for the current function", handle_clean_mcode, True),
    ("Xref analysis", "Deep cross-reference analysis on the current function", handle_xref_analysis, True),
    ("Smart Patch", "Modify function behavior with natural language and apply binary patches", handle_smart_patch, False),
)

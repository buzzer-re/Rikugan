"""Tool framework: @tool decorator, ToolDefinition, JSON schema generation."""

from __future__ import annotations

import functools
import inspect
import json
import typing
from dataclasses import dataclass, field
from typing import Any, Callable, Dict, List, Optional, get_type_hints

import traceback

from ..core.errors import ToolError, ToolValidationError
from ..core.logging import log_error as _log_error, log_trace
from ..core.thread_safety import idasync


def parse_addr(value: Any) -> int:
    """Parse an address that may arrive as hex string or int from the LLM."""
    if isinstance(value, int):
        return value
    return int(value, 0)


# Python type -> JSON Schema type
_TYPE_MAP = {
    str: "string",
    int: "integer",
    float: "number",
    bool: "boolean",
    list: "array",
    dict: "object",
}


@dataclass
class ParameterSchema:
    name: str
    type: str
    description: str = ""
    required: bool = True
    default: Any = None
    enum: Optional[List[Any]] = None
    items: Optional[Dict[str, Any]] = None


@dataclass
class ToolDefinition:
    name: str
    description: str
    parameters: List[ParameterSchema] = field(default_factory=list)
    category: str = "general"
    requires_decompiler: bool = False
    mutating: bool = False
    timeout: Optional[float] = None  # per-tool timeout in seconds (None = use default)
    handler: Optional[Callable] = field(default=None, repr=False)
    requires: List[str] = field(default_factory=list)

    def to_json_schema(self) -> Dict[str, Any]:
        properties: Dict[str, Any] = {}
        required: List[str] = []

        for param in self.parameters:
            prop: Dict[str, Any] = {"type": param.type}
            if param.description:
                prop["description"] = param.description
            if param.enum:
                prop["enum"] = param.enum
            if param.items:
                prop["items"] = param.items
            properties[param.name] = prop
            if param.required:
                required.append(param.name)

        schema: Dict[str, Any] = {
            "type": "object",
            "properties": properties,
        }
        if required:
            schema["required"] = required
        return schema

    def to_provider_format(self) -> Dict[str, Any]:
        return {
            "type": "function",
            "function": {
                "name": self.name,
                "description": self.description,
                "parameters": self.to_json_schema(),
            },
        }


def _extract_annotation_metadata(annotation: Any) -> Dict[str, Any]:
    """Extract description, enum, etc. from typing.Annotated metadata."""
    metadata: Dict[str, Any] = {}
    if hasattr(annotation, "__metadata__"):
        for m in annotation.__metadata__:
            if isinstance(m, str):
                metadata["description"] = m
            elif isinstance(m, dict):
                metadata.update(m)
    return metadata


def _resolve_type(annotation: Any) -> tuple:
    """Resolve a type annotation to (json_type, extra_schema_props, base_type)."""
    origin = getattr(annotation, "__origin__", None)
    args = getattr(annotation, "__args__", ())

    # Handle Annotated — use typing.get_origin() because on Python 3.14
    # Annotated[int, ...].__origin__ is `int`, not `typing.Annotated`.
    if typing.get_origin(annotation) is typing.Annotated:
        real_args = typing.get_args(annotation)
        base = real_args[0] if real_args else str
        return _resolve_type(base)

    # Handle Optional
    if origin is typing.Union and len(args) == 2 and type(None) in args:
        inner = args[0] if args[1] is type(None) else args[1]
        json_type, extra, _ = _resolve_type(inner)
        return json_type, extra, inner

    # Handle List
    if origin in (list, typing.List if hasattr(typing, "List") else list):
        items = {}
        if args:
            inner_type, _, _ = _resolve_type(args[0])
            items = {"type": inner_type}
        return "array", {"items": items} if items else {}, list

    # Handle Dict
    if origin in (dict, typing.Dict if hasattr(typing, "Dict") else dict):
        return "object", {}, dict

    # Primitives
    json_type = _TYPE_MAP.get(annotation, "string")
    return json_type, {}, annotation


def _build_parameters(func: Callable) -> List[ParameterSchema]:
    """Build parameter schemas from function signature and type hints."""
    sig = inspect.signature(func)
    hints = get_type_hints(func, include_extras=True)
    params: List[ParameterSchema] = []

    for name, param in sig.parameters.items():
        if name in ("self", "cls"):
            continue

        annotation = hints.get(name, str)

        # Extract Annotated metadata — use typing.get_origin() because
        # on Python 3.14 Annotated[X, ...].__origin__ is X, not Annotated.
        meta: Dict[str, Any] = {}
        if typing.get_origin(annotation) is typing.Annotated:
            meta = _extract_annotation_metadata(annotation)
        annotation_for_type = annotation

        json_type, extra, base_type = _resolve_type(annotation_for_type)

        # Determine required and default
        has_default = param.default is not inspect.Parameter.empty
        is_optional = (
            getattr(annotation, "__origin__", None) is typing.Union
            and type(None) in getattr(annotation, "__args__", ())
        )

        ps = ParameterSchema(
            name=name,
            type=json_type,
            description=meta.get("description", ""),
            required=not has_default and not is_optional,
            default=param.default if has_default else None,
            enum=meta.get("enum"),
            items=extra.get("items"),
        )
        params.append(ps)

    return params


def tool(
    name: Optional[str] = None,
    description: Optional[str] = None,
    category: str = "general",
    requires_decompiler: bool = False,
    mutating: bool = False,
    timeout: Optional[float] = None,
    requires: Optional[List[str]] = None,
) -> Callable[[Callable[..., Any]], Callable[..., Any]]:
    """Decorator to register a function as an agent tool.

    Usage::

        @tool(name="decompile_function", category="decompiler", requires_decompiler=True)
        def decompile_function(address: Annotated[int, "Function address"]) -> str:
            '''Decompile the function at the given address.'''
            ...
    """
    def decorator(func: Callable[..., Any]) -> Callable[..., Any]:
        tool_name = name or func.__name__
        tool_desc = description or (func.__doc__ or "").strip().split("\n")[0]
        params = _build_parameters(func)

        # Build requires list — merge explicit requires with requires_decompiler compat
        effective_requires = list(requires or [])
        if requires_decompiler and "hexrays" not in effective_requires:
            effective_requires.append("hexrays")

        defn = ToolDefinition(
            name=tool_name,
            description=tool_desc,
            parameters=params,
            category=category,
            requires_decompiler=requires_decompiler,
            mutating=mutating,
            timeout=timeout,
            handler=func,
            requires=effective_requires,
        )

        # All tools call IDA APIs, which must run on the main thread.
        # Wrap with idasync so background agent threads are marshalled
        # through execute_sync automatically.
        safe_func = idasync(func)

        @functools.wraps(func)
        def wrapper(*args: Any, **kwargs: Any) -> Any:
            log_trace(f"tool:{tool_name} CALL args={kwargs}")
            try:
                result = safe_func(*args, **kwargs)
                log_trace(f"tool:{tool_name} OK result_len={len(str(result))}")
                return result
            except ToolError:
                raise
            except Exception as e:
                _log_error(f"tool:{tool_name} EXCEPTION: {e}\n{traceback.format_exc()}")
                raise ToolError(str(e), tool_name=tool_name) from e

        wrapper._tool_definition = defn  # type: ignore[attr-defined]
        defn.handler = wrapper
        return wrapper

    return decorator

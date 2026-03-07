"""Tool registry: discovers, stores, and dispatches tool calls."""

from __future__ import annotations

import json
from concurrent.futures import ThreadPoolExecutor, TimeoutError as FuturesTimeoutError
from typing import Any, Callable, Dict, List, Optional

from ..core.errors import ToolError, ToolNotFoundError, ToolValidationError
from ..core.logging import log_debug
from ..constants import TOOL_RESULT_TRUNCATE_LEN
from .base import ToolDefinition
from .cache import ToolResultCache

# Default timeout for tool execution (seconds).  Per-tool overrides via ToolDefinition.timeout.
_DEFAULT_TOOL_TIMEOUT = 30.0

# Shared executor — single thread is sufficient since IDA tools run on main thread via idasync
_executor = ThreadPoolExecutor(max_workers=2, thread_name_prefix="tool-timeout")


class ToolRegistry:
    """Central registry of all available tools."""

    def __init__(self) -> None:
        self._tools: Dict[str, ToolDefinition] = {}
        self._schema_cache: Optional[List[Dict[str, Any]]] = None
        self._result_cache = ToolResultCache()
        self._capabilities: Dict[str, bool] = {}

    @staticmethod
    def _coerce_arguments(defn: ToolDefinition, arguments: Dict[str, Any]) -> Dict[str, Any]:
        """Coerce mistyped tool arguments to match the schema.

        LLMs sometimes send integers as strings (e.g. "30" instead of 30).
        Walk the parameter schema and cast values to their declared types.
        """
        if not defn.parameters or not arguments:
            return arguments

        param_types = {p.name: p.type for p in defn.parameters}
        coerced = dict(arguments)

        for key, value in coerced.items():
            expected = param_types.get(key)
            if expected is None:
                continue

            try:
                if expected == "integer":
                    # bool is a subclass of int — check bool FIRST
                    if isinstance(value, bool):
                        coerced[key] = int(value)
                    elif not isinstance(value, int):
                        # Handle "30", "30.0", "0x1e" etc.
                        coerced[key] = int(float(value))
                elif expected == "number" and not isinstance(value, (int, float)):
                    coerced[key] = float(value)
                elif expected == "boolean" and not isinstance(value, bool):
                    # Coerce int 0/1 and string "true"/"false"
                    if isinstance(value, int):
                        coerced[key] = bool(value)
                    else:
                        coerced[key] = str(value).lower() in ("true", "1", "yes")
                elif expected == "string" and not isinstance(value, str):
                    coerced[key] = str(value)
            except (ValueError, TypeError) as e:
                log_debug(f"_coerce_arguments: coercion failed for {key!r}: {e}")  # handler will raise validation error

        return coerced

    def register(self, defn: ToolDefinition) -> None:
        self._tools[defn.name] = defn
        self._schema_cache = None  # invalidate
        log_debug(f"Registered tool: {defn.name}")

    def register_function(self, func: Callable[..., Any]) -> None:
        defn = getattr(func, "_tool_definition", None)
        if defn is None:
            raise ValueError(f"{func.__name__} is not decorated with @tool")
        self.register(defn)

    def register_module(self, module: Any) -> None:
        """Register all @tool-decorated functions in a module."""
        for name in dir(module):
            obj = getattr(module, name)
            if callable(obj) and isinstance(getattr(obj, "_tool_definition", None), ToolDefinition):
                self.register(obj._tool_definition)

    def unregister_by_prefix(self, prefix: str) -> int:
        """Remove all tools whose name starts with *prefix*. Returns count removed."""
        to_remove = [name for name in self._tools if name.startswith(prefix)]
        for name in to_remove:
            del self._tools[name]
        if to_remove:
            self._schema_cache = None
            log_debug(f"Unregistered {len(to_remove)} tools with prefix {prefix!r}")
        return len(to_remove)

    def set_capabilities(self, capabilities: Dict[str, bool]) -> None:
        """Declare which host capabilities are available (e.g. hexrays, ida_struct)."""
        self._capabilities.update(capabilities)
        self._schema_cache = None  # invalidate — available tools may have changed

    def _available(self, defn: ToolDefinition) -> bool:
        """Check if all requirements of a tool definition are met."""
        for req in defn.requires:
            if not self._capabilities.get(req, True):
                return False
        return True

    def get(self, name: str) -> Optional[ToolDefinition]:
        return self._tools.get(name)

    def list_tools(self) -> List[ToolDefinition]:
        return list(self._tools.values())

    def list_names(self) -> List[str]:
        return list(self._tools.keys())

    def to_provider_format(self) -> List[Dict[str, Any]]:
        if self._schema_cache is None:
            self._schema_cache = [
                t.to_provider_format() for t in self._tools.values()
                if self._available(t)
            ]
        return self._schema_cache

    def execute(self, name: str, arguments: Dict[str, Any]) -> str:
        defn = self._tools.get(name)
        if defn is None:
            raise ToolNotFoundError(f"Unknown tool: {name}", tool_name=name)
        if defn.handler is None:
            raise ToolError(f"Tool {name} has no handler", tool_name=name)
        if not self._available(defn):
            missing = [r for r in defn.requires if not self._capabilities.get(r, True)]
            raise ToolError(
                f"Tool {name} unavailable — requires: {', '.join(missing)}",
                tool_name=name,
            )

        arguments = self._coerce_arguments(defn, arguments)

        # Check cache for read-only tools
        cached = self._result_cache.get(name, arguments)
        if cached is not None:
            return cached

        timeout = defn.timeout if defn.timeout is not None else _DEFAULT_TOOL_TIMEOUT

        try:
            future = _executor.submit(defn.handler, **arguments)
            result = future.result(timeout=timeout)
        except FuturesTimeoutError:
            future.cancel()
            raise ToolError(
                f"Tool {name} timed out after {timeout}s", tool_name=name,
            )
        except (ToolError, ToolValidationError):
            raise
        except TypeError as e:
            raise ToolValidationError(
                f"Invalid arguments for {name}: {e}", tool_name=name
            ) from e
        except Exception as e:
            raise ToolError(f"Tool {name} failed: {e}", tool_name=name) from e

        result_str = self._format_result(result)
        if len(result_str) > TOOL_RESULT_TRUNCATE_LEN:
            result_str = result_str[:TOOL_RESULT_TRUNCATE_LEN] + "\n... (truncated)"

        # Cache result for read-only tools; invalidate on mutating tools
        self._result_cache.put(name, arguments, result_str)
        if defn.mutating:
            self._result_cache.invalidate()

        return result_str

    @staticmethod
    def _format_result(result: Any) -> str:
        if result is None:
            return "OK"
        if isinstance(result, str):
            return result
        if isinstance(result, (dict, list)):
            return json.dumps(result, indent=2, default=str)
        return str(result)


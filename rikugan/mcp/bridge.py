"""Bridge MCP tools into Rikugan ToolRegistry."""

from __future__ import annotations

import json
from collections.abc import Callable
from typing import Any

from ..constants import MCP_TOOL_PREFIX
from ..core.logging import log_info, log_warning
from ..tools.base import ParameterSchema, ToolDefinition
from ..tools.registry import ToolRegistry
from .client import MCPClient

# Every declared tool is re-sent on every request, so a server's documentation
# is a per-turn cost. Servers written for a chat client often ship paragraphs;
# keep enough to choose the tool and drop the rest.
_MAX_TOOL_DESCRIPTION = 400
_MAX_PARAM_DESCRIPTION = 150

# Both Anthropic and OpenAI reject tool names longer than this.
_MAX_TOOL_NAME = 64


def _clip(text: str, limit: int) -> str:
    """Shorten *text* to *limit* characters on a word boundary where possible."""
    text = " ".join((text or "").split())
    if len(text) <= limit:
        return text
    cut = text[:limit]
    space = cut.rfind(" ")
    if space > limit // 2:
        cut = cut[:space]
    return cut.rstrip(" .,;:") + "\u2026"


def _mcp_schema_to_parameters(input_schema: dict[str, Any]) -> list[ParameterSchema]:
    """Convert a JSON Schema object to a list of ParameterSchema."""
    params: list[ParameterSchema] = []

    properties = input_schema.get("properties", {})
    required = set(input_schema.get("required", []))

    for name, prop in properties.items():
        json_type = prop.get("type", "string")
        # Normalize array types
        if isinstance(json_type, list):
            json_type = json_type[0] if json_type else "string"

        ps = ParameterSchema(
            name=name,
            type=json_type,
            description=_clip(prop.get("description", ""), _MAX_PARAM_DESCRIPTION),
            required=name in required,
            default=prop.get("default"),
            enum=prop.get("enum"),
            items=prop.get("items"),
        )
        params.append(ps)

    return params


def _make_mcp_handler(client: MCPClient, tool_name: str) -> Callable:
    """Create a closure that calls client.call_tool() for the given tool."""

    def handler(**kwargs: Any) -> str:
        return client.call_tool(tool_name, kwargs)

    handler.__name__ = f"mcp_{client.name}_{tool_name}"
    handler.__doc__ = f"MCP tool: {tool_name} (server: {client.name})"
    return handler


def register_mcp_tools(client: MCPClient, registry: ToolRegistry, prefix: str = "") -> int:
    """Register all tools from an MCP client into the Rikugan ToolRegistry.

    Returns the number of tools registered.
    """
    if not prefix:
        # Sanitize server name for use in tool names
        safe_name = client.name.replace("-", "_").replace(".", "_")
        prefix = f"{MCP_TOOL_PREFIX}{safe_name}_"

    tools = client.get_tools()
    count = 0
    skipped: list[str] = []

    for mcp_tool in tools:
        rikugan_name = f"{prefix}{mcp_tool.name}"
        if len(rikugan_name) > _MAX_TOOL_NAME:
            skipped.append(mcp_tool.name)
            continue
        description = f"[MCP:{client.name}] {_clip(mcp_tool.description, _MAX_TOOL_DESCRIPTION)}"
        parameters = _mcp_schema_to_parameters(mcp_tool.input_schema)
        handler = _make_mcp_handler(client, mcp_tool.name)

        defn = ToolDefinition(
            name=rikugan_name,
            description=description,
            parameters=parameters,
            category=f"mcp:{client.name}",
            handler=handler,
        )
        registry.register(defn)
        count += 1

    if skipped:
        log_warning(f"MCP[{client.name}]: skipped {len(skipped)} tools whose names exceed {_MAX_TOOL_NAME} chars")
    log_info(f"Registered {count} MCP tools from {client.name} (prefix={prefix}); {describe_payload(registry)}")
    return count


def describe_payload(registry: ToolRegistry) -> str:
    """Size of the tool declaration the model is sent on every request.

    Worth a log line: a large tool set is charged again on each turn, and it is
    otherwise invisible when a provider rejects the request for being too big.
    """
    schemas = registry.to_provider_format()
    size = len(json.dumps(schemas))
    return f"{len(schemas)} tools declared, ~{size // 1024} KB (~{size // 4} tokens) per request"

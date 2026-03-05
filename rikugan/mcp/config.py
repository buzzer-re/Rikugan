"""MCP server configuration: load and save mcp.json from the Rikugan config directory."""

from __future__ import annotations

import json
import os
from dataclasses import dataclass, field
from typing import Dict, List

from ..core.config import RikuganConfig
from ..core.errors import MCPError
from ..core.logging import log_debug, log_error


@dataclass
class MCPServerConfig:
    """Configuration for a single MCP server."""

    name: str
    command: str
    args: List[str] = field(default_factory=list)
    env: Dict[str, str] = field(default_factory=dict)
    enabled: bool = True
    timeout: float = 30.0


def load_mcp_config(path: str = "") -> List[MCPServerConfig]:
    """Load MCP server configurations from the Rikugan config directory.

    When *path* is not given, uses ``RikuganConfig().mcp_config_path``
    (``~/.idapro/rikugan/mcp.json``).

    Returns an empty list if the file doesn't exist (graceful no-op).
    """
    if not path:
        path = RikuganConfig().mcp_config_path

    if not os.path.isfile(path):
        log_debug(f"MCP config not found: {path}")
        return []

    try:
        with open(path, "r", encoding="utf-8") as f:
            data = json.load(f)
    except (json.JSONDecodeError, OSError) as e:
        log_error(f"Failed to load MCP config: {e}")
        return []

    servers_dict = data.get("mcpServers", {})
    servers: List[MCPServerConfig] = []

    for name, cfg in servers_dict.items():
        if not isinstance(cfg, dict):
            continue
        server = MCPServerConfig(
            name=name,
            command=cfg.get("command", ""),
            args=cfg.get("args", []),
            env=cfg.get("env", {}),
            enabled=cfg.get("enabled", True),
            timeout=float(cfg.get("timeout", 30.0)),
        )
        if server.command:
            servers.append(server)
            log_debug(f"MCP server config: {name} cmd={server.command}")
        else:
            log_error(f"MCP server {name}: missing 'command', skipping")

    return servers


def save_mcp_config(servers: List[MCPServerConfig], path: str = "") -> None:
    """Save MCP server configurations back to disk."""
    if not path:
        path = RikuganConfig().mcp_config_path

    servers_dict: Dict[str, dict] = {}
    for s in servers:
        servers_dict[s.name] = {
            "command": s.command,
            "args": s.args,
            "env": s.env,
            "enabled": s.enabled,
        }

    data = {"mcpServers": servers_dict}

    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(data, f, indent=2)

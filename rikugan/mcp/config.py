"""MCP server configuration: load and save mcp.json from the Rikugan config directory."""

from __future__ import annotations

import json
import os
from dataclasses import dataclass, field

from ..constants import CONFIG_DIR_NAME, MCP_CONFIG_FILE
from ..core.host import get_user_config_base_dir
from ..core.logging import log_debug, log_error


def _default_mcp_config_path() -> str:
    """Compute the default MCP config path without instantiating RikuganConfig."""
    return os.path.join(get_user_config_base_dir(), CONFIG_DIR_NAME, MCP_CONFIG_FILE)


@dataclass
class MCPServerConfig:
    """Configuration for a single MCP server.

    A server is reached either by spawning *command* (stdio) or by connecting to
    *url* over HTTP. The second form is what an in-process server such as Binary
    Ninja's own MCP plugin offers: there is no process for us to start.
    """

    name: str
    command: str = ""
    args: list[str] = field(default_factory=list)
    env: dict[str, str] = field(default_factory=dict)
    enabled: bool = True
    timeout: float = 30.0
    url: str = ""
    # Characters of the server's own tool documentation to keep. Its prose is
    # re-sent on every turn, so a verbose server is a standing cost; 0 means
    # use the bridge's default.
    description_limit: int = 0

    @property
    def is_remote(self) -> bool:
        return bool(self.url)


def load_mcp_config(path: str = "") -> list[MCPServerConfig]:
    """Load MCP server configurations from the Rikugan config directory.

    When *path* is not given, computes the default from the host config
    base directory (e.g. ``~/.idapro/rikugan/mcp.json``).

    Returns an empty list if the file doesn't exist (graceful no-op).
    """
    if not path:
        path = _default_mcp_config_path()

    if not os.path.isfile(path):
        log_debug(f"MCP config not found: {path}")
        return []

    try:
        with open(path, encoding="utf-8") as f:
            data = json.load(f)
    except (json.JSONDecodeError, OSError) as e:
        log_error(f"Failed to load MCP config: {e}")
        return []

    servers_dict = data.get("mcpServers", {})
    servers: list[MCPServerConfig] = []

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
            url=cfg.get("url", ""),
            description_limit=int(cfg.get("description_limit", 0)),
        )
        if server.command or server.url:
            servers.append(server)
            log_debug(f"MCP server config: {name} {'url=' + server.url if server.url else 'cmd=' + server.command}")
        else:
            log_error(f"MCP server {name}: needs 'command' or 'url', skipping")

    return servers


def save_mcp_config(servers: list[MCPServerConfig], path: str = "") -> None:
    """Save MCP server configurations back to disk."""
    if not path:
        path = _default_mcp_config_path()

    servers_dict: dict[str, dict] = {}
    for s in servers:
        entry: dict = {
            "command": s.command,
            "args": s.args,
            "env": s.env,
            "enabled": s.enabled,
        }
        if s.url:
            entry["url"] = s.url
        if s.timeout != 30.0:
            entry["timeout"] = s.timeout
        if s.description_limit:
            entry["description_limit"] = s.description_limit
        servers_dict[s.name] = entry

    data = {"mcpServers": servers_dict}

    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(data, f, indent=2)

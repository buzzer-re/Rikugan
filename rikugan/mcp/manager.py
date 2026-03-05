"""MCP manager: orchestrates multiple MCP server connections."""

from __future__ import annotations

import threading
from typing import Callable, Dict, List, Optional

from ..core.logging import log_debug, log_error, log_info
from ..tools.registry import ToolRegistry
from .config import MCPServerConfig, load_mcp_config
from .client import MCPClient
from .bridge import register_mcp_tools


class MCPManager:
    """Manages multiple MCP server connections.

    Servers are started in background threads and their tools are
    registered into the Rikugan ToolRegistry as they come online.
    """

    def __init__(self):
        self._configs: List[MCPServerConfig] = []
        self._clients: Dict[str, MCPClient] = {}
        self._lock = threading.Lock()

    def load_config(self, path: str = "") -> int:
        """Load MCP config. Returns number of enabled servers found."""
        self._configs = load_mcp_config(path)
        enabled = [c for c in self._configs if c.enabled]
        log_info(f"MCP config: {len(enabled)} enabled servers out of {len(self._configs)} total")
        return len(enabled)

    def start_servers(
        self,
        registry: ToolRegistry,
        on_complete: Optional[Callable[[str, int], None]] = None,
    ) -> None:
        """Start all enabled servers in background threads.

        Each server's tools are registered into `registry` as they come online.
        Optional `on_complete(server_name, tool_count)` callback is called per server.
        """
        for config in self._configs:
            if not config.enabled:
                continue

            thread = threading.Thread(
                target=self._start_one,
                args=(config, registry, on_complete),
                daemon=True,
                name=f"mcp-start-{config.name}",
            )
            thread.start()

    def _start_one(
        self,
        config: MCPServerConfig,
        registry: ToolRegistry,
        on_complete: Optional[Callable[[str, int], None]],
    ) -> None:
        """Start a single MCP server (runs in background thread)."""
        client = MCPClient(config)
        try:
            client.start(timeout=config.timeout)
            with self._lock:
                self._clients[config.name] = client
            count = register_mcp_tools(client, registry)
            log_info(f"MCP[{config.name}]: started OK, {count} tools registered")
            if on_complete:
                on_complete(config.name, count)
        except Exception as e:
            log_error(f"MCP[{config.name}]: failed to start: {e}")
            try:
                client.stop()
            except Exception as stop_err:
                log_debug(f"MCP[{config.name}]: cleanup after start failure: {stop_err}")

    def stop_all(self) -> None:
        """Stop all running MCP servers."""
        with self._lock:
            clients = list(self._clients.values())
            self._clients.clear()

        for client in clients:
            try:
                client.stop()
            except Exception as e:
                log_error(f"MCP[{client.name}]: stop error: {e}")

        log_info("MCP: all servers stopped")

    def list_servers(self) -> List[str]:
        """List names of connected servers."""
        with self._lock:
            return list(self._clients.keys())

    def get_client(self, name: str) -> Optional[MCPClient]:
        """Get a client by server name."""
        with self._lock:
            return self._clients.get(name)

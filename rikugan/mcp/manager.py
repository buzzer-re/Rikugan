"""MCP manager: orchestrates multiple MCP server connections."""

from __future__ import annotations

import threading
import time
from collections.abc import Callable

from ..constants import MCP_TOOL_PREFIX
from ..core.logging import log_debug, log_error, log_info, log_warning
from ..tools.registry import ToolRegistry
from .bridge import register_mcp_tools
from .client import MCPClient
from .config import MCPServerConfig, load_mcp_config

# Soft timeout: log a warning if startup takes longer than this.
_SOFT_TIMEOUT = 5.0
# Hard timeout: abort startup entirely after this many seconds.
_HARD_TIMEOUT = 15.0


class MCPManager:
    """Manages multiple MCP server connections.

    Servers are started in background threads and their tools are
    registered into the Rikugan ToolRegistry as they come online.
    """

    def __init__(self):
        self._configs: list[MCPServerConfig] = []
        self._clients: dict[str, MCPClient] = {}
        self._lock = threading.Lock()
        self._shut_down = False
        self._generation = 0  # incremented on each start/reload cycle

    def load_config(self, path: str = "") -> int:
        """Load MCP config. Returns number of enabled servers found."""
        self._configs = load_mcp_config(path)
        enabled = [c for c in self._configs if c.enabled]
        log_info(f"MCP config: {len(enabled)} enabled servers out of {len(self._configs)} total")
        return len(enabled)

    def add_external_configs(self, configs: list[MCPServerConfig]) -> None:
        """Append additional MCP server configs (from external sources).

        These are added to ``_configs`` before ``start_servers()`` is called.
        """
        if not configs:
            return
        self._configs.extend(configs)
        log_info(f"MCP: added {len(configs)} external server config(s)")

    def start_servers(
        self,
        registry: ToolRegistry,
        on_complete: Callable[[str, int], None] | None = None,
    ) -> None:
        """Start all enabled servers in background threads.

        Each server's tools are registered into `registry` as they come online.
        Optional `on_complete(server_name, tool_count)` callback is called per server.
        """
        if self._shut_down:
            log_warning("MCP: start_servers called after shutdown — ignoring")
            return

        with self._lock:
            self._generation += 1
            gen = self._generation

        for config in self._configs:
            if not config.enabled:
                continue

            thread = threading.Thread(
                target=self._start_one,
                args=(config, registry, on_complete, gen),
                daemon=True,
                name=f"mcp-start-{config.name}",
            )
            thread.start()

    def _start_one(
        self,
        config: MCPServerConfig,
        registry: ToolRegistry,
        on_complete: Callable[[str, int], None] | None,
        generation: int,
    ) -> None:
        """Start a single MCP server (runs in background thread).

        Uses a soft timeout (_SOFT_TIMEOUT) to emit a warning and a hard
        timeout (_HARD_TIMEOUT) to abort, preventing indefinite UI freezes.
        The *generation* token guards against late registrations after a
        reload or shutdown has superseded this start cycle.
        """
        hard = min(config.timeout, _HARD_TIMEOUT)
        client = MCPClient(config)
        t0 = time.monotonic()
        try:
            client.start(timeout=hard)
            elapsed = time.monotonic() - t0
            if elapsed > _SOFT_TIMEOUT:
                log_warning(f"MCP[{config.name}]: startup took {elapsed:.1f}s (soft limit {_SOFT_TIMEOUT}s)")
            with self._lock:
                if self._generation != generation or self._shut_down:
                    log_warning(
                        f"MCP[{config.name}]: discarding stale startup (gen {generation} vs current {self._generation})"
                    )
                    client.stop()
                    return
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

    def start_server(
        self,
        config: MCPServerConfig,
        registry: ToolRegistry,
        on_complete: Callable[[str, int], None] | None = None,
    ) -> None:
        """Start one server in the background, leaving the others alone.

        Used for a server the user turns on mid-session, where restarting every
        configured server would be both slow and visible.
        """
        if self._shut_down:
            log_warning("MCP: start_server called after shutdown — ignoring")
            return
        with self._lock:
            gen = self._generation
        threading.Thread(
            target=self._start_one,
            args=(config, registry, on_complete, gen),
            daemon=True,
            name=f"mcp-start-{config.name}",
        ).start()

    def stop_server(self, name: str, registry: ToolRegistry | None = None, prefix: str = "") -> bool:
        """Stop one server and drop its tools from *registry*."""
        with self._lock:
            client = self._clients.pop(name, None)
        if client is None:
            return False
        try:
            client.stop()
        except Exception as e:
            log_error(f"MCP[{name}]: stop failed: {e}")
        if registry is not None and prefix:
            removed = registry.unregister_by_prefix(prefix)
            log_info(f"MCP[{name}]: stopped, {removed} tools removed")
        return True

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

    def shutdown(self) -> None:
        """Stop all servers and prevent further starts.

        Call this during application shutdown instead of ``stop_all()``
        to ensure no new servers are started after cleanup.
        Increments the generation to invalidate any in-flight startups.
        """
        with self._lock:
            self._shut_down = True
            self._generation += 1
        self.stop_all()

    def list_servers(self) -> list[str]:
        """List names of connected servers."""
        with self._lock:
            return list(self._clients.keys())

    def get_client(self, name: str) -> MCPClient | None:
        """Get a client by server name."""
        with self._lock:
            return self._clients.get(name)

    def reload(
        self,
        registry: ToolRegistry,
        config_path: str = "",
        on_complete: Callable[[str, int], None] | None = None,
    ) -> None:
        """Reload MCP config and restart servers.

        Stops all running servers, removes stale MCP tools from the
        registry, re-reads the config file, and starts any newly-enabled
        servers.  Safe to call from a background thread.
        """
        log_info("MCP: reloading configuration")
        self.stop_all()
        registry.unregister_by_prefix(MCP_TOOL_PREFIX)
        self.load_config(config_path)
        self.start_servers(registry, on_complete=on_complete)

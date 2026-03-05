"""MCP client: manages a single MCP server subprocess using the official mcp SDK."""

from __future__ import annotations

import asyncio
import threading
from typing import Any, Dict, List, Optional

from ..constants import MCP_DEFAULT_TIMEOUT
from ..core.errors import MCPConnectionError, MCPError, MCPTimeoutError
from ..core.logging import log_debug, log_error, log_info
from .config import MCPServerConfig

try:
    from mcp import ClientSession
    from mcp.client.stdio import StdioServerParameters, stdio_client

    _HAS_MCP = True
except ImportError:
    _HAS_MCP = False


class MCPToolSchema:
    """Schema for a tool exposed by an MCP server."""

    __slots__ = ("name", "description", "input_schema")

    def __init__(self, name: str = "", description: str = "", input_schema: Optional[Dict[str, Any]] = None):
        self.name = name
        self.description = description
        self.input_schema = input_schema or {}


class MCPClient:
    """Client for a single MCP server process.

    Uses the official ``mcp`` Python SDK (``ClientSession`` + ``stdio_client``)
    running in a dedicated background thread with its own asyncio event loop.
    All public methods are synchronous — they dispatch to the async loop and
    block until the result is ready.
    """

    def __init__(self, config: MCPServerConfig):
        if not _HAS_MCP:
            raise MCPError(
                "The 'mcp' package is required for MCP support. "
                "Install it with: pip install mcp"
            )

        self.config = config
        self.name = config.name
        self._tools: List[MCPToolSchema] = []
        self._started = False
        self._healthy = True
        self._running = False

        # Async internals — set up in start()
        self._loop: Optional[asyncio.AbstractEventLoop] = None
        self._thread: Optional[threading.Thread] = None
        self._session: Optional[ClientSession] = None
        self._shutdown_event: Optional[asyncio.Event] = None
        self._ready = threading.Event()
        self._start_error: Optional[str] = None

    @property
    def is_running(self) -> bool:
        return self._started and self._running and self._thread is not None and self._thread.is_alive()

    @property
    def is_healthy(self) -> bool:
        return self.is_running and self._healthy

    def start(self, timeout: float = 30.0) -> None:
        """Spawn the MCP server process, perform handshake, and discover tools.

        Blocks until the server is ready or the timeout expires.
        """
        log_info(f"MCP[{self.name}]: starting server: {self.config.command} {self.config.args}")

        self._running = True
        self._ready.clear()
        self._start_error = None

        self._thread = threading.Thread(
            target=self._run_loop,
            daemon=True,
            name=f"mcp-{self.name}",
        )
        self._thread.start()

        # Wait for the async loop to finish initialization
        if not self._ready.wait(timeout=timeout):
            self._running = False
            raise MCPConnectionError(
                f"MCP[{self.name}]: initialize timed out after {timeout}s"
            )

        if self._start_error:
            self._running = False
            raise MCPConnectionError(
                f"MCP[{self.name}]: handshake failed: {self._start_error}"
            )

        self._started = True
        log_info(f"MCP[{self.name}]: started OK, {len(self._tools)} tools registered")

    def stop(self) -> None:
        """Shut down the MCP server process."""
        log_debug(f"MCP[{self.name}]: stopping")
        self._running = False
        self._started = False

        # Signal the async loop to exit
        if self._loop and self._shutdown_event and not self._loop.is_closed():
            self._loop.call_soon_threadsafe(self._shutdown_event.set)

        if self._thread and self._thread.is_alive():
            self._thread.join(timeout=5)

        self._session = None
        self._loop = None
        self._thread = None

    def get_tools(self) -> List[MCPToolSchema]:
        return list(self._tools)

    def call_tool(self, name: str, arguments: Dict[str, Any], timeout: float = MCP_DEFAULT_TIMEOUT) -> str:
        """Call an MCP tool and return the result as a string."""
        log_debug(f"MCP[{self.name}]: calling tool {name}")

        try:
            result = self._run_coro(self._async_call_tool(name, arguments), timeout=timeout)
        except MCPTimeoutError:
            raise
        except MCPError:
            raise
        except Exception as e:
            raise MCPError(f"MCP tool {name} error: {e}")

        return result

    # ------------------------------------------------------------------
    # Async internals
    # ------------------------------------------------------------------

    def _run_coro(self, coro, timeout: float = MCP_DEFAULT_TIMEOUT) -> Any:
        """Submit a coroutine to the background loop and block for the result."""
        if not self._loop or self._loop.is_closed():
            raise MCPConnectionError(f"MCP[{self.name}]: not connected")

        future = asyncio.run_coroutine_threadsafe(coro, self._loop)
        try:
            return future.result(timeout=timeout)
        except TimeoutError:
            future.cancel()
            raise MCPTimeoutError(f"MCP[{self.name}]: operation timed out after {timeout}s")

    async def _async_call_tool(self, name: str, arguments: Dict[str, Any]) -> str:
        """Call an MCP tool via the ClientSession."""
        if not self._session:
            raise MCPConnectionError(f"MCP[{self.name}]: no active session")

        result = await self._session.call_tool(name, arguments)

        # Extract text content
        parts = []
        for item in result.content:
            if hasattr(item, "text"):
                parts.append(item.text)
            else:
                parts.append(str(item))
        return "\n".join(parts) if parts else str(result)

    def _run_loop(self) -> None:
        """Background thread: run the asyncio event loop with the MCP session."""
        try:
            asyncio.run(self._async_main())
        except Exception as e:
            if self._running:
                log_error(f"MCP[{self.name}]: event loop error: {e}")
            if not self._ready.is_set():
                self._start_error = str(e)
                self._ready.set()
        finally:
            self._running = False

    async def _async_main(self) -> None:
        """Async entry point: connect, handshake, discover tools, then keep alive."""
        self._loop = asyncio.get_running_loop()
        self._shutdown_event = asyncio.Event()

        server_params = StdioServerParameters(
            command=self.config.command,
            args=self.config.args,
            env=self.config.env if self.config.env else None,
        )

        try:
            async with stdio_client(server_params) as (read_stream, write_stream):
                async with ClientSession(read_stream, write_stream) as session:
                    self._session = session

                    # Initialize handshake
                    init_result = await session.initialize()
                    log_debug(f"MCP[{self.name}]: initialized, server: {init_result.server_info}")

                    # Discover tools
                    tools_result = await session.list_tools()
                    self._tools = []
                    for t in tools_result.tools:
                        self._tools.append(MCPToolSchema(
                            name=t.name,
                            description=t.description or "",
                            input_schema=t.inputSchema if hasattr(t, "inputSchema") else {},
                        ))
                    log_info(f"MCP[{self.name}]: discovered {len(self._tools)} tools")

                    # Signal that initialization is complete
                    self._ready.set()

                    # Keep the session alive until stop() is called
                    await self._shutdown_event.wait()

        except Exception as e:
            if not self._ready.is_set():
                self._start_error = str(e)
                self._ready.set()
            else:
                log_error(f"MCP[{self.name}]: session error: {e}")

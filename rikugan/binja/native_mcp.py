"""Binary Ninja's own MCP server, when one is running.

Binary Ninja 6.0 ships an MCP server plugin that exposes the host's analysis
over HTTP. It lives in the same process we do, so there is nothing to spawn —
we connect to it, and only after the user has agreed for this binary.

Detection is a real MCP handshake rather than a port check: a wrong or stale
URL simply fails to connect and the offer is never made.
"""

from __future__ import annotations

from dataclasses import dataclass

from ..constants import MCP_TOOL_PREFIX
from ..core.logging import log_debug, log_info
from ..mcp.client import MCPClient
from ..mcp.config import MCPServerConfig

SERVER_NAME = "binaryninja"
DEFAULT_URL = "http://127.0.0.1:9009/mcp"

# The probe runs on a background thread, but a server that is not there should
# fail fast rather than hold the offer back.
_PROBE_TIMEOUT = 4.0


def tool_prefix() -> str:
    """Prefix the bridge gives this server's tools in the registry."""
    return f"{MCP_TOOL_PREFIX}{SERVER_NAME.replace('-', '_').replace('.', '_')}_"


def server_config(url: str, timeout: float = 30.0) -> MCPServerConfig:
    return MCPServerConfig(name=SERVER_NAME, url=url or DEFAULT_URL, timeout=timeout)


@dataclass
class ProbeResult:
    """What a detection attempt found."""

    available: bool
    url: str
    tool_count: int = 0
    error: str = ""


def probe(url: str = "") -> ProbeResult:
    """Connect to the server and list its tools. Safe to call off the UI thread.

    Returns ``available=False`` with the reason rather than raising: a missing
    server is the normal case, not an error worth surfacing.
    """
    target = url or DEFAULT_URL
    client = MCPClient(server_config(target, timeout=_PROBE_TIMEOUT))
    try:
        client.start(timeout=_PROBE_TIMEOUT)
        count = len(client.get_tools())
        log_info(f"Binary Ninja MCP server found at {target} ({count} tools)")
        return ProbeResult(available=True, url=target, tool_count=count)
    except Exception as e:
        log_debug(f"No Binary Ninja MCP server at {target}: {e}")
        return ProbeResult(available=False, url=target, error=str(e))
    finally:
        try:
            client.stop()
        except Exception as e:  # pragma: no cover - teardown of a failed probe
            log_debug(f"Binary Ninja MCP probe cleanup: {e}")

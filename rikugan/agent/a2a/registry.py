"""External agent registry: auto-discovers CLI agents available on PATH."""

from __future__ import annotations

import shutil

from ...core.logging import log_debug, log_info
from .types import ExternalAgentConfig

# ---------------------------------------------------------------------------
# Known CLI agents and their default configurations
# ---------------------------------------------------------------------------

_KNOWN_AGENTS: dict[str, dict[str, str | list[str]]] = {
    "claude": {
        "display_name": "Claude Code",
        "executable": "claude",
        "capabilities": ["code_analysis", "general_reasoning", "code_generation"],
    },
    "codex": {
        "display_name": "Codex",
        "executable": "codex",
        "capabilities": ["code_analysis", "code_generation"],
    },
}


# ---------------------------------------------------------------------------
# Registry
# ---------------------------------------------------------------------------


class ExternalAgentRegistry:
    """Registry of external agents discovered on the system.

    Auto-discovers known CLI agents by checking if their executables
    are available on PATH via ``shutil.which``.
    """

    def __init__(self) -> None:
        self._agents: dict[str, ExternalAgentConfig] = {}

    def discover(self) -> int:
        """Scan PATH for known CLI agents. Returns the number discovered."""
        self._agents.clear()
        discovered = 0

        for agent_key, info in _KNOWN_AGENTS.items():
            executable = str(info["executable"])
            path = shutil.which(executable)
            if path is not None:
                capabilities = info.get("capabilities", [])
                if isinstance(capabilities, str):
                    capabilities = [capabilities]

                config = ExternalAgentConfig(
                    name=agent_key,
                    transport="subprocess",
                    endpoint=path,
                    capabilities=list(capabilities),
                )
                self._agents[agent_key] = config
                discovered += 1
                log_debug(f"Discovered external agent: {info['display_name']} at {path}")
            else:
                log_debug(f"External agent not found on PATH: {executable}")

        if discovered:
            log_info(f"Discovered {discovered} external agent(s)")
        return discovered

    def register(self, config: ExternalAgentConfig) -> None:
        """Manually register an external agent."""
        self._agents[config.name] = config
        log_debug(f"Registered external agent: {config.name}")

    def unregister(self, name: str) -> bool:
        """Remove an agent from the registry. Returns True if it existed."""
        if name in self._agents:
            del self._agents[name]
            log_debug(f"Unregistered external agent: {name}")
            return True
        return False

    def get(self, name: str) -> ExternalAgentConfig | None:
        """Look up an agent by name."""
        return self._agents.get(name)

    def list_agents(self) -> list[ExternalAgentConfig]:
        """Return all registered agents."""
        return list(self._agents.values())

    def list_names(self) -> list[str]:
        """Return names of all registered agents."""
        return list(self._agents.keys())

    def has(self, name: str) -> bool:
        """Check if an agent is registered."""
        return name in self._agents

"""Rikugan configuration with JSON persistence."""

from __future__ import annotations

import json
import os
from dataclasses import asdict, dataclass, field
from typing import TYPE_CHECKING, Any

if TYPE_CHECKING:
    from .profile import AnalysisProfile

from ..constants import (
    CONFIG_DIR_NAME,
    CONFIG_FILE_NAME,
    CONFIG_SCHEMA_VERSION,
    DEFAULT_CONTEXT_WINDOW,
    DEFAULT_MAX_TOKENS,
    DEFAULT_TEMPERATURE,
    MCP_CONFIG_FILE,
    SKILLS_DIR_NAME,
)
from .host import get_user_config_base_dir
from .logging import log_error


def _default_config_dir() -> str:
    return os.path.join(get_user_config_base_dir(), CONFIG_DIR_NAME)


@dataclass
class ProviderConfig:
    name: str = "anthropic"
    model: str = "claude-sonnet-4-20250514"
    api_key: str = ""
    api_base: str = ""
    temperature: float = DEFAULT_TEMPERATURE
    max_tokens: int = DEFAULT_MAX_TOKENS
    context_window: int = DEFAULT_CONTEXT_WINDOW
    extra: dict[str, Any] = field(default_factory=dict)


@dataclass
class RikuganConfig:
    provider: ProviderConfig = field(default_factory=ProviderConfig)
    providers: dict[str, dict[str, Any]] = field(default_factory=dict)
    custom_providers: dict[str, dict[str, Any]] = field(default_factory=dict)
    auto_context: bool = True
    plan_mode_default: bool = False
    checkpoint_auto_save: bool = True
    approve_mutations: bool = False  # require approval for mutating tools (rename, retype, etc.)
    exploration_turn_limit: int = 30  # max turns in exploration phase before forcing transition
    max_retries: int = 3  # max retries on rate-limit / transient API errors
    silent_retry_mode: bool = False  # show loading indicator instead of error messages on retry
    theme: str = "dark"

    # Skills & MCP external integration
    disabled_skills: list[str] = field(default_factory=list)
    enabled_external_skills: list[str] = field(default_factory=list)
    enabled_external_mcp: list[str] = field(default_factory=list)

    # Analysis profiles
    active_profile: str = "default"
    custom_profiles: dict[str, dict] = field(default_factory=dict)

    # A2A / external agents
    a2a_auto_discover: bool = True
    a2a_agents: list[dict[str, Any]] = field(default_factory=list)

    # Bulk renamer defaults
    bulk_renamer_batch_size: int = 10
    bulk_renamer_max_concurrent: int = 3

    _config_dir: str = field(default_factory=_default_config_dir, repr=False)

    @property
    def config_path(self) -> str:
        return os.path.join(self._config_dir, CONFIG_FILE_NAME)

    @property
    def checkpoints_dir(self) -> str:
        return os.path.join(self._config_dir, "checkpoints")

    @property
    def skills_dir(self) -> str:
        return os.path.join(self._config_dir, SKILLS_DIR_NAME)

    @property
    def mcp_config_path(self) -> str:
        return os.path.join(self._config_dir, MCP_CONFIG_FILE)

    def validate(self) -> list[str]:
        """Validate config values. Returns list of error messages (empty = valid)."""
        errors: list[str] = []
        if not (0.0 <= self.provider.temperature <= 2.0):
            errors.append(f"temperature {self.provider.temperature} out of range [0, 2]")
        if self.provider.max_tokens <= 0:
            errors.append(f"max_tokens must be positive, got {self.provider.max_tokens}")
        if self.provider.context_window <= 0:
            errors.append(f"context_window must be positive, got {self.provider.context_window}")
        if not (1 <= self.max_retries <= 10):
            errors.append(f"max_retries {self.max_retries} out of range [1, 10]")
        if not self.active_profile or not isinstance(self.active_profile, str):
            errors.append("active_profile must be a non-empty string")
        if not isinstance(self.custom_profiles, dict):
            errors.append("custom_profiles must be a dict")
        else:
            for k, v in self.custom_profiles.items():
                if not isinstance(v, dict):
                    errors.append(f"custom_profiles['{k}'] must be a dict")
        return errors

    def save(self) -> None:
        errors = self.validate()
        if errors:
            for err in errors:
                log_error(f"Config validation: {err}")
            # Clamp to valid ranges rather than refusing to save
            self.provider.temperature = max(0.0, min(2.0, self.provider.temperature))
            self.provider.max_tokens = max(1, self.provider.max_tokens)
            self.provider.context_window = max(1024, self.provider.context_window)
            self.max_retries = max(1, min(10, self.max_retries))

        os.makedirs(self._config_dir, exist_ok=True)
        # Snapshot current provider into the providers dict before saving
        self._snapshot_current_provider()
        d = asdict(self)
        d.pop("_config_dir", None)
        d["schema_version"] = CONFIG_SCHEMA_VERSION
        with open(self.config_path, "w") as f:
            json.dump(d, f, indent=2)

    def load(self) -> None:
        if not os.path.exists(self.config_path):
            return
        with open(self.config_path) as f:
            data = json.load(f)
        # Schema version check (for future migrations)
        _stored_version = data.pop("schema_version", 0)
        if "provider" in data:
            for k, v in data["provider"].items():
                if hasattr(self.provider, k):
                    setattr(self.provider, k, v)
        self.providers = data.get("providers", {})
        self.custom_providers = data.get("custom_providers", {})
        for k in (
            "auto_context",
            "plan_mode_default",
            "checkpoint_auto_save",
            "approve_mutations",
            "exploration_turn_limit",
            "max_retries",
            "silent_retry_mode",
            "theme",
            "disabled_skills",
            "enabled_external_skills",
            "enabled_external_mcp",
            "active_profile",
            "custom_profiles",
            "a2a_auto_discover",
            "a2a_agents",
            "bulk_renamer_batch_size",
            "bulk_renamer_max_concurrent",
        ):
            if k in data:
                setattr(self, k, data[k])

    def _snapshot_current_provider(self) -> None:
        """Store current provider settings into the providers dict."""
        name = self.provider.name
        self.providers[name] = {
            "model": self.provider.model,
            "api_key": self.provider.api_key,
            "api_base": self.provider.api_base,
            "temperature": self.provider.temperature,
            "max_tokens": self.provider.max_tokens,
            "context_window": self.provider.context_window,
        }

    def switch_provider(self, new_name: str) -> None:
        """Switch to a different provider, preserving current settings.

        Saves the current provider's config and restores the new one
        (if previously configured).
        """
        self._snapshot_current_provider()
        self.provider.name = new_name

        saved = self.providers.get(new_name, {})
        if saved:
            self.provider.model = saved.get("model", "")
            self.provider.api_key = saved.get("api_key", "")
            self.provider.api_base = saved.get("api_base", "")
            self.provider.temperature = saved.get("temperature", DEFAULT_TEMPERATURE)
            self.provider.max_tokens = saved.get("max_tokens", DEFAULT_MAX_TOKENS)
            self.provider.context_window = saved.get("context_window", DEFAULT_CONTEXT_WINDOW)
        else:
            # Fresh provider — clear key/base, keep defaults
            self.provider.api_key = ""
            self.provider.api_base = ""
            self.provider.model = ""
            self.provider.temperature = DEFAULT_TEMPERATURE
            self.provider.max_tokens = DEFAULT_MAX_TOKENS
            self.provider.context_window = DEFAULT_CONTEXT_WINDOW

    def add_custom_provider(self, name: str) -> None:
        """Register a new custom OpenAI-compatible provider name."""
        self.custom_providers[name] = {}

    def remove_custom_provider(self, name: str) -> None:
        """Remove a custom provider and its saved settings."""
        self.custom_providers.pop(name, None)
        self.providers.pop(name, None)

    def is_custom_provider(self, name: str) -> bool:
        return name in self.custom_providers

    def get_active_profile(self) -> AnalysisProfile:
        """Return the currently active AnalysisProfile."""
        from .profile import get_profile

        return get_profile(self.active_profile, self.custom_profiles)

    @classmethod
    def load_or_create(cls) -> RikuganConfig:
        cfg = cls()
        cfg.load()
        return cfg

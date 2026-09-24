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
    restore_sessions_on_start: bool = True
    # When True, saved chats are listed in the sidebar but none is opened on
    # startup — a placeholder is shown until the user picks or creates one.
    dont_auto_load_chats: bool = False
    session_storage_dir: str = ""
    approve_mutations: bool = False  # require approval for mutating tools (rename, retype, etc.)
    # Binary Ninja only. Its BinaryView API is thread-safe, so read-only tools
    # run on the agent's background thread; marshalling them onto the UI thread
    # froze the whole window for the length of every decompile. Mutations and
    # UI-driving tools are still marshalled either way. Set False to send
    # everything back through the main thread.
    binja_background_tools: bool = True
    # Binary Ninja 6.0 ships an MCP server plugin. When one is running Rikugan
    # offers to use it, once per binary; the answer is remembered here, keyed by
    # database instance id, so a binary is never asked twice.
    binja_mcp_url: str = ""
    binja_mcp_consent: dict[str, bool] = field(default_factory=dict)
    exploration_turn_limit: int = 100  # max turns in exploration phase before forcing transition
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

    # Context management
    preserve_context: bool = False  # disable tool result truncation + context compaction

    # OAuth consent — user must accept risk before keychain autoload
    oauth_consent_accepted: bool = False

    # Bulk renamer defaults
    bulk_renamer_batch_size: int = 10
    bulk_renamer_max_concurrent: int = 3

    # API key encryption
    encrypt_api_keys: bool = False
    _encryption_block: dict = field(default_factory=dict, repr=False)

    _config_dir: str = field(default_factory=_default_config_dir, repr=False)

    @property
    def config_path(self) -> str:
        return os.path.join(self._config_dir, CONFIG_FILE_NAME)

    @property
    def checkpoints_dir(self) -> str:
        if self.session_storage_dir:
            return os.path.abspath(os.path.expanduser(self.session_storage_dir))
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

    def save(self, password: str = "") -> None:
        errors = self.validate()
        if errors:
            for err in errors:
                log_error(f"Config validation: {err}")
            # Clamp to valid ranges rather than refusing to save
            self.provider.context_window = max(1024, self.provider.context_window)
            self.max_retries = max(1, min(10, self.max_retries))

        os.makedirs(self._config_dir, exist_ok=True)
        # Snapshot current provider into the providers dict before saving
        self._snapshot_current_provider()
        d = asdict(self)
        d.pop("_config_dir", None)
        d.pop("_encryption_block", None)
        d["schema_version"] = CONFIG_SCHEMA_VERSION

        if self.encrypt_api_keys and password:
            from .crypto import encrypt_keys

            # Collect all API keys into a single blob
            key_data = {
                "provider_api_key": d["provider"]["api_key"],
                "providers": {name: info.get("api_key", "") for name, info in d.get("providers", {}).items()},
            }
            d["encryption"] = {"enabled": True, **encrypt_keys(password, key_data)}
            # Zero out plaintext keys on disk
            d["provider"]["api_key"] = ""
            for info in d.get("providers", {}).values():
                info["api_key"] = ""
        else:
            d["encryption"] = {"enabled": False}

        with open(self.config_path, "w", encoding="utf-8") as f:
            json.dump(d, f, indent=2)

    def load(self) -> None:
        if not os.path.exists(self.config_path):
            return
        with open(self.config_path, encoding="utf-8") as f:
            data = json.load(f)
        # Schema version check (for future migrations)
        _stored_version = data.pop("schema_version", 0)

        # Detect encrypted API keys — actual decryption deferred to
        # decrypt_stored_keys() which is called at session start.
        enc = data.pop("encryption", {})
        if enc.get("enabled"):
            self.encrypt_api_keys = True
            self._encryption_block = enc

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
            "restore_sessions_on_start",
            "dont_auto_load_chats",
            "session_storage_dir",
            "approve_mutations",
            "binja_background_tools",
            "binja_mcp_url",
            "binja_mcp_consent",
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
            "oauth_consent_accepted",
            "encrypt_api_keys",
        ):
            if k in data:
                setattr(self, k, data[k])

    def has_encrypted_keys(self) -> bool:
        """True if the config was loaded with encrypted keys pending decryption."""
        return self.encrypt_api_keys and bool(self._encryption_block)

    def decrypt_stored_keys(self, password: str) -> bool:
        """Decrypt stored API keys using *password*.

        Returns True on success, False on wrong password.
        """
        if not self._encryption_block:
            return True
        try:
            from .crypto import decrypt_keys

            keys = decrypt_keys(password, self._encryption_block)
        except ValueError:
            return False

        # Restore plaintext keys into the live config
        self.provider.api_key = keys.get("provider_api_key", "")
        for name, key in keys.get("providers", {}).items():
            if name in self.providers:
                self.providers[name]["api_key"] = key

        # Restore the current provider's key from the providers snapshot
        saved = self.providers.get(self.provider.name, {})
        if saved.get("api_key"):
            self.provider.api_key = saved["api_key"]

        self._encryption_block = {}
        return True

    def _snapshot_current_provider(self) -> None:
        """Store current provider settings into the providers dict."""
        name = self.provider.name
        self.providers[name] = {
            "model": self.provider.model,
            "api_key": self.provider.api_key,
            "api_base": self.provider.api_base,
            "context_window": self.provider.context_window,
            "extra": self.provider.extra,
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
            self.provider.context_window = saved.get("context_window", DEFAULT_CONTEXT_WINDOW)
            self.provider.extra = saved.get("extra", {})
        else:
            # Fresh provider — clear key/base, keep defaults
            self.provider.api_key = ""
            self.provider.api_base = ""
            self.provider.model = ""
            self.provider.context_window = DEFAULT_CONTEXT_WINDOW
            self.provider.extra = dict(self.custom_providers.get(new_name, {}))

    def add_custom_provider(self, name: str, settings: dict[str, Any] | None = None) -> None:
        """Register a new custom OpenAI-compatible provider name."""
        self.custom_providers[name] = settings or {}

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

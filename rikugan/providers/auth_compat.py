"""Compatibility helpers for auth-cache lifecycle operations."""

from __future__ import annotations

import importlib
from types import ModuleType
from typing import Any

from ..core.logging import log_debug, log_warning


def _load_auth_cache() -> ModuleType | None:
    try:
        return importlib.import_module("rikugan.providers.auth_cache")
    except ImportError as e:
        log_warning(f"Unable to import auth cache: {e}")
        return None


def _module_origin(module: ModuleType) -> str:
    file_path = getattr(module, "__file__", "")
    if isinstance(file_path, str) and file_path:
        return file_path
    spec = getattr(module, "__spec__", None)
    origin = getattr(spec, "origin", "")
    if isinstance(origin, str) and origin:
        return origin
    return "unknown location"


def _call_module_func(module: ModuleType, name: str, *args: Any) -> bool:
    func = getattr(module, name, None)
    if not callable(func):
        log_warning(
            f"rikugan.providers.auth_cache is missing {name} "
            f"from {_module_origin(module)}; installation may be stale or mixed. "
            "Restart the host and rerun the Rikugan installer after updating.",
        )
        return False
    try:
        func(*args)
    except Exception as e:
        log_debug(f"auth_cache.{name} failed: {e}")
        return False
    return True


def apply_keychain_consent(accepted: bool) -> bool:
    """Apply persisted OAuth keychain consent if the loaded auth cache supports it."""
    auth_cache = _load_auth_cache()
    if auth_cache is None:
        return False
    return _call_module_func(auth_cache, "set_keychain_consent", accepted)


def invalidate_auth_cache() -> bool:
    """Invalidate cached auth if the loaded auth cache supports it."""
    auth_cache = _load_auth_cache()
    if auth_cache is None:
        return False
    return _call_module_func(auth_cache, "invalidate_cache")

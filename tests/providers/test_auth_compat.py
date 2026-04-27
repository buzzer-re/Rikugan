"""Tests for auth cache compatibility helpers."""

from __future__ import annotations

import sys
import types

from rikugan.providers.auth_compat import apply_keychain_consent, invalidate_auth_cache


def test_apply_keychain_consent_calls_available_setter(monkeypatch):
    module = types.ModuleType("rikugan.providers.auth_cache")
    calls: list[tuple[str, bool | None]] = []

    def _set_keychain_consent(accepted: bool) -> None:
        calls.append(("set", accepted))

    def _invalidate_cache() -> None:
        calls.append(("invalidate", None))

    module.set_keychain_consent = _set_keychain_consent
    module.invalidate_cache = _invalidate_cache
    monkeypatch.setitem(sys.modules, "rikugan.providers.auth_cache", module)

    assert apply_keychain_consent(True) is True
    assert invalidate_auth_cache() is True
    assert calls == [("set", True), ("invalidate", None)]


def test_apply_keychain_consent_handles_stale_auth_cache(monkeypatch):
    module = types.ModuleType("rikugan.providers.auth_cache")
    monkeypatch.setitem(sys.modules, "rikugan.providers.auth_cache", module)

    assert apply_keychain_consent(False) is False

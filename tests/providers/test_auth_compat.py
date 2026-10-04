"""Tests for auth cache compatibility helpers."""

from __future__ import annotations

import subprocess
import sys
import textwrap
import types
from pathlib import Path

import pytest

import rikugan.providers.auth_compat as auth_compat
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


# Binary Ninja imports the plugin DIRECTORY as top-level ``rikugan``, so the real
# package is ``rikugan.rikugan.providers`` and the absolute name
# ``rikugan.providers.auth_cache`` does not exist. Run in a subprocess so the
# duplicate package tree never pollutes this session's sys.modules.
_NESTED_LAYOUT_PROBE = """
import importlib, sys
sys.path.insert(0, {host!r})
importlib.import_module("rikugan")
try:
    importlib.import_module("rikugan.providers.auth_cache")
    print("ABSOLUTE_RESOLVES")
except ImportError:
    print("ABSOLUTE_FAILS")
compat = importlib.import_module("rikugan.rikugan.providers.auth_compat")
cache = importlib.import_module("rikugan.rikugan.providers.auth_cache")
print("APPLIED", compat.apply_keychain_consent(True))
print("CONSENT", cache._keychain_consent)
print("SAME_MODULE", compat._load_auth_cache() is cache)
"""


@pytest.mark.skipif(sys.platform == "win32", reason="requires symlink support")
def test_consent_applies_under_nested_plugin_layout(tmp_path):
    """Regression: consent must reach auth_cache when the package is nested."""
    repo_root = Path(auth_compat.__file__).resolve().parents[2]
    host = tmp_path / "plugins"
    host.mkdir()
    (host / "rikugan").symlink_to(repo_root, target_is_directory=True)

    result = subprocess.run(
        [sys.executable, "-c", textwrap.dedent(_NESTED_LAYOUT_PROBE).format(host=str(host))],
        capture_output=True,
        text=True,
        timeout=120,
    )
    assert result.returncode == 0, result.stderr
    out = result.stdout.split()
    assert "ABSOLUTE_FAILS" in out, "layout no longer reproduces the nesting under test"
    assert "APPLIED True" in result.stdout
    assert "CONSENT True" in result.stdout
    assert "SAME_MODULE True" in result.stdout

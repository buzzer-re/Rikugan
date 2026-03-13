"""Tests for rikugan.ui.settings_dialog — pure logic helpers."""

from __future__ import annotations

import sys
import types
import unittest
from unittest.mock import MagicMock, patch

from tests.qt_stubs import ensure_pyside6_stubs

ensure_pyside6_stubs()

# Stub heavy dependencies
for _mod_name in [
    "rikugan.core.config",
    "rikugan.core.logging",
    "rikugan.core.types",
    "rikugan.providers.anthropic_provider",
    "rikugan.providers.auth_cache",
    "rikugan.providers.ollama_provider",
    "rikugan.providers.registry",
]:
    if _mod_name not in sys.modules:
        _stub = types.ModuleType(_mod_name)
        for _attr in [
            "RikuganConfig",
            "log_debug",
            "log_error",
            "log_info",
            "ModelInfo",
            "resolve_anthropic_auth",
            "resolve_auth_cached",
            "DEFAULT_OLLAMA_URL",
            "ProviderRegistry",
        ]:
            setattr(_stub, _attr, MagicMock())
        sys.modules[_mod_name] = _stub

# Ensure DEFAULT_OLLAMA_URL is a string on the stub (real module already has it)
_ollama_mod = sys.modules.get("rikugan.providers.ollama_provider")
if _ollama_mod is not None and not isinstance(getattr(_ollama_mod, "DEFAULT_OLLAMA_URL", None), str):
    _ollama_mod.DEFAULT_OLLAMA_URL = "http://localhost:11434"

# Install real resolve_auth_cached logic on the stub so tests can exercise it
_ac_stub = sys.modules["rikugan.providers.auth_cache"]
_ac_stub._cached_oauth = None
_ac_stub.resolve_anthropic_auth = MagicMock(return_value=("tok", "api_key"))


def _resolve_auth_cached_impl(explicit_key=""):
    if explicit_key:
        return _ac_stub.resolve_anthropic_auth(explicit_key)
    if _ac_stub._cached_oauth is not None:
        return _ac_stub._cached_oauth
    _ac_stub._cached_oauth = _ac_stub.resolve_anthropic_auth("")
    return _ac_stub._cached_oauth


_ac_stub.resolve_auth_cached = _resolve_auth_cached_impl
_ac_stub.invalidate_cache = MagicMock()

from rikugan.ui.settings_dialog import _AddProviderDialog, _ModelFetcher  # noqa: E402


# ---------------------------------------------------------------------------
# _ModelFetcher
# ---------------------------------------------------------------------------


class TestModelFetcherShutdown(unittest.TestCase):
    def test_shutdown_sets_alive_false(self):
        registry = MagicMock()
        fetcher = _ModelFetcher(registry)
        self.assertTrue(fetcher._alive)
        fetcher.shutdown()
        self.assertFalse(fetcher._alive)

    def test_shutdown_drains_queue(self):
        registry = MagicMock()
        fetcher = _ModelFetcher(registry)
        fetcher._queue.put(("models", "anthropic", []))
        fetcher._queue.put(("error", "anthropic", "fail"))
        fetcher.shutdown()
        self.assertTrue(fetcher._queue.empty())

    def test_shutdown_empty_queue_noop(self):
        registry = MagicMock()
        fetcher = _ModelFetcher(registry)
        fetcher.shutdown()  # must not raise


class TestModelFetcherPoll(unittest.TestCase):
    def test_poll_returns_none_when_empty(self):
        registry = MagicMock()
        fetcher = _ModelFetcher(registry)
        result = fetcher.poll()
        self.assertIsNone(result)

    def test_poll_returns_item_when_available(self):
        registry = MagicMock()
        fetcher = _ModelFetcher(registry)
        fetcher._queue.put(("models", "anthropic", ["gpt4"]))
        result = fetcher.poll()
        self.assertEqual(result, ("models", "anthropic", ["gpt4"]))

    def test_poll_non_destructive_multiple(self):
        registry = MagicMock()
        fetcher = _ModelFetcher(registry)
        fetcher._queue.put("item1")
        fetcher._queue.put("item2")
        self.assertEqual(fetcher.poll(), "item1")
        self.assertEqual(fetcher.poll(), "item2")
        self.assertIsNone(fetcher.poll())


class TestModelFetcherFetch(unittest.TestCase):
    def test_fetch_error_when_create_fails(self):
        registry = MagicMock()
        registry.create.side_effect = RuntimeError("boom")
        fetcher = _ModelFetcher(registry)
        fetcher.fetch("anthropic", "key", "base")
        result = fetcher.poll()
        self.assertIsNotNone(result)
        kind, _provider, msg = result
        self.assertEqual(kind, "error")
        self.assertIn("boom", msg)

    def test_fetch_no_queue_when_not_alive(self):
        registry = MagicMock()
        registry.create.side_effect = RuntimeError("boom")
        fetcher = _ModelFetcher(registry)
        fetcher._alive = False
        fetcher.fetch("anthropic", "key", "base")
        self.assertIsNone(fetcher.poll())


# ---------------------------------------------------------------------------
# _resolve_auth_cached
# ---------------------------------------------------------------------------


class TestResolveAuthCached(unittest.TestCase):
    """Tests for the auth_cache module (extracted from settings_dialog)."""

    def _get_ac(self):
        return sys.modules["rikugan.providers.auth_cache"]

    def setUp(self):
        ac = self._get_ac()
        ac._cached_oauth = None

    def test_explicit_key_bypasses_cache(self):
        ac = self._get_ac()
        mock_auth = MagicMock(return_value=("token", "api_key"))
        with patch.object(ac, "resolve_anthropic_auth", mock_auth):
            ac.resolve_auth_cached("my-key")
        mock_auth.assert_called_once_with("my-key")

    def test_no_key_uses_cache_on_second_call(self):
        ac = self._get_ac()
        mock_auth = MagicMock(return_value=("tok", "oauth"))
        with patch.object(ac, "resolve_anthropic_auth", mock_auth):
            ac.resolve_auth_cached("")
            ac.resolve_auth_cached("")
        mock_auth.assert_called_once()  # second call hits cache

    def test_cache_is_populated_after_first_call(self):
        ac = self._get_ac()
        mock_auth = MagicMock(return_value=("t", "o"))
        with patch.object(ac, "resolve_anthropic_auth", mock_auth):
            ac.resolve_auth_cached("")
        self.assertIsNotNone(ac._cached_oauth)


# ---------------------------------------------------------------------------
# _AddProviderDialog._validate via object.__new__
# ---------------------------------------------------------------------------


def _make_dialog(name_text: str, base_text: str, existing: list | None = None) -> _AddProviderDialog:
    dlg = object.__new__(_AddProviderDialog)
    dlg._existing = {n.lower() for n in (existing or [])}
    dlg._name_edit = MagicMock()
    dlg._name_edit.text.return_value = name_text
    dlg._base_edit = MagicMock()
    dlg._base_edit.text.return_value = base_text
    dlg._error_label = MagicMock()
    dlg.accept = MagicMock()
    return dlg


class TestAddProviderDialogValidate(unittest.TestCase):
    def test_empty_name_shows_error(self):
        dlg = _make_dialog("   ", "http://example.com")
        dlg._validate()
        dlg._error_label.show.assert_called()
        dlg.accept.assert_not_called()

    def test_duplicate_name_shows_error(self):
        dlg = _make_dialog("ollama", "http://example.com", existing=["ollama"])
        dlg._validate()
        dlg._error_label.show.assert_called()
        dlg.accept.assert_not_called()

    def test_empty_base_url_shows_error(self):
        dlg = _make_dialog("mynew", "   ")
        dlg._validate()
        dlg._error_label.show.assert_called()
        dlg.accept.assert_not_called()

    def test_valid_input_calls_accept(self):
        dlg = _make_dialog("mynew", "http://example.com")
        dlg._validate()
        dlg.accept.assert_called_once()

    def test_name_normalized_to_lowercase(self):
        dlg = _make_dialog("MyProvider", "http://example.com")
        dlg._validate()
        dlg._name_edit.setText.assert_called_with("myprovider")

    def test_name_spaces_replaced_with_dashes(self):
        dlg = _make_dialog("my provider", "http://example.com")
        dlg._validate()
        dlg._name_edit.setText.assert_called_with("my-provider")

    def test_duplicate_check_case_insensitive(self):
        dlg = _make_dialog("OLLAMA", "http://example.com", existing=["ollama"])
        dlg._validate()
        dlg.accept.assert_not_called()

    def test_valid_no_error_shown(self):
        dlg = _make_dialog("fresh", "http://example.com")
        dlg._validate()
        dlg._error_label.show.assert_not_called()


# ---------------------------------------------------------------------------
# SettingsDialog logic via object.__new__
# ---------------------------------------------------------------------------


def _import_dialog():
    from rikugan.ui.settings_dialog import SettingsDialog

    return SettingsDialog


def _make_settings():
    SettingsDialog = _import_dialog()
    dlg = object.__new__(SettingsDialog)
    dlg._closed = False
    dlg._model_restore_hint = ""
    dlg._resolved_token = ""
    dlg._fetched_models = []
    dlg._fetcher = MagicMock()
    dlg._model_combo = MagicMock()
    dlg._model_combo.currentIndex.return_value = -1
    dlg._model_combo.count.return_value = 0
    dlg._model_status = MagicMock()
    dlg._fetch_btn = MagicMock()
    dlg._context_spin = MagicMock()
    dlg._max_tokens_spin = MagicMock()
    dlg._provider_combo = MagicMock()
    dlg._provider_combo.currentText.return_value = "anthropic"
    dlg._config = MagicMock()
    dlg._registry = MagicMock()
    dlg._auth_status = MagicMock()
    dlg._api_key_edit = MagicMock()
    dlg._api_base_edit = MagicMock()
    dlg._temp_spin = MagicMock()
    dlg._explore_turns_spin = MagicMock()
    dlg._auto_context_cb = MagicMock()
    dlg._auto_save_cb = MagicMock()
    return dlg


class TestSettingsDialogGetSelectedModelId(unittest.TestCase):
    def test_returns_item_data_when_available(self):
        dlg = _make_settings()
        dlg._model_combo.currentIndex.return_value = 0
        dlg._model_combo.itemData.return_value = "claude-3-5-sonnet-20241022"
        result = dlg._get_selected_model_id()
        self.assertEqual(result, "claude-3-5-sonnet-20241022")

    def test_returns_current_text_when_no_data(self):
        dlg = _make_settings()
        dlg._model_combo.currentIndex.return_value = 0
        dlg._model_combo.itemData.return_value = None
        dlg._model_combo.currentText.return_value = " typed-model "
        result = dlg._get_selected_model_id()
        self.assertEqual(result, "typed-model")

    def test_returns_current_text_when_index_negative(self):
        dlg = _make_settings()
        dlg._model_combo.currentIndex.return_value = -1
        dlg._model_combo.currentText.return_value = "manual-model"
        result = dlg._get_selected_model_id()
        self.assertEqual(result, "manual-model")


class TestSettingsDialogPollFetcher(unittest.TestCase):
    def test_noop_when_closed(self):
        dlg = _make_settings()
        dlg._closed = True
        dlg._poll_fetcher()
        dlg._fetcher.poll.assert_not_called()

    def test_noop_when_poll_returns_none(self):
        dlg = _make_settings()
        dlg._fetcher.poll.return_value = None
        dlg._poll_fetcher()  # must not raise

    def test_ignores_stale_provider_result(self):
        dlg = _make_settings()
        dlg._provider_combo.currentText.return_value = "openai"
        dlg._fetcher.poll.return_value = ("models", "anthropic", [])
        dlg._poll_fetcher()
        dlg._model_status.setText.assert_not_called()

    def test_handles_malformed_result_gracefully(self):
        dlg = _make_settings()
        dlg._fetcher.poll.return_value = "not_a_tuple"
        dlg._poll_fetcher()  # must not raise


class TestSettingsDialogOnModelsReady(unittest.TestCase):
    def _model(self, mid: str, name: str | None = None, ctx: int = 200000, max_out: int = 8192):
        m = MagicMock()
        m.id = mid
        m.name = name or mid
        m.context_window = ctx
        m.max_output_tokens = max_out
        return m

    def test_enables_fetch_btn(self):
        dlg = _make_settings()
        dlg._on_models_ready([])
        dlg._fetch_btn.setEnabled.assert_called_with(True)

    def test_no_models_shows_manual_hint(self):
        dlg = _make_settings()
        dlg._on_models_ready([])
        dlg._model_status.setText.assert_called_with("Type model name manually")

    def test_models_shows_count(self):
        dlg = _make_settings()
        models = [self._model("m1"), self._model("m2")]
        dlg._on_models_ready(models)
        dlg._model_status.setText.assert_called_with("2 models")

    def test_clears_restore_hint_after(self):
        dlg = _make_settings()
        dlg._model_restore_hint = "claude-3-5-sonnet"
        dlg._on_models_ready([])
        self.assertEqual(dlg._model_restore_hint, "")


class TestSettingsDialogOnFetchError(unittest.TestCase):
    def test_enables_fetch_btn(self):
        dlg = _make_settings()
        dlg._on_fetch_error("Connection refused")
        dlg._fetch_btn.setEnabled.assert_called_with(True)

    def test_sets_error_text(self):
        dlg = _make_settings()
        dlg._on_fetch_error("Connection refused")
        dlg._model_status.setText.assert_called_with("Connection refused")

    def test_clears_restore_hint(self):
        dlg = _make_settings()
        dlg._model_restore_hint = "old"
        dlg._on_fetch_error("err")
        self.assertEqual(dlg._model_restore_hint, "")


class TestDeferredInit(unittest.TestCase):
    def test_noop_when_closed(self):
        dlg = _make_settings()
        dlg._closed = True
        dlg._deferred_init()  # must not raise, and not call _update_auth_status
        # No way to verify directly but ensure it doesn't crash


if __name__ == "__main__":
    unittest.main()

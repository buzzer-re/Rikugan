"""Tests for OllamaProvider and OpenAICompatProvider."""

from __future__ import annotations

import json
import os
import sys
import unittest
from unittest.mock import MagicMock, patch, PropertyMock

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(__file__))))
from tests.mocks.ida_mock import install_ida_mocks
install_ida_mocks()

from rikugan.providers.ollama_provider import OllamaProvider, DEFAULT_OLLAMA_URL
from rikugan.providers.openai_compat import OpenAICompatProvider
from rikugan.core.types import ModelInfo


# ---------------------------------------------------------------------------
# OllamaProvider
# ---------------------------------------------------------------------------

class TestOllamaProviderInit(unittest.TestCase):
    def test_defaults(self):
        p = OllamaProvider()
        assert p.api_key == "ollama"
        assert p.api_base == DEFAULT_OLLAMA_URL
        assert p.model == "llama3.1"
        assert p.name == "ollama"

    def test_custom_base_url(self):
        p = OllamaProvider(api_base="http://remote:11434/v1")
        assert p.api_base == "http://remote:11434/v1"

    def test_env_base_url(self):
        with patch.dict(os.environ, {"OLLAMA_BASE_URL": "http://env-host:11434/v1"}):
            p = OllamaProvider()
        assert p.api_base == "http://env-host:11434/v1"

    def test_explicit_key_overrides_default(self):
        p = OllamaProvider(api_key="custom-key")
        assert p.api_key == "custom-key"

    def test_empty_key_falls_back_to_ollama(self):
        p = OllamaProvider(api_key="")
        assert p.api_key == "ollama"

    def test_auth_status(self):
        label, status = OllamaProvider().auth_status()
        assert label == "Local"
        assert status == "ok"

    def test_capabilities(self):
        caps = OllamaProvider().capabilities
        assert caps.streaming is True
        assert caps.tool_use is True
        assert caps.vision is False
        assert caps.max_context_window > 0


class TestOllamaListModels(unittest.TestCase):
    def _make_mock_response(self, data: dict):
        resp = MagicMock()
        resp.read.return_value = json.dumps(data).encode()
        resp.__enter__ = lambda s: s
        resp.__exit__ = MagicMock(return_value=False)
        return resp

    def test_list_models_success(self):
        payload = {"models": [{"name": "llama3.1"}, {"name": "mistral"}]}
        mock_resp = self._make_mock_response(payload)
        with patch("urllib.request.urlopen", return_value=mock_resp):
            models = OllamaProvider().list_models()
        assert len(models) == 2
        assert models[0].id == "llama3.1"
        assert models[1].id == "mistral"
        assert all(m.provider == "ollama" for m in models)

    def test_list_models_url_construction(self):
        """Verifies /v1 suffix is stripped before appending /api/tags."""
        payload = {"models": [{"name": "phi3"}]}
        mock_resp = self._make_mock_response(payload)
        captured = {}
        def fake_urlopen(url, timeout=None):
            captured["url"] = url
            return mock_resp
        with patch("urllib.request.urlopen", fake_urlopen):
            OllamaProvider(api_base="http://localhost:11434/v1").list_models()
        assert captured["url"] == "http://localhost:11434/api/tags"

    def test_list_models_network_error_falls_back(self):
        with patch("urllib.request.urlopen", side_effect=OSError("connection refused")):
            models = OllamaProvider(model="custom-model").list_models()
        assert len(models) == 1
        assert models[0].id == "custom-model"

    def test_list_models_empty_response(self):
        mock_resp = self._make_mock_response({"models": []})
        with patch("urllib.request.urlopen", return_value=mock_resp):
            models = OllamaProvider().list_models()
        assert models == []

    def test_list_models_missing_key_falls_back(self):
        """Response without 'models' key falls back to current model."""
        mock_resp = self._make_mock_response({})
        with patch("urllib.request.urlopen", return_value=mock_resp):
            models = OllamaProvider(model="fallback-model").list_models()
        assert models == []


# ---------------------------------------------------------------------------
# OpenAICompatProvider
# ---------------------------------------------------------------------------

class TestOpenAICompatProvider(unittest.TestCase):
    def test_name_default(self):
        p = OpenAICompatProvider()
        assert p.name == "openai_compat"

    def test_custom_provider_name(self):
        p = OpenAICompatProvider(provider_name="together")
        assert p.name == "together"

    def test_custom_base_url(self):
        p = OpenAICompatProvider(api_base="https://api.together.xyz/v1")
        assert p.api_base == "https://api.together.xyz/v1"

    def test_capabilities(self):
        caps = OpenAICompatProvider().capabilities
        assert caps.streaming is True
        assert caps.tool_use is True

    def test_list_models_uses_current_model_when_endpoint_fails(self):
        p = OpenAICompatProvider(model="my-model", api_key="k")
        mock_client = MagicMock()
        mock_client.models.list.side_effect = Exception("not available")
        with patch.object(p, "_get_client", return_value=mock_client):
            models = p.list_models()
        assert len(models) == 1
        assert models[0].id == "my-model"

    def test_list_models_empty_when_no_model_set(self):
        p = OpenAICompatProvider(model="", api_key="k")
        mock_client = MagicMock()
        mock_client.models.list.side_effect = Exception("not available")
        with patch.object(p, "_get_client", return_value=mock_client):
            models = p.list_models()
        assert models == []

    def test_get_client_raises_without_openai(self):
        from rikugan.core.errors import ProviderError
        p = OpenAICompatProvider(api_key="k", api_base="http://localhost/v1")
        with patch("importlib.import_module", side_effect=ImportError("no openai")):
            with self.assertRaises(ProviderError):
                p._get_client()


if __name__ == "__main__":
    unittest.main()

"""Ollama provider adapter (local models via OpenAI-compat API)."""

from __future__ import annotations

import json
import os
import urllib.request

from ..core.logging import log_debug
from ..core.types import ModelInfo, ProviderCapabilities
from .openai_compat import OpenAICompatProvider

DEFAULT_OLLAMA_URL = "http://localhost:11434/v1"


class OllamaProvider(OpenAICompatProvider):
    """Adapter for Ollama's OpenAI-compatible API."""

    def __init__(
        self,
        api_key: str = "ollama",
        api_base: str = "",
        model: str = "llama3.1",
        **kwargs,
    ):
        api_key = api_key or "ollama"
        api_base = api_base or os.environ.get("OLLAMA_BASE_URL", DEFAULT_OLLAMA_URL)
        super().__init__(
            api_key=api_key,
            api_base=api_base,
            model=model,
            provider_name="ollama",
            **kwargs,
        )

    @property
    def name(self) -> str:
        return "ollama"

    def auth_status(self) -> tuple[str, str]:
        return "Local", "ok"

    @property
    def capabilities(self) -> ProviderCapabilities:
        return ProviderCapabilities(
            streaming=True,
            tool_use=True,
            vision=False,
            max_context_window=200000,
            max_output_tokens=16384,
        )

    def list_models(self) -> list[ModelInfo]:
        """Try to list models from the Ollama API."""
        try:
            base = self.api_base.removesuffix("/v1").rstrip("/")
            url = f"{base}/api/tags"
            with urllib.request.urlopen(url, timeout=5) as resp:
                data = json.loads(resp.read())
            models = []
            for m in data.get("models", []):
                name = m.get("name", "unknown")
                models.append(ModelInfo(name, name, "ollama"))
            return models
        except Exception as exc:
            log_debug(f"Ollama list_models failed, using fallback: {exc}")
            return [ModelInfo(self.model, self.model, "ollama")]

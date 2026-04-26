"""OpenAI-compatible provider for third-party endpoints (e.g. Together, Groq, vLLM)."""

from __future__ import annotations

import importlib
from typing import Any

from ..core.errors import ProviderError
from ..core.logging import log_debug
from ..core.types import ModelInfo, ProviderCapabilities
from .openai_provider import OpenAIProvider


class OpenAICompatProvider(OpenAIProvider):
    """Provider that speaks the OpenAI API protocol against a custom base URL."""

    def __init__(
        self,
        api_key: str = "",
        api_base: str = "",
        model: str = "",
        provider_name: str = "openai_compat",
        **kwargs,
    ):
        super().__init__(api_key=api_key, model=model, **kwargs)
        self.api_base = api_base
        self._provider_name = provider_name

    def _get_client(self):
        if self._client is None:
            try:
                openai = importlib.import_module("openai")
            except ImportError as exc:
                raise ProviderError(
                    "openai package not installed. Run: pip install openai",
                    provider=self._provider_name,
                ) from exc
            kwargs: dict[str, Any] = {}
            if self.api_key:
                kwargs["api_key"] = self.api_key
            elif self.api_base:
                # Custom endpoint without explicit key — use a placeholder
                # to prevent the SDK from reading OPENAI_API_KEY env var
                kwargs["api_key"] = "no-key"
            if self.api_base:
                kwargs["base_url"] = self.api_base
            self._client = openai.OpenAI(**kwargs)
        return self._client

    @property
    def name(self) -> str:
        return self._provider_name

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
        """Fetch models from the OpenAI-compatible endpoint."""
        try:
            client = self._get_client()
            response = client.models.list()
            models = []
            for m in response.data:
                name = getattr(m, "name", None) or m.id
                models.append(
                    ModelInfo(
                        id=m.id,
                        name=name,
                        provider=self._provider_name,
                    )
                )
            if models:
                models.sort(key=lambda x: x.id)
                return models
        except Exception as e:
            log_debug(f"list_models for {self._provider_name!r} failed: {e}")
        # Endpoint doesn't support /v1/models or returned nothing.
        # Return current model if set; otherwise empty (user types manually).
        if self.model:
            return [ModelInfo(self.model, self.model, self._provider_name)]
        return []

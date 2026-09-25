"""Cross-provider tests: builtin models, capabilities, shared adapter contracts."""

from __future__ import annotations

import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(__file__)))
from tests.mocks.ida_mock import install_ida_mocks

install_ida_mocks()


def _reload_anthropic_provider_module() -> None:
    """Force the real provider module to load, not leftover test stubs."""
    sys.modules.pop("rikugan.providers.anthropic_provider", None)
    sys.modules.pop("rikugan.core.types", None)


class TestBuiltinModels(unittest.TestCase):
    """All providers must declare non-empty builtin model lists."""

    def test_anthropic_builtin_models(self):
        _reload_anthropic_provider_module()
        from rikugan.providers.anthropic_provider import AnthropicProvider

        p = AnthropicProvider(api_key="test", model="test")
        models = p._builtin_models()
        self.assertTrue(len(models) > 0)
        for m in models:
            self.assertEqual(m.provider, "anthropic")
            self.assertTrue(m.context_window > 0)

    def test_openai_builtin_models(self):
        from rikugan.providers.openai_provider import OpenAIProvider

        p = OpenAIProvider(api_key="test", model="test")
        models = p._builtin_models()
        self.assertTrue(len(models) > 0)
        for m in models:
            self.assertEqual(m.provider, "openai")

    def test_codex_has_no_offline_defaults(self):
        # Codex intentionally has no builtin/fallback list: the model list is
        # always resolved live (list_models -> _fetch_models_live) so a failed
        # fetch surfaces as an error instead of placeholder models.
        from rikugan.providers.codex_provider import CodexProvider

        self.assertEqual(CodexProvider._builtin_models(), [])
        # Codex overrides list_models so the base builtin-fallback path is bypassed.
        self.assertIsNot(
            CodexProvider.list_models,
            CodexProvider.__mro__[1].list_models,  # base LLMProvider.list_models
        )

    def test_gemini_builtin_models(self):
        from rikugan.providers.gemini_provider import GeminiProvider

        models = GeminiProvider._builtin_models()
        self.assertTrue(len(models) > 0)
        for m in models:
            self.assertEqual(m.provider, "gemini")
            self.assertTrue(m.context_window > 0)


class TestProviderCapabilities(unittest.TestCase):
    """All providers must declare streaming and tool_use capabilities."""

    def test_anthropic_capabilities(self):
        _reload_anthropic_provider_module()
        from rikugan.providers.anthropic_provider import AnthropicProvider

        p = AnthropicProvider(api_key="test", model="test")
        caps = p.capabilities
        self.assertTrue(caps.streaming)
        self.assertTrue(caps.tool_use)
        self.assertTrue(caps.vision)

    def test_openai_capabilities(self):
        from rikugan.providers.openai_provider import OpenAIProvider

        p = OpenAIProvider(api_key="test", model="test")
        caps = p.capabilities
        self.assertTrue(caps.streaming)
        self.assertTrue(caps.tool_use)

    def test_gemini_capabilities(self):
        from rikugan.providers.gemini_provider import GeminiProvider

        p = GeminiProvider(api_key="test", model="test")
        caps = p.capabilities
        self.assertTrue(caps.streaming)
        self.assertTrue(caps.tool_use)


class TestProviderRequestDefaults(unittest.TestCase):
    """Providers own request defaults; user-facing generation knobs stay out."""

    def test_openai_omits_generation_knobs(self):
        from rikugan.core.types import Message, Role
        from rikugan.providers.openai_provider import OpenAIProvider

        p = OpenAIProvider(api_key="test", model="gpt-4o")
        kwargs = p._build_request_kwargs([Message(role=Role.USER, content="hi")], tools=None, system="")

        self.assertNotIn("temperature", kwargs)
        self.assertNotIn("max_tokens", kwargs)
        self.assertNotIn("max_completion_tokens", kwargs)

    def test_anthropic_keeps_required_provider_max_tokens(self):
        _reload_anthropic_provider_module()
        from rikugan.core.types import Message, Role
        from rikugan.providers.anthropic_provider import AnthropicProvider

        # Anthropic requires max_tokens on every request, unlike OpenAI.
        p = AnthropicProvider(api_key="test", model="claude-opus-4-7")
        kwargs = p._build_request_kwargs([Message(role=Role.USER, content="hi")], tools=None, system="")

        self.assertEqual(kwargs["max_tokens"], AnthropicProvider._model_limits("claude-opus-4-7")[1])
        self.assertNotIn("temperature", kwargs)


if __name__ == "__main__":
    unittest.main()

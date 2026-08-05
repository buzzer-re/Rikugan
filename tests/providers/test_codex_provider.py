"""Tests for Codex OAuth auth file handling and request auth headers."""

from __future__ import annotations

import base64
import json
import os
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from rikugan.core.errors import AuthenticationError
from rikugan.core.types import Message, Role
from rikugan.providers.codex_provider import (
    CODEX_MODELS_CLIENT_VERSION,
    CodexProvider,
    _codex_models_client_version,
    _id_token_info,
    _version_tuple,
    codex_auth_status,
)


def _jwt(claims: dict) -> str:
    payload = base64.urlsafe_b64encode(json.dumps(claims).encode("utf-8")).decode("ascii").rstrip("=")
    return f"e30.{payload}.sig"


def _write_auth(home: Path, tokens: dict, auth_mode: str = "chatgpt") -> None:
    home.mkdir(parents=True, exist_ok=True)
    (home / "auth.json").write_text(
        json.dumps(
            {
                "auth_mode": auth_mode,
                "OPENAI_API_KEY": None,
                "tokens": tokens,
                "last_refresh": "2026-05-27T00:00:00Z",
            }
        )
    )


class TestCodexAuthFile(unittest.TestCase):
    def test_status_requires_managed_chatgpt_tokens_with_refresh(self):
        with tempfile.TemporaryDirectory() as tmp, patch.dict(os.environ, {"CODEX_HOME": tmp}, clear=False):
            _write_auth(Path(tmp), {"access_token": "access"})

            self.assertEqual(codex_auth_status(), ("Setup required", "error"))
            with self.assertRaises(AuthenticationError):
                CodexProvider().ensure_ready()

    def test_validate_key_fetches_live_models(self):
        with tempfile.TemporaryDirectory() as tmp, patch.dict(os.environ, {"CODEX_HOME": tmp}, clear=False):
            _write_auth(
                Path(tmp),
                {
                    "access_token": "access",
                    "refresh_token": "refresh",
                    "id_token": "bad.jwt",
                    "account_id": "acct",
                },
            )
            provider = CodexProvider()

            with patch.object(
                CodexProvider,
                "_request",
                return_value={"models": [{"slug": "gpt-live", "display_name": "GPT Live", "visibility": "list"}]},
            ) as request:
                self.assertTrue(provider.validate_key())
                request.assert_called_once()

    def test_validate_key_rejects_missing_auth(self):
        with tempfile.TemporaryDirectory() as tmp, patch.dict(os.environ, {"CODEX_HOME": tmp}, clear=False):
            self.assertFalse(CodexProvider().validate_key())


class TestCodexHeaders(unittest.TestCase):
    def test_headers_use_codex_originator_and_account_id(self):
        id_token = _jwt(
            {
                "https://api.openai.com/auth": {
                    "chatgpt_account_id": "acct-123",
                    "chatgpt_account_is_fedramp": True,
                }
            }
        )
        with tempfile.TemporaryDirectory() as tmp, patch.dict(os.environ, {"CODEX_HOME": tmp}, clear=False):
            _write_auth(Path(tmp), {"access_token": "access", "refresh_token": "refresh", "id_token": id_token})

            headers = CodexProvider()._headers()

        self.assertEqual(headers["Authorization"], "Bearer access")
        self.assertEqual(headers["originator"], "codex_cli_rs")
        self.assertTrue(headers["User-Agent"].startswith("codex_cli_rs/"))
        self.assertEqual(headers["ChatGPT-Account-ID"], "acct-123")
        self.assertEqual(headers["X-OpenAI-Fedramp"], "true")

    def test_headers_fall_back_to_access_token_account_id(self):
        access_token = _jwt({"https://api.openai.com/auth": {"chatgpt_account_id": "acct-from-access"}})
        with tempfile.TemporaryDirectory() as tmp, patch.dict(os.environ, {"CODEX_HOME": tmp}, clear=False):
            _write_auth(
                Path(tmp),
                {
                    "access_token": access_token,
                    "refresh_token": "refresh",
                    "id_token": "malformed",
                },
            )

            headers = CodexProvider()._headers()

        self.assertEqual(headers["ChatGPT-Account-ID"], "acct-from-access")

    def test_malformed_jwt_payload_is_ignored(self):
        self.assertIsNone(_id_token_info("not-a-jwt")["chatgpt_account_id"])


class TestCodexModels(unittest.TestCase):
    def test_fetch_models_live_uses_codex_models_endpoint(self):
        with tempfile.TemporaryDirectory() as tmp, patch.dict(os.environ, {"CODEX_HOME": tmp}, clear=False):
            home = Path(tmp)
            _write_auth(home, {"access_token": "access", "refresh_token": "refresh", "account_id": "acct"})
            (home / "models_cache.json").write_text(json.dumps({"client_version": "0.150.0"}))
            provider = CodexProvider()

            with patch.object(
                CodexProvider,
                "_request",
                return_value={
                    "models": [
                        {
                            "slug": "gpt-5.5",
                            "display_name": "GPT-5.5",
                            "visibility": "list",
                            "context_window": 272000,
                            "supports_parallel_tool_calls": True,
                            "input_modalities": ["text", "image"],
                        },
                        {
                            "slug": "codex-auto-review",
                            "display_name": "Codex Auto Review",
                            "visibility": "hide",
                        },
                    ]
                },
            ) as request:
                models = provider._fetch_models_live()

        self.assertEqual([m.id for m in models], ["gpt-5.5"])
        self.assertEqual(models[0].name, "GPT-5.5")
        self.assertEqual(models[0].context_window, 272000)
        self.assertTrue(models[0].supports_tools)
        self.assertTrue(models[0].supports_vision)
        request.assert_called_once_with("GET", "models?client_version=0.150.0", None, stream=False)


class TestCodexClientVersion(unittest.TestCase):
    """The models endpoint gates availability on client_version — never report a stale one."""

    def _resolve(self, files: dict[str, dict]) -> str:
        with tempfile.TemporaryDirectory() as tmp, patch.dict(os.environ, {"CODEX_HOME": tmp}, clear=False):
            for name, payload in files.items():
                (Path(tmp) / name).write_text(json.dumps(payload))
            return _codex_models_client_version()

    def test_stale_models_cache_does_not_mask_newer_version(self):
        """The real-world case: an old cache alongside an updated CLI."""
        resolved = self._resolve(
            {
                "models_cache.json": {"client_version": "0.134.0"},
                "version.json": {"latest_version": "0.146.0"},
            }
        )
        self.assertEqual(resolved, "0.146.0")

    def test_newer_cache_wins_over_version_file(self):
        resolved = self._resolve(
            {
                "models_cache.json": {"client_version": "0.152.0"},
                "version.json": {"latest_version": "0.146.0"},
            }
        )
        self.assertEqual(resolved, "0.152.0")

    def test_builtin_floor_used_when_local_versions_are_older(self):
        resolved = self._resolve({"models_cache.json": {"client_version": "0.100.0"}})
        self.assertEqual(resolved, CODEX_MODELS_CLIENT_VERSION)

    def test_missing_files_fall_back_to_builtin(self):
        self.assertEqual(self._resolve({}), CODEX_MODELS_CLIENT_VERSION)

    def test_malformed_json_is_ignored(self):
        with tempfile.TemporaryDirectory() as tmp, patch.dict(os.environ, {"CODEX_HOME": tmp}, clear=False):
            (Path(tmp) / "models_cache.json").write_text("{not json")
            self.assertEqual(_codex_models_client_version(), CODEX_MODELS_CLIENT_VERSION)

    def test_numeric_not_lexicographic_ordering(self):
        """String comparison would rank "0.9.0" above "0.146.0"."""
        self.assertEqual(max(["0.9.0", "0.146.0"], key=_version_tuple), "0.146.0")

    def test_prerelease_does_not_outrank_its_release(self):
        """A -beta/rc suffix must never resolve higher than the plain release."""
        for prerelease in ("0.146.0-beta.1", "0.146.0rc1", "0.146.0+build.7"):
            with self.subTest(prerelease=prerelease):
                self.assertLessEqual(_version_tuple(prerelease), _version_tuple("0.146.0"))

    def test_leading_v_is_tolerated(self):
        self.assertEqual(_version_tuple("v0.146.0"), _version_tuple("0.146.0"))

    def test_unparsable_versions_never_win(self):
        for junk in ("", "unknown", "not.a.version"):
            with self.subTest(junk=junk):
                self.assertLess(_version_tuple(junk), _version_tuple("0.1.0"))


class TestCodexRequestPayload(unittest.TestCase):
    def test_omits_generation_knobs_and_empty_tool_fields(self):
        provider = CodexProvider(model="gpt-5-codex")

        kwargs = provider._build_request_kwargs([Message(role=Role.USER, content="hi")], tools=None, system="")

        self.assertNotIn("temperature", kwargs)
        self.assertNotIn("max_output_tokens", kwargs)
        self.assertNotIn("tools", kwargs)
        self.assertNotIn("tool_choice", kwargs)
        self.assertNotIn("parallel_tool_calls", kwargs)
        self.assertNotIn("instructions", kwargs)

    def test_includes_tool_fields_only_with_tools(self):
        provider = CodexProvider(model="gpt-5-codex")
        tools = [
            {
                "type": "function",
                "function": {
                    "name": "lookup",
                    "description": "Lookup a value",
                    "parameters": {"type": "object", "properties": {}},
                },
            }
        ]

        kwargs = provider._build_request_kwargs([Message(role=Role.USER, content="hi")], tools=tools, system="sys")

        self.assertEqual(kwargs["instructions"], "sys")
        self.assertEqual(kwargs["tool_choice"], "auto")
        self.assertTrue(kwargs["parallel_tool_calls"])
        self.assertEqual(kwargs["tools"][0]["name"], "lookup")


if __name__ == "__main__":
    unittest.main()

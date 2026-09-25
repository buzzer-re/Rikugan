"""Anthropic Claude provider adapter with OAuth token support."""

from __future__ import annotations

import importlib
import json
import os
import subprocess
import sys
from collections.abc import Generator
from typing import Any, NoReturn

from ..core.errors import (
    AuthenticationError,
    ContextLengthError,
    ProviderError,
    RateLimitError,
)
from ..core.logging import log_debug, log_error
from ..core.types import (
    Message,
    ModelInfo,
    ProviderCapabilities,
    Role,
    StreamChunk,
    TokenUsage,
    ToolCall,
)
from .base import LLMProvider


def _read_oauth_from_keychain() -> str | None:
    """Read the Claude OAuth access token from macOS Keychain.

    `claude setup-token` stores credentials under "Claude Code-credentials"
    as JSON: {"claudeAiOauth": {"accessToken": "sk-ant-oat01-...", ...}}
    """
    if sys.platform != "darwin":
        return None
    try:
        result = subprocess.run(
            [
                "security",
                "find-generic-password",
                "-s",
                "Claude Code-credentials",
                "-w",
            ],
            capture_output=True,
            text=True,
            timeout=5,
        )
        if result.returncode != 0:
            return None
        data = json.loads(result.stdout.strip())
        return data.get("claudeAiOauth", {}).get("accessToken")
    except Exception:
        return None


def resolve_anthropic_auth(
    api_key: str = "",
    allow_keychain: bool = True,
) -> tuple[str, str]:
    """Resolve the best available Anthropic credential.

    Returns (token, auth_type) where auth_type is "api_key" or "oauth".
    Priority:
      1. Explicit api_key argument
      2. ANTHROPIC_API_KEY env var
      3. CLAUDE_CODE_OAUTH_TOKEN env var
      4. OAuth token from macOS Keychain (requires *allow_keychain*)
    """
    # Explicit key or env var
    key = api_key or os.environ.get("ANTHROPIC_API_KEY", "")
    if key:
        if key.startswith("sk-ant-oat"):
            return key, "oauth"
        return key, "api_key"

    # CLAUDE_CODE_OAUTH_TOKEN env var (claude setup-token pattern)
    oauth_env = os.environ.get("CLAUDE_CODE_OAUTH_TOKEN", "")
    if oauth_env:
        return oauth_env, "oauth"

    # macOS Keychain — only if the user has accepted the OAuth consent
    if allow_keychain:
        oauth = _read_oauth_from_keychain()
        if oauth:
            return oauth, "oauth"

    return "", ""


class AnthropicProvider(LLMProvider):
    """Adapter for the Anthropic Messages API.

    Supports both API key and OAuth token authentication.
    OAuth tokens (from `claude setup-token`) are auto-detected from
    the macOS Keychain when no explicit API key is provided.
    """

    def __init__(
        self,
        api_key: str = "",
        api_base: str = "",
        model: str = "claude-sonnet-4-20250514",
        **kwargs,
    ):
        if api_key:
            token, self._auth_type = resolve_anthropic_auth(api_key)
        else:
            # Go through the cache, which respects OAuth consent.
            from .auth_cache import resolve_auth_cached

            token, self._auth_type = resolve_auth_cached()
        super().__init__(api_key=token, api_base=api_base, model=model)

    def _get_client(self):
        if self._client is None:
            try:
                anthropic = importlib.import_module("anthropic")
            except ImportError as exc:
                raise ProviderError(
                    "anthropic package not installed. Run: pip install anthropic",
                    provider="anthropic",
                ) from exc
            if not self.api_key:
                raise AuthenticationError(
                    "No Anthropic credential found. Set ANTHROPIC_API_KEY, "
                    "paste a key in settings, or run `claude setup-token`.",
                    provider="anthropic",
                )
            # OAuth tokens use Bearer auth + beta header;
            # API keys use x-api-key header.
            kwargs: dict[str, Any] = {}
            if self.api_base:
                kwargs["base_url"] = self.api_base
            kwargs["timeout"] = 120.0  # 2min vs SDK default 10min
            if self._auth_type == "oauth":
                kwargs["auth_token"] = self.api_key
                kwargs["default_headers"] = {
                    "anthropic-beta": "oauth-2025-04-20,claude-code-20250219",
                }
                self._client = anthropic.Anthropic(**kwargs)
            else:
                kwargs["api_key"] = self.api_key
                self._client = anthropic.Anthropic(**kwargs)
        return self._client

    @property
    def name(self) -> str:
        return "anthropic"

    @property
    def auth_type(self) -> str:
        return self._auth_type

    def auth_status(self) -> tuple[str, str]:
        if self.api_key:
            if self._auth_type == "oauth":
                return "OAuth", "ok"
            return "API Key", "ok"
        return "No key", "error"

    @property
    def capabilities(self) -> ProviderCapabilities:
        ctx, max_output = self._model_limits(self.model)
        return ProviderCapabilities(
            streaming=True,
            tool_use=True,
            vision=True,
            max_context_window=ctx,
            max_output_tokens=max_output,
            supports_system_prompt=True,
            supports_cache_control=True,
        )

    @staticmethod
    def _model_limits(model_id: str) -> tuple[int, int]:
        """Return conservative provider-owned context/output limits."""
        model = model_id.lower()
        if "sonnet-4" in model or "3-7-sonnet" in model:
            return 200000, 64000
        if "opus-4" in model:
            return 200000, 32000
        if "haiku-3" in model:
            return 200000, 8192
        return 200000, 8192

    def context_window(self) -> int:
        return self._model_limits(self.model)[0]

    def _default_max_tokens(self) -> int:
        return self._model_limits(self.model)[1]

    def _fetch_models_live(self) -> list[ModelInfo]:
        """Fetch models from the Anthropic API."""
        client = self._get_client()
        response = client.models.list(limit=100)
        models = []
        for m in response.data:
            model_id = m.id
            display_name = getattr(m, "display_name", model_id)
            # API doesn't return context/output limits; use known defaults
            ctx_window, max_output = self._model_limits(model_id)
            models.append(
                ModelInfo(
                    id=model_id,
                    name=display_name,
                    provider="anthropic",
                    context_window=ctx_window,
                    max_output_tokens=max_output,
                    supports_tools=True,
                    supports_vision=True,
                )
            )
        # Sort: newest/best first
        models.sort(key=lambda m: m.id, reverse=True)
        return models if models else self._builtin_models()

    @staticmethod
    def _builtin_models() -> list[ModelInfo]:
        return [
            ModelInfo(
                "claude-opus-4-7",
                "Claude Opus 4.7",
                "anthropic",
                200000,
                32000,
                True,
                True,
            ),
            ModelInfo(
                "claude-sonnet-4-6",
                "Claude Sonnet 4.6",
                "anthropic",
                200000,
                64000,
                True,
                True,
            ),
            ModelInfo(
                "claude-opus-4-6",
                "Claude Opus 4.6",
                "anthropic",
                200000,
                32000,
                True,
                True,
            ),
            ModelInfo(
                "claude-opus-4-20250514",
                "Claude Opus 4",
                "anthropic",
                200000,
                32000,
                True,
                True,
            ),
            ModelInfo(
                "claude-sonnet-4-20250514",
                "Claude Sonnet 4",
                "anthropic",
                200000,
                64000,
                True,
                True,
            ),
            ModelInfo(
                "claude-haiku-4-5-20251001",
                "Claude Haiku 4.5",
                "anthropic",
                200000,
                8192,
                True,
                True,
            ),
        ]

    def _format_messages(self, messages: list[Message]) -> list[dict[str, Any]]:
        """Convert to Anthropic's messages format."""
        formatted = []
        for msg in messages:
            if msg.role == Role.SYSTEM:
                continue  # System goes in the `system` param

            if msg.role == Role.USER:
                formatted.append({"role": "user", "content": msg.content})

            elif msg.role == Role.ASSISTANT:
                content: list = []
                if msg.content:
                    content.append({"type": "text", "text": msg.content})
                for tc in msg.tool_calls:
                    content.append(
                        {
                            "type": "tool_use",
                            "id": tc.id,
                            "name": tc.name,
                            "input": tc.arguments,
                        }
                    )
                formatted.append(
                    {"role": "assistant", "content": content or msg.content}  # type: ignore[dict-item]
                )

            elif msg.role == Role.TOOL:
                for tr in msg.tool_results:
                    formatted.append(
                        {
                            "role": "user",
                            "content": [  # type: ignore[dict-item]
                                {
                                    "type": "tool_result",
                                    "tool_use_id": tr.tool_call_id,
                                    "content": tr.content,
                                    "is_error": tr.is_error,
                                }
                            ],
                        }
                    )

        return formatted

    def _format_tools(self, tools: list[dict[str, Any]]) -> list[dict[str, Any]]:
        """Convert OpenAI-style tool schemas to Anthropic format."""
        anthropic_tools = []
        for t in tools:
            func = t.get("function", t)
            anthropic_tools.append(
                {
                    "name": func["name"],
                    "description": func.get("description", ""),
                    "input_schema": func.get("parameters", {"type": "object", "properties": {}}),
                }
            )
        return anthropic_tools

    def _normalize_response(self, response) -> Message:
        """Convert Anthropic response to internal Message."""
        content_text = ""
        tool_calls = []

        for block in response.content:
            if block.type == "thinking":
                content_text += f"<think>{block.thinking}</think>\n"
            elif block.type == "text":
                content_text += block.text
            elif block.type == "tool_use":
                tool_calls.append(
                    ToolCall(
                        id=block.id,
                        name=block.name,
                        arguments=block.input if isinstance(block.input, dict) else json.loads(block.input),
                    )
                )

        usage = TokenUsage(
            prompt_tokens=response.usage.input_tokens,
            completion_tokens=response.usage.output_tokens,
            total_tokens=response.usage.input_tokens + response.usage.output_tokens,
            cache_read_tokens=getattr(response.usage, "cache_read_input_tokens", 0) or 0,
            cache_creation_tokens=getattr(response.usage, "cache_creation_input_tokens", 0) or 0,
        )

        return Message(
            role=Role.ASSISTANT,
            content=content_text,
            tool_calls=tool_calls,
            token_usage=usage,
        )

    # A tool set this large is worth naming when the API turns a request away:
    # it is re-sent every turn and is otherwise invisible from the message.
    # A tool set this large is worth naming as a contributing factor, though
    # it is not on its own why a request is turned away.
    _BIG_TOOL_SET = 80

    _USAGE_WORDS = ("usage", "limit", "credit", "balance", "quota")

    def _quota_rejection_hint(self, message: str) -> str:
        """Explain a rejection the message itself does not explain.

        Anthropic answers a request it will not bill with a note about extra
        usage and a link to buy more. On a subscription token that is the whole
        story: the account is at its cap, and no amount of trimming on our side
        adds allowance. Say that, and name the way round it, rather than
        leaving it to look like a bug in Rikugan.

        A large tool set does decide which side of the line a given request
        lands on, so it is mentioned — as a contributing factor, not the cause.
        """
        lowered = message.lower()
        if not any(word in lowered for word in self._USAGE_WORDS):
            return ""
        if self._auth_type != "oauth":
            return ""
        count, size = getattr(self, "_last_tool_payload", (0, 0))
        payload = ""
        if count >= self._BIG_TOOL_SET:
            payload = (
                f" This request declared {count} tools (~{size // 1024} KB), re-sent every turn; "
                "a smaller tool set can slip under the cap but does not raise it."
            )
        return (
            "\n\nRikugan is signed in with a Claude subscription token, and this account "
            "has no extra usage left to bill against. Claude Code may keep working on a "
            "separate allowance. An Anthropic API key (Settings \u2192 Providers) bills to "
            f"API credits instead and is not subject to this cap.{payload}"
        )

    def _handle_api_error(self, e: Exception) -> NoReturn:
        """Raise the appropriate Rikugan error from an Anthropic API error."""
        try:
            anthropic = importlib.import_module("anthropic")
        except ImportError:
            raise ProviderError(str(e), provider="anthropic") from e

        if isinstance(e, anthropic.AuthenticationError):
            raise AuthenticationError(provider="anthropic") from e
        if isinstance(e, anthropic.RateLimitError):
            retry_after = 0.0
            # Try to extract retry-after from response headers
            resp = getattr(e, "response", None)
            if resp is not None:
                retry_hdr = getattr(resp, "headers", {}).get("retry-after", "")
                try:
                    retry_after = float(retry_hdr)
                except (ValueError, TypeError) as parse_err:
                    log_debug(f"Could not parse retry-after header {retry_hdr!r}: {parse_err}")
            raise RateLimitError(provider="anthropic", retry_after=retry_after or 5.0) from e
        if isinstance(e, anthropic.BadRequestError):
            msg = str(e)
            if "context" in msg.lower() or "token" in msg.lower():
                raise ContextLengthError(str(e), provider="anthropic") from e
            raise ProviderError(msg + self._quota_rejection_hint(msg), provider="anthropic") from e
        raise ProviderError(str(e), provider="anthropic") from e

    def _build_request_kwargs(
        self,
        messages: list[Message],
        tools: list[dict[str, Any]] | None,
        system: str,
    ) -> dict[str, Any]:
        """Build kwargs dict for messages.create/stream."""
        formatted_messages = self._format_messages(messages)

        kwargs: dict[str, Any] = {
            "model": self.model,
            "messages": formatted_messages,
            "max_tokens": self._default_max_tokens(),
        }

        # System prompt with cache_control for prompt caching
        if system:
            system_blocks: list[dict[str, Any]] = []
            # OAuth billing attribution — required by Anthropic for
            # Claude Code subscription tokens.
            if self._auth_type == "oauth":
                system_blocks.append(
                    {
                        "type": "text",
                        "text": ("x-anthropic-billing-header: cc_version=2.1.77; cc_entrypoint=cli; cch=00000;"),
                    }
                )
            system_blocks.append(
                {
                    "type": "text",
                    "text": system,
                    "cache_control": {"type": "ephemeral"},
                }
            )
            kwargs["system"] = system_blocks

        if tools:
            formatted_tools = self._format_tools(tools)
            self._last_tool_payload = (len(formatted_tools), len(json.dumps(formatted_tools)))
            # Mark the last tool with cache_control so the full tool list is cached
            if formatted_tools:
                formatted_tools[-1]["cache_control"] = {"type": "ephemeral"}
            kwargs["tools"] = formatted_tools

        # Mark the last user message with cache_control to cache conversation history
        # (only if there are enough messages for caching to be worthwhile)
        if len(formatted_messages) >= 4:
            last_msg = formatted_messages[-1]
            if isinstance(last_msg.get("content"), list) and last_msg["content"]:
                last_msg["content"][-1]["cache_control"] = {"type": "ephemeral"}
            elif isinstance(last_msg.get("content"), str):
                # Convert string content to block format for cache_control
                last_msg["content"] = [
                    {
                        "type": "text",
                        "text": last_msg["content"],
                        "cache_control": {"type": "ephemeral"},
                    }
                ]

        return kwargs

    def _call_api(self, client: Any, kwargs: dict[str, Any]) -> Any:
        """Invoke the Anthropic messages.create API."""
        return client.messages.create(**kwargs)

    def _stream_chunks(
        self,
        client: Any,
        kwargs: dict[str, Any],
    ) -> Generator[StreamChunk, None, None]:
        """Yield StreamChunks from the Anthropic streaming API."""
        try:
            with client.messages.stream(**kwargs) as stream:
                current_tool_id = None
                current_tool_name = None
                tool_args_buf = ""

                in_thinking = False

                with self._track_request_handle(stream):
                    for event in stream:
                        etype = event.type

                        if etype == "content_block_start":
                            block = event.content_block
                            if block.type == "tool_use":
                                current_tool_id = block.id
                                current_tool_name = block.name
                                tool_args_buf = ""
                                yield StreamChunk(
                                    tool_call_id=block.id,
                                    tool_name=block.name,
                                    is_tool_call_start=True,
                                )
                            elif block.type == "thinking":
                                in_thinking = True
                                yield StreamChunk(text="<think>\n")
                            elif block.type == "text":
                                if block.text:
                                    yield StreamChunk(text=block.text)

                        elif etype == "content_block_delta":
                            delta = event.delta
                            if delta.type == "thinking_delta":
                                yield StreamChunk(text=delta.thinking)
                            elif delta.type == "text_delta":
                                yield StreamChunk(text=delta.text)
                            elif delta.type == "input_json_delta":
                                tool_args_buf += delta.partial_json
                                yield StreamChunk(
                                    tool_call_id=current_tool_id,
                                    tool_name=current_tool_name,
                                    tool_args_delta=delta.partial_json,
                                )

                        elif etype == "content_block_stop":
                            if in_thinking:
                                yield StreamChunk(text="\n</think>\n")
                                in_thinking = False
                            elif current_tool_id:
                                yield StreamChunk(
                                    tool_call_id=current_tool_id,
                                    tool_name=current_tool_name,
                                    tool_args_delta="",
                                    is_tool_call_end=True,
                                )
                                current_tool_id = None
                                current_tool_name = None
                                tool_args_buf = ""

                        elif etype == "message_delta":
                            sr = getattr(event, "delta", None)
                            if sr and hasattr(sr, "stop_reason"):
                                yield StreamChunk(finish_reason=sr.stop_reason)
                            # Capture final output_tokens from message_delta usage
                            usage_delta = getattr(event, "usage", None)
                            if usage_delta is not None:
                                output_tokens = getattr(usage_delta, "output_tokens", 0) or 0
                                if output_tokens > 0:
                                    yield StreamChunk(
                                        usage=TokenUsage(
                                            prompt_tokens=0,
                                            completion_tokens=output_tokens,
                                        )
                                    )

                        elif etype == "message_start":
                            msg = event.message
                            if hasattr(msg, "usage"):
                                yield StreamChunk(
                                    usage=TokenUsage(
                                        prompt_tokens=msg.usage.input_tokens,
                                        completion_tokens=0,
                                        cache_read_tokens=getattr(msg.usage, "cache_read_input_tokens", 0) or 0,
                                        cache_creation_tokens=getattr(msg.usage, "cache_creation_input_tokens", 0) or 0,
                                    )
                                )

        except Exception as e:
            log_error(f"AnthropicProvider.chat_stream error: {e}")
            self._handle_api_error(e)

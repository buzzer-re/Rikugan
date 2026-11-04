"""OpenAI provider adapter."""

from __future__ import annotations

import importlib
import json
import os
from collections.abc import Generator
from typing import Any, NoReturn

from ..core.errors import (
    AuthenticationError,
    ContextLengthError,
    ProviderError,
    RateLimitError,
)
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


class OpenAIProvider(LLMProvider):
    """Adapter for the OpenAI Chat Completions API."""

    def __init__(self, api_key: str = "", api_base: str = "", model: str = "gpt-4o", **kwargs):
        api_key = api_key or os.environ.get("OPENAI_API_KEY", "")
        super().__init__(api_key=api_key, api_base=api_base, model=model)

    def _get_client(self):
        if self._client is None:
            try:
                openai = importlib.import_module("openai")
            except ImportError as exc:
                raise ProviderError(
                    "openai package not installed. Run: pip install openai",
                    provider="openai",
                ) from exc
            if not self.api_key:
                raise AuthenticationError(provider="openai")
            kwargs = {"api_key": self.api_key, "timeout": 120.0}
            if self.api_base:
                kwargs["base_url"] = self.api_base
            http_client = self._unverified_http_client(openai)
            if http_client is not None:
                kwargs["http_client"] = http_client
            self._client = openai.OpenAI(**kwargs)
        return self._client

    @property
    def name(self) -> str:
        return "openai"

    @property
    def capabilities(self) -> ProviderCapabilities:
        return ProviderCapabilities(
            streaming=True,
            tool_use=True,
            vision=True,
            max_context_window=128000,
            max_output_tokens=16384,
            supports_system_prompt=True,
        )

    def _fetch_models_live(self) -> list[ModelInfo]:
        """Fetch chat-capable models from the OpenAI API."""
        client = self._get_client()
        response = client.models.list()
        models = []
        chat_prefixes = ("gpt-", "o1-", "o3-", "o4-", "chatgpt-")
        skip_words = (
            "-instruct",
            "embedding",
            "tts",
            "whisper",
            "dall-e",
            "audio",
            "realtime",
            "transcribe",
        )
        for m in response.data:
            if not any(m.id.startswith(p) for p in chat_prefixes):
                continue
            if any(s in m.id for s in skip_words):
                continue
            models.append(
                ModelInfo(
                    id=m.id,
                    name=m.id,
                    provider="openai",
                    context_window=128000,
                    max_output_tokens=16384,
                    supports_tools=True,
                    supports_vision=True,
                )
            )
        models.sort(key=lambda m: m.id, reverse=True)
        return models if models else self._builtin_models()

    @staticmethod
    def _builtin_models() -> list[ModelInfo]:
        return [
            ModelInfo("gpt-4o", "GPT-4o", "openai", 128000, 16384, True, True),
            ModelInfo("gpt-4o-mini", "GPT-4o Mini", "openai", 128000, 16384, True, True),
            ModelInfo("o3-mini", "o3-mini", "openai", 200000, 100000, True, False),
        ]

    def _format_messages(self, messages: list[Message]) -> list[dict[str, Any]]:
        formatted = []
        for msg in messages:
            if msg.role == Role.SYSTEM:
                formatted.append({"role": "system", "content": msg.content})
            elif msg.role == Role.USER:
                formatted.append({"role": "user", "content": msg.content})
            elif msg.role == Role.ASSISTANT:
                if not msg.content and not msg.tool_calls:
                    # Would go out as a message with neither content nor a
                    # tool call, which is not a valid turn.
                    continue
                d: dict[str, Any] = {"role": "assistant"}
                if msg.content:
                    d["content"] = msg.content
                if msg.tool_calls:
                    d["tool_calls"] = [
                        {
                            "id": tc.id,
                            "type": "function",
                            "function": {
                                "name": tc.name,
                                "arguments": json.dumps(tc.arguments),
                            },
                        }
                        for tc in msg.tool_calls
                    ]
                formatted.append(d)
            elif msg.role == Role.TOOL:
                for tr in msg.tool_results:
                    formatted.append(
                        {
                            "role": "tool",
                            "tool_call_id": tr.tool_call_id,
                            "content": tr.content,
                        }
                    )
        return formatted

    def _normalize_response(self, response) -> Message:
        choice = response.choices[0]
        rm = choice.message

        tool_calls = []
        if rm.tool_calls:
            for tc in rm.tool_calls:
                tool_calls.append(
                    ToolCall(
                        id=tc.id,
                        name=tc.function.name,
                        arguments=json.loads(tc.function.arguments),
                    )
                )

        usage = TokenUsage()
        if response.usage:
            usage = TokenUsage(
                prompt_tokens=response.usage.prompt_tokens,
                completion_tokens=response.usage.completion_tokens,
                total_tokens=response.usage.total_tokens,
            )

        # OpenAI o-series reasoning_content
        text = rm.content or ""
        reasoning = getattr(rm, "reasoning_content", None)
        if reasoning:
            text = f"<think>{reasoning}</think>\n{text}"

        return Message(
            role=Role.ASSISTANT,
            content=text,
            tool_calls=tool_calls,
            token_usage=usage,
        )

    def _handle_api_error(self, e: Exception) -> NoReturn:
        try:
            openai = importlib.import_module("openai")
        except ImportError:
            raise ProviderError(str(e), provider="openai") from e
        if isinstance(e, openai.AuthenticationError):
            raise AuthenticationError(provider="openai") from e
        if isinstance(e, openai.RateLimitError):
            raise RateLimitError(provider="openai") from e
        if isinstance(e, openai.BadRequestError):
            msg = str(e)
            if "context" in msg.lower() or "token" in msg.lower():
                raise ContextLengthError(msg, provider="openai") from e
        raise ProviderError(str(e), provider="openai") from e

    def _build_request_kwargs(
        self,
        messages: list[Message],
        tools: list[dict[str, Any]] | None,
        system: str,
    ) -> dict[str, Any]:
        """Build kwargs dict for chat.completions.create."""
        msgs = []
        if system:
            msgs.append({"role": "system", "content": system})
        msgs.extend(self._format_messages(messages))

        kwargs: dict[str, Any] = {
            "model": self.model,
            "messages": msgs,
        }
        if tools:
            kwargs["tools"] = tools
        return kwargs

    def _call_api(self, client: Any, kwargs: dict[str, Any]) -> Any:
        """Invoke the OpenAI chat.completions.create API."""
        return client.chat.completions.create(**kwargs)

    def _stream_chunks(
        self,
        client: Any,
        kwargs: dict[str, Any],
    ) -> Generator[StreamChunk, None, None]:
        """Yield StreamChunks from the OpenAI streaming API."""
        kwargs["stream"] = True
        kwargs["stream_options"] = {"include_usage": True}

        try:
            stream = client.chat.completions.create(**kwargs)
            current_tool_calls: dict[int, dict] = {}

            _in_reasoning = False

            with self._track_request_handle(stream):
                for chunk in stream:
                    if not chunk.choices:
                        continue
                    delta = chunk.choices[0].delta

                    # OpenAI o-series reasoning_content
                    reasoning = getattr(delta, "reasoning_content", None)
                    if reasoning:
                        if not _in_reasoning:
                            yield StreamChunk(text="<think>")
                            _in_reasoning = True
                        yield StreamChunk(text=reasoning)
                    elif _in_reasoning:
                        yield StreamChunk(text="</think>\n")
                        _in_reasoning = False

                    if delta.content:
                        yield StreamChunk(text=delta.content)

                    if delta.tool_calls:
                        for tc_delta in delta.tool_calls:
                            idx = tc_delta.index
                            if idx not in current_tool_calls:
                                current_tool_calls[idx] = {
                                    "id": tc_delta.id or "",
                                    "name": tc_delta.function.name
                                    if tc_delta.function and tc_delta.function.name
                                    else "",
                                    "args": "",
                                }
                                if tc_delta.id:
                                    yield StreamChunk(
                                        tool_call_id=tc_delta.id,
                                        tool_name=tc_delta.function.name if tc_delta.function else "",
                                        is_tool_call_start=True,
                                    )

                            if tc_delta.function and tc_delta.function.arguments:
                                current_tool_calls[idx]["args"] += tc_delta.function.arguments
                                yield StreamChunk(
                                    tool_call_id=current_tool_calls[idx]["id"],
                                    tool_name=current_tool_calls[idx]["name"],
                                    tool_args_delta=tc_delta.function.arguments,
                                )

                    if chunk.choices[0].finish_reason:
                        if _in_reasoning:
                            yield StreamChunk(text="</think>\n")
                            _in_reasoning = False
                        for tc_info in current_tool_calls.values():
                            yield StreamChunk(
                                tool_call_id=tc_info["id"],
                                tool_name=tc_info["name"],
                                is_tool_call_end=True,
                            )
                        yield StreamChunk(finish_reason=chunk.choices[0].finish_reason)

                    if chunk.usage:
                        yield StreamChunk(
                            usage=TokenUsage(
                                prompt_tokens=chunk.usage.prompt_tokens,
                                completion_tokens=chunk.usage.completion_tokens,
                                total_tokens=chunk.usage.total_tokens,
                            )
                        )

        except Exception as e:
            self._handle_api_error(e)

"""Codex / ChatGPT provider adapter using Codex OAuth device auth."""

from __future__ import annotations

import base64
import binascii
import json
import os
import time
import urllib.error
import urllib.parse
import urllib.request
from collections.abc import Generator
from dataclasses import dataclass
from pathlib import Path
from typing import Any, NoReturn

from ..core.errors import AuthenticationError, ContextLengthError, ProviderError, RateLimitError
from ..core.tls import ssl_context
from ..core.types import Message, ModelInfo, ProviderCapabilities, Role, StreamChunk, TokenUsage, ToolCall
from .base import LLMProvider

CODEX_CLIENT_ID = "app_EMoamEEZ73f0CkXaXp7hrann"
CODEX_ISSUER = "https://auth.openai.com"
CODEX_BASE_URL = "https://chatgpt.com/backend-api/codex"
CODEX_AUTH_MODE = "chatgpt"
CODEX_ORIGINATOR = "codex_cli_rs"
CODEX_MODELS_CLIENT_VERSION = "0.133.0"


@dataclass
class CodexDeviceCode:
    verification_url: str
    user_code: str
    device_auth_id: str
    interval: int = 5


def codex_home() -> Path:
    raw = os.environ.get("CODEX_HOME", "").strip()
    return Path(raw).expanduser() if raw else Path.home() / ".codex"


def _auth_path() -> Path:
    return codex_home() / "auth.json"


def _models_cache_path() -> Path:
    return codex_home() / "models_cache.json"


def _version_path() -> Path:
    return codex_home() / "version.json"


def _decode_jwt_payload(jwt: str) -> dict[str, Any]:
    parts = jwt.split(".")
    if len(parts) < 2:
        return {}
    payload = parts[1] + "=" * (-len(parts[1]) % 4)
    try:
        return json.loads(base64.urlsafe_b64decode(payload.encode("ascii")).decode("utf-8"))
    except (binascii.Error, UnicodeDecodeError, ValueError, json.JSONDecodeError):
        return {}


def _first_org_id(claims: dict[str, Any]) -> str | None:
    auth = claims.get("https://api.openai.com/auth") or {}
    organizations = auth.get("organizations") or claims.get("organizations") or []
    if not isinstance(organizations, list):
        return None
    for org in organizations:
        if isinstance(org, dict) and org.get("id"):
            return str(org["id"])
    return None


def _id_token_info(id_token: str) -> dict[str, Any]:
    claims = _decode_jwt_payload(id_token)
    auth = claims.get("https://api.openai.com/auth") or {}
    profile = claims.get("https://api.openai.com/profile") or {}
    account_id = (
        auth.get("chatgpt_account_id")
        or claims.get("chatgpt_account_id")
        or auth.get("user_id")
        or claims.get("user_id")
        or _first_org_id(claims)
    )
    return {
        "email": claims.get("email") or profile.get("email"),
        "chatgpt_plan_type": auth.get("chatgpt_plan_type") or claims.get("chatgpt_plan_type"),
        "chatgpt_user_id": auth.get("chatgpt_user_id") or auth.get("user_id") or claims.get("user_id"),
        "chatgpt_account_id": account_id,
        "chatgpt_account_is_fedramp": bool(auth.get("chatgpt_account_is_fedramp", False)),
        "raw_jwt": id_token,
    }


def _codex_models_client_version() -> str:
    for path, key in ((_models_cache_path(), "client_version"), (_version_path(), "latest_version")):
        try:
            value = json.loads(path.read_text(encoding="utf-8")).get(key)
        except (OSError, UnicodeDecodeError, json.JSONDecodeError, AttributeError):
            continue
        if isinstance(value, str) and value:
            return value
    return CODEX_MODELS_CLIENT_VERSION


def _request_json(
    url: str,
    payload: dict[str, Any] | None = None,
    headers: dict[str, str] | None = None,
    timeout: float = 120.0,
) -> dict[str, Any]:
    data = None if payload is None else json.dumps(payload).encode("utf-8")
    req_headers = {"Content-Type": "application/json", "User-Agent": "Rikugan Codex"}
    if headers:
        req_headers.update(headers)
    req = urllib.request.Request(url, data=data, headers=req_headers, method="GET" if data is None else "POST")
    with urllib.request.urlopen(req, timeout=timeout, context=ssl_context()) as resp:
        raw = resp.read().decode("utf-8")
    return json.loads(raw) if raw else {}


def _request_form(url: str, fields: dict[str, str], timeout: float = 120.0) -> dict[str, Any]:
    data = urllib.parse.urlencode(fields).encode("utf-8")
    req = urllib.request.Request(
        url,
        data=data,
        headers={"Content-Type": "application/x-www-form-urlencoded", "User-Agent": "Rikugan Codex"},
        method="POST",
    )
    with urllib.request.urlopen(req, timeout=timeout, context=ssl_context()) as resp:
        return json.loads(resp.read().decode("utf-8"))


def request_codex_device_code() -> CodexDeviceCode:
    """Start the Codex device-code login flow."""
    base = CODEX_ISSUER.rstrip("/")
    resp = _request_json(
        f"{base}/api/accounts/deviceauth/usercode",
        {"client_id": CODEX_CLIENT_ID},
        timeout=30.0,
    )
    interval_raw = resp.get("interval", 5)
    try:
        interval = int(interval_raw)
    except (TypeError, ValueError):
        interval = 5
    return CodexDeviceCode(
        verification_url=f"{base}/codex/device",
        user_code=str(resp.get("user_code") or resp.get("usercode") or ""),
        device_auth_id=str(resp.get("device_auth_id") or ""),
        interval=max(1, interval),
    )


def _save_codex_auth(id_token: str, access_token: str, refresh_token: str) -> None:
    token_info = _id_token_info(id_token)
    account_id = token_info.get("chatgpt_account_id")
    auth = {
        "auth_mode": CODEX_AUTH_MODE,
        "OPENAI_API_KEY": None,
        "tokens": {
            "id_token": id_token,
            "access_token": access_token,
            "refresh_token": refresh_token,
            "account_id": account_id,
        },
        "last_refresh": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "agent_identity": None,
    }
    path = _auth_path()
    path.parent.mkdir(parents=True, exist_ok=True)
    flags = os.O_WRONLY | os.O_CREAT | os.O_TRUNC
    fd = os.open(path, flags, 0o600)
    with os.fdopen(fd, "w") as f:
        json.dump(auth, f, indent=2)


def complete_codex_device_code_login(device_code: CodexDeviceCode, timeout_seconds: int = 15 * 60) -> None:
    """Poll Codex device auth and persist the resulting OAuth tokens."""
    base = CODEX_ISSUER.rstrip("/")
    deadline = time.monotonic() + timeout_seconds
    code_resp: dict[str, Any] | None = None
    while time.monotonic() < deadline:
        try:
            code_resp = _request_json(
                f"{base}/api/accounts/deviceauth/token",
                {
                    "device_auth_id": device_code.device_auth_id,
                    "user_code": device_code.user_code,
                },
                timeout=30.0,
            )
            break
        except urllib.error.HTTPError as exc:
            if exc.code not in (403, 404):
                raise
            time.sleep(device_code.interval)
    if not code_resp:
        raise TimeoutError("Codex device auth timed out after 15 minutes")

    tokens = _request_form(
        f"{base}/oauth/token",
        {
            "grant_type": "authorization_code",
            "code": str(code_resp["authorization_code"]),
            "redirect_uri": f"{base}/deviceauth/callback",
            "client_id": CODEX_CLIENT_ID,
            "code_verifier": str(code_resp["code_verifier"]),
        },
    )
    _save_codex_auth(
        str(tokens["id_token"]),
        str(tokens["access_token"]),
        str(tokens["refresh_token"]),
    )


def codex_auth_status() -> tuple[str, str]:
    try:
        auth = json.loads(_auth_path().read_text(encoding="utf-8"))
        tokens = auth.get("tokens") or {}
        if (
            auth.get("auth_mode") == CODEX_AUTH_MODE
            and isinstance(tokens, dict)
            and tokens.get("access_token")
            and tokens.get("refresh_token")
        ):
            return "ChatGPT OAuth", "ok"
    except (OSError, UnicodeDecodeError, json.JSONDecodeError):
        pass
    return "Setup required", "error"


class CodexProvider(LLMProvider):
    """Adapter for Codex-backed ChatGPT OAuth sessions."""

    def __init__(self, api_key: str = "", api_base: str = "", model: str = "gpt-5.4", **kwargs):
        super().__init__(api_key=api_key, api_base=api_base or CODEX_BASE_URL, model=model)
        self._auth: dict[str, Any] = {}

    @property
    def name(self) -> str:
        return "codex"

    @property
    def capabilities(self) -> ProviderCapabilities:
        return ProviderCapabilities(
            streaming=True,
            tool_use=True,
            vision=True,
            max_context_window=272000,
            max_output_tokens=128000,
            supports_system_prompt=True,
        )

    def auth_status(self) -> tuple[str, str]:
        return codex_auth_status()

    def _load_auth(self) -> dict[str, Any]:
        try:
            auth = json.loads(_auth_path().read_text(encoding="utf-8"))
        except (OSError, UnicodeDecodeError, json.JSONDecodeError) as exc:
            raise AuthenticationError(
                "No Codex OAuth token found. Open settings and run Setup Codex.",
                provider="codex",
            ) from exc
        tokens = auth.get("tokens") or {}
        if not isinstance(tokens, dict) or auth.get("auth_mode") != CODEX_AUTH_MODE or not tokens.get("access_token"):
            raise AuthenticationError(
                "No Codex OAuth token found. Open settings and run Setup Codex.",
                provider="codex",
            )
        if not tokens.get("refresh_token"):
            raise AuthenticationError("Codex refresh token is missing. Run Setup Codex again.", provider="codex")
        self._auth = auth
        return auth

    def _refresh_auth(self) -> None:
        auth = self._load_auth()
        refresh_token = ((auth.get("tokens") or {}).get("refresh_token") or "").strip()
        if not refresh_token:
            raise AuthenticationError("Codex refresh token is missing. Run Setup Codex again.", provider="codex")
        tokens = _request_json(
            f"{CODEX_ISSUER}/oauth/token",
            {
                "client_id": CODEX_CLIENT_ID,
                "grant_type": "refresh_token",
                "refresh_token": refresh_token,
            },
            timeout=60.0,
        )
        current = auth.get("tokens") or {}
        current_id = current.get("id_token") or ""
        if isinstance(current_id, dict):
            current_id = current_id.get("raw_jwt") or ""
        _save_codex_auth(
            str(tokens.get("id_token") or current_id),
            str(tokens.get("access_token") or current.get("access_token") or ""),
            str(tokens.get("refresh_token") or current.get("refresh_token") or ""),
        )
        self._auth = {}

    def _headers(self) -> dict[str, str]:
        auth = self._load_auth()
        tokens = auth.get("tokens") or {}
        access_token = str(tokens.get("access_token") or "")
        account_id = tokens.get("account_id")
        id_token = tokens.get("id_token") or {}
        if isinstance(id_token, dict):
            account_id = account_id or id_token.get("chatgpt_account_id")
            fedramp = bool(id_token.get("chatgpt_account_is_fedramp", False))
        else:
            info = _id_token_info(str(id_token))
            account_id = account_id or info.get("chatgpt_account_id")
            fedramp = bool(info.get("chatgpt_account_is_fedramp", False))
        if not account_id:
            access_info = _id_token_info(access_token)
            account_id = access_info.get("chatgpt_account_id")
        headers = {
            "Authorization": f"Bearer {access_token}",
            "Accept": "text/event-stream",
            "User-Agent": f"{CODEX_ORIGINATOR}/0.0.0 (Rikugan)",
            "originator": CODEX_ORIGINATOR,
        }
        if account_id:
            headers["ChatGPT-Account-ID"] = str(account_id)
        if fedramp:
            headers["X-OpenAI-Fedramp"] = "true"
        return headers

    def _get_client(self) -> Any:
        self._load_auth()
        return self

    def _fetch_models_live(self) -> list[ModelInfo]:
        self._load_auth()
        client_version = urllib.parse.quote(_codex_models_client_version(), safe="")
        response = self._request("GET", f"models?client_version={client_version}", None, stream=False)
        models = response.get("models") if isinstance(response, dict) else None
        if not isinstance(models, list):
            raise ProviderError("Codex models endpoint returned no models list", provider="codex")

        parsed = [m for item in models if (m := self._parse_model_info(item)) is not None]
        if not parsed:
            raise ProviderError("Codex models endpoint returned no visible models", provider="codex")
        return parsed

    def _parse_model_info(self, item: Any) -> ModelInfo | None:
        if not isinstance(item, dict):
            return None
        model_id = str(item.get("slug") or item.get("id") or "").strip()
        if not model_id:
            return None
        if str(item.get("visibility") or "list").lower() in {"hide", "hidden"}:
            return None

        context_window = self._positive_int(item.get("context_window")) or self.capabilities.max_context_window
        max_output = (
            self._positive_int(item.get("max_output_tokens"))
            or self._positive_int(item.get("output_token_limit"))
            or self.capabilities.max_output_tokens
        )
        input_modalities = item.get("input_modalities")
        supports_vision = True
        if isinstance(input_modalities, list):
            supports_vision = "image" in {str(modality).lower() for modality in input_modalities}

        return ModelInfo(
            id=model_id,
            name=str(item.get("display_name") or model_id),
            provider="codex",
            context_window=context_window,
            max_output_tokens=max_output,
            supports_tools=bool(item.get("supports_parallel_tool_calls", True)),
            supports_vision=supports_vision,
        )

    @staticmethod
    def _positive_int(value: Any) -> int | None:
        try:
            parsed = int(value)
        except (TypeError, ValueError):
            return None
        return parsed if parsed > 0 else None

    def list_models(self) -> list[ModelInfo]:
        """Always resolve the model list from the Codex upstream.

        Codex model availability is account- and client-version-specific, so a
        static fallback would misrepresent what the user can actually call.
        Unlike the base implementation, failures are propagated to the caller
        (e.g. the settings dialog) so they surface as an explicit error instead
        of being masked behind placeholder models.
        """
        return self._fetch_models_live()

    @staticmethod
    def _builtin_models() -> list[ModelInfo]:
        # Codex has no offline defaults — see list_models(). The model list is
        # always fetched live; if it doesn't arrive, the model is not available.
        return []

    def _format_messages(self, messages: list[Message]) -> list[dict[str, Any]]:
        items: list[dict[str, Any]] = []
        for msg in messages:
            if msg.role == Role.SYSTEM:
                continue
            if msg.role == Role.USER:
                items.append(
                    {
                        "type": "message",
                        "role": "user",
                        "content": [{"type": "input_text", "text": msg.content or "continue"}],
                    }
                )
            elif msg.role == Role.ASSISTANT:
                if msg.content:
                    items.append(
                        {
                            "type": "message",
                            "role": "assistant",
                            "content": [{"type": "output_text", "text": msg.content}],
                        }
                    )
                for tc in msg.tool_calls:
                    items.append(
                        {
                            "type": "function_call",
                            "name": tc.name,
                            "arguments": json.dumps(tc.arguments),
                            "call_id": tc.id,
                        }
                    )
            elif msg.role == Role.TOOL:
                for tr in msg.tool_results:
                    items.append(
                        {
                            "type": "function_call_output",
                            "call_id": tr.tool_call_id,
                            "output": tr.content,
                        }
                    )
        return items

    def _format_tools(self, tools: list[dict[str, Any]] | None) -> list[dict[str, Any]]:
        out = []
        for tool_def in tools or []:
            func = tool_def.get("function", tool_def)
            out.append(
                {
                    "type": "function",
                    "name": func["name"],
                    "description": func.get("description", ""),
                    "parameters": func.get("parameters", {"type": "object", "properties": {}}),
                    "strict": False,
                }
            )
        return out

    def _build_request_kwargs(
        self,
        messages: list[Message],
        tools: list[dict[str, Any]] | None,
        system: str,
    ) -> dict[str, Any]:
        kwargs: dict[str, Any] = {
            "model": self.model,
            "input": self._format_messages(messages),
            "store": False,
            "stream": True,
        }
        if system:
            kwargs["instructions"] = system
        formatted_tools = self._format_tools(tools)
        if formatted_tools:
            kwargs["tools"] = formatted_tools
            kwargs["tool_choice"] = "auto"
            kwargs["parallel_tool_calls"] = True
        return kwargs

    def _request(self, method: str, path: str, payload: dict[str, Any] | None, stream: bool) -> Any:
        url = f"{self.api_base.rstrip('/')}/{path.lstrip('/')}"
        data = None if payload is None else json.dumps(payload).encode("utf-8")
        headers = self._headers()
        headers["Accept"] = "text/event-stream" if stream else "application/json"
        if payload is not None:
            headers["Content-Type"] = "application/json"
        req = urllib.request.Request(url, data=data, headers=headers, method=method)
        try:
            resp = urllib.request.urlopen(req, timeout=120.0, context=ssl_context())
        except urllib.error.HTTPError as exc:
            if exc.code == 401:
                self._refresh_auth()
                headers = self._headers()
                if payload is not None:
                    headers["Content-Type"] = "application/json"
                req = urllib.request.Request(url, data=data, headers=headers, method=method)
                resp = urllib.request.urlopen(req, timeout=120.0, context=ssl_context())
            else:
                raise
        if stream:
            return resp
        raw = resp.read().decode("utf-8")
        return json.loads(raw) if raw else {}

    def _call_api(self, client: Any, kwargs: dict[str, Any]) -> Any:
        return list(self._stream_chunks(client, kwargs))

    def _normalize_response(self, raw: Any) -> Message:
        text = ""
        tool_calls: list[ToolCall] = []
        usage = TokenUsage()
        current: dict[str, dict[str, Any]] = {}
        for chunk in raw:
            if chunk.text:
                text += chunk.text
            if chunk.is_tool_call_start and chunk.tool_call_id:
                current[chunk.tool_call_id] = {"name": chunk.tool_name or "", "args": ""}
            if chunk.tool_args_delta and chunk.tool_call_id:
                current.setdefault(chunk.tool_call_id, {"name": chunk.tool_name or "", "args": ""})
                current[chunk.tool_call_id]["args"] += chunk.tool_args_delta
            if chunk.is_tool_call_end and chunk.tool_call_id:
                item = current.pop(chunk.tool_call_id, {"name": chunk.tool_name or "", "args": "{}"})
                try:
                    args = json.loads(item["args"] or "{}")
                except json.JSONDecodeError:
                    args = {}
                tool_calls.append(ToolCall(id=chunk.tool_call_id, name=item["name"], arguments=args))
            if chunk.usage:
                usage = chunk.usage
        return Message(role=Role.ASSISTANT, content=text, tool_calls=tool_calls, token_usage=usage)

    def _handle_api_error(self, e: Exception) -> NoReturn:
        if isinstance(e, urllib.error.HTTPError):
            msg = e.read().decode("utf-8", errors="replace")
            if e.code in (401, 403):
                raise AuthenticationError(msg or str(e), provider="codex") from e
            if e.code == 429:
                raise RateLimitError(provider="codex") from e
            if "context" in msg.lower() or "token" in msg.lower():
                raise ContextLengthError(msg, provider="codex") from e
            raise ProviderError(msg or str(e), provider="codex") from e
        raise ProviderError(str(e), provider="codex") from e

    def _stream_chunks(self, client: Any, kwargs: dict[str, Any]) -> Generator[StreamChunk, None, None]:
        try:
            response = self._request("POST", "responses", kwargs, stream=True)
            with self._track_request_handle(response):
                yield from self._iter_sse(response)
        except Exception as e:
            self._handle_api_error(e)

    def _iter_sse(self, response: Any) -> Generator[StreamChunk, None, None]:
        buffers: dict[str, dict[str, str]] = {}
        for raw in response:
            line = raw.decode("utf-8", errors="replace").strip()
            if not line.startswith("data:"):
                continue
            data = line[5:].strip()
            if not data or data == "[DONE]":
                continue
            try:
                event = json.loads(data)
            except json.JSONDecodeError:
                continue
            kind = event.get("type", "")
            if kind == "response.output_text.delta":
                yield StreamChunk(text=str(event.get("delta") or ""))
            elif kind in ("response.function_call_arguments.delta", "response.custom_tool_call_input.delta"):
                call_id = str(event.get("call_id") or event.get("item_id") or "")
                if call_id:
                    delta = str(event.get("delta") or "")
                    buffers.setdefault(call_id, {"name": "", "args": ""})
                    buffers[call_id]["args"] += delta
                    yield StreamChunk(tool_call_id=call_id, tool_args_delta=delta)
            elif kind == "response.output_item.added":
                item = event.get("item") or {}
                if item.get("type") == "function_call":
                    call_id = str(item.get("call_id") or item.get("id") or "")
                    if call_id:
                        existing = buffers.setdefault(call_id, {"name": "", "args": ""})
                        existing["name"] = str(item.get("name") or existing.get("name") or "")
                        yield StreamChunk(
                            tool_call_id=call_id,
                            tool_name=existing["name"],
                            is_tool_call_start=True,
                        )
            elif kind == "response.output_item.done":
                item = event.get("item") or {}
                if item.get("type") == "function_call":
                    call_id = str(item.get("call_id") or item.get("id") or "")
                    if call_id:
                        args = str(item.get("arguments") or "")
                        if args and not buffers.get(call_id, {}).get("args"):
                            yield StreamChunk(tool_call_id=call_id, tool_args_delta=args)
                        yield StreamChunk(
                            tool_call_id=call_id,
                            tool_name=str(item.get("name") or buffers.get(call_id, {}).get("name") or ""),
                            is_tool_call_end=True,
                        )
            elif kind == "response.completed":
                resp = event.get("response") or {}
                usage = resp.get("usage") or {}
                yield StreamChunk(
                    usage=TokenUsage(
                        prompt_tokens=int(usage.get("input_tokens") or 0),
                        completion_tokens=int(usage.get("output_tokens") or 0),
                        total_tokens=int(usage.get("total_tokens") or 0),
                    ),
                    finish_reason="completed",
                )
                return
            elif kind in ("response.failed", "response.incomplete"):
                resp = event.get("response") or {}
                error = resp.get("error") or {}
                raise ProviderError(str(error.get("message") or kind), provider="codex")

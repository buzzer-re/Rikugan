"""Tests for the agent loop."""

from __future__ import annotations

import json
import os
import sys
import unittest
from typing import Any, Dict, List, Optional
from unittest.mock import MagicMock

sys.path.insert(0, os.path.dirname(os.path.dirname(__file__)))
from tests.mocks.ida_mock import install_ida_mocks
install_ida_mocks()

from rikugan.core.types import (
    Message, ModelInfo, ProviderCapabilities, Role, ToolCall,
    StreamChunk, TokenUsage,
)
from rikugan.core.config import RikuganConfig
from rikugan.agent.loop import AgentLoop, BackgroundAgentRunner
from rikugan.agent.exploration_mode import ExplorationState
from rikugan.agent.turn import TurnEventType
from rikugan.tools.base import ParameterSchema, ToolDefinition
from rikugan.tools.registry import ToolRegistry
from rikugan.state.session import INTERNAL_EVENT_CANCELLED, INTERNAL_EVENT_KEY, SessionState
from rikugan.providers.base import LLMProvider


class MockProvider(LLMProvider):
    """Mock LLM provider that returns scripted responses."""

    def __init__(self, responses: Optional[List[List[StreamChunk]]] = None):
        super().__init__(api_key="test", model="mock-model")
        self._responses = responses or []
        self._call_count = 0

    @property
    def name(self) -> str:
        return "mock"

    @property
    def capabilities(self) -> ProviderCapabilities:
        return ProviderCapabilities()

    def _get_client(self):
        return None

    def _fetch_models_live(self) -> List[ModelInfo]:
        return [ModelInfo(id="mock-model", name="Mock", provider="mock")]

    @staticmethod
    def _builtin_models() -> List[ModelInfo]:
        return [ModelInfo(id="mock-model", name="Mock", provider="mock")]

    def _format_messages(self, messages):
        return messages

    def _normalize_response(self, raw):
        return raw

    def _build_request_kwargs(self, messages, tools, system):
        return {}

    def _call_api(self, client, kwargs):
        return None

    def _handle_api_error(self, e):
        raise e

    def _stream_chunks(self, client, kwargs):
        yield from ()

    def chat(self, messages, tools=None, system=""):
        return Message(role=Role.ASSISTANT, content="mock response")

    def chat_stream(self, messages, tools=None, system=""):
        if self._call_count < len(self._responses):
            chunks = self._responses[self._call_count]
            self._call_count += 1
            for chunk in chunks:
                yield chunk
        else:
            yield StreamChunk(text="No more scripted responses.")


def _text_response(text: str) -> List[StreamChunk]:
    """Create a simple text-only response."""
    return [
        StreamChunk(text=text),
        StreamChunk(usage=TokenUsage(prompt_tokens=10, completion_tokens=5, total_tokens=15)),
    ]


def _text_response_no_usage(text: str) -> List[StreamChunk]:
    """Create a text response with no usage metadata (compat provider behavior)."""
    return [StreamChunk(text=text)]


def _text_response_with_finish_reason(text: str, finish_reason: str) -> List[StreamChunk]:
    """Create a text response that ends with an explicit provider finish reason."""
    return [StreamChunk(text=text), StreamChunk(finish_reason=finish_reason)]


def _tool_call_response(tool_name: str, args: Dict[str, Any], call_id: str = "call_1") -> List[StreamChunk]:
    """Create a response with a tool call."""
    return [
        StreamChunk(is_tool_call_start=True, tool_call_id=call_id, tool_name=tool_name),
        StreamChunk(tool_args_delta=json.dumps(args), tool_call_id=call_id),
        StreamChunk(is_tool_call_end=True, tool_call_id=call_id, tool_name=tool_name),
        StreamChunk(usage=TokenUsage(prompt_tokens=10, completion_tokens=5, total_tokens=15)),
    ]


class TestAgentLoop(unittest.TestCase):
    def _make_loop(self, provider: MockProvider, tools: Optional[ToolRegistry] = None) -> AgentLoop:
        config = RikuganConfig()
        config.auto_context = False  # Skip IDA API calls
        session = SessionState(provider_name="mock", model_name="mock-model")
        return AgentLoop(
            provider=provider,
            tool_registry=tools or ToolRegistry(),
            config=config,
            session=session,
        )

    def test_simple_text_response(self):
        provider = MockProvider(responses=[_text_response("Hello!")])
        loop = self._make_loop(provider)

        events = list(loop.run("Hi"))
        types = [e.type for e in events]
        self.assertIn(TurnEventType.TURN_START, types)
        self.assertIn(TurnEventType.TEXT_DELTA, types)
        self.assertIn(TurnEventType.TEXT_DONE, types)
        self.assertIn(TurnEventType.TURN_END, types)

        text_done = next(e for e in events if e.type == TurnEventType.TEXT_DONE)
        self.assertEqual(text_done.text, "Hello!")

    def test_session_records_messages(self):
        provider = MockProvider(responses=[_text_response("Hi there")])
        config = RikuganConfig()
        config.auto_context = False
        session = SessionState()
        loop = AgentLoop(provider, ToolRegistry(), config, session)

        list(loop.run("Hello"))
        self.assertEqual(len(session.messages), 2)
        self.assertEqual(session.messages[0].role, Role.USER)
        self.assertEqual(session.messages[0].content, "Hello")
        self.assertEqual(session.messages[1].role, Role.ASSISTANT)
        self.assertEqual(session.messages[1].content, "Hi there")

    def test_tool_call_and_result(self):
        # Set up a tool
        registry = ToolRegistry()
        registry.register(ToolDefinition(
            name="echo_tool",
            description="Echo the input",
            parameters=[ParameterSchema(name="text", type="string", description="Text to echo", required=True)],
            handler=lambda text: f"Echo: {text}",
            category="test",
        ))

        # Turn 1: tool call, Turn 2: text response
        provider = MockProvider(responses=[
            _tool_call_response("echo_tool", {"text": "hello"}, call_id="call_1"),
            _text_response("The echo returned hello"),
        ])
        loop = self._make_loop(provider, tools=registry)

        events = list(loop.run("Echo hello"))
        types = [e.type for e in events]
        self.assertIn(TurnEventType.TOOL_CALL_START, types)
        self.assertIn(TurnEventType.TOOL_CALL_DONE, types)
        self.assertIn(TurnEventType.TOOL_RESULT, types)

        tool_result = next(e for e in events if e.type == TurnEventType.TOOL_RESULT)
        # TurnEvent now carries the sanitized (wrapped) result, not the raw string.
        self.assertIn("Echo: hello", tool_result.tool_result)
        self.assertFalse(tool_result.tool_is_error)

    def test_tool_error(self):
        registry = ToolRegistry()
        registry.register(ToolDefinition(
            name="failing_tool",
            description="Always fails",
            parameters=[],
            handler=lambda: (_ for _ in ()).throw(ValueError("bad input")),
            category="test",
        ))

        provider = MockProvider(responses=[
            _tool_call_response("failing_tool", {}, call_id="call_1"),
            _text_response("Tool failed"),
        ])
        loop = self._make_loop(provider, tools=registry)

        events = list(loop.run("Run failing tool"))
        tool_result = next(e for e in events if e.type == TurnEventType.TOOL_RESULT)
        self.assertTrue(tool_result.tool_is_error)

    def test_cancel_aborts_provider_request(self):
        """Cancel closes the provider's in-flight client/stream."""
        provider = MockProvider(responses=[_text_response("Hello!")])
        client = MagicMock()
        provider._client = client
        loop = self._make_loop(provider)

        loop.cancel()

        client.close.assert_called_once()
        self.assertIsNone(provider._client)

    def test_cancellation_mid_tool_loop(self):
        """Cancel during a multi-turn tool loop."""
        registry = ToolRegistry()

        def cancel_handler():
            # Cancel during tool execution
            loop.cancel()
            return "done"

        registry.register(ToolDefinition(
            name="cancel_trigger",
            description="Triggers cancel",
            parameters=[],
            handler=cancel_handler,
            category="test",
        ))

        provider = MockProvider(responses=[
            _tool_call_response("cancel_trigger", {}, call_id="call_1"),
            _text_response("Should not reach"),
        ])
        loop = self._make_loop(provider, tools=registry)

        events = list(loop.run("Trigger cancel"))
        types = [e.type for e in events]
        self.assertIn(TurnEventType.CANCELLED, types)
        # Should not reach the second response
        self.assertNotIn(TurnEventType.TEXT_DONE, types)
        self.assertEqual(loop.session.messages[-1].metadata[INTERNAL_EVENT_KEY], INTERNAL_EVENT_CANCELLED)
        self.assertEqual(loop.session.messages[-1].content, "Cancelled by user")

    def test_is_running_flag(self):
        provider = MockProvider(responses=[_text_response("Done")])
        loop = self._make_loop(provider)
        self.assertFalse(loop.is_running)

        list(loop.run("Hi"))  # consume generator
        self.assertFalse(loop.is_running)

    def test_usage_tracked(self):
        provider = MockProvider(responses=[_text_response("Hi")])
        config = RikuganConfig()
        config.auto_context = False
        session = SessionState()
        loop = AgentLoop(provider, ToolRegistry(), config, session)

        events = list(loop.run("Hello"))
        usage_events = [e for e in events if e.type == TurnEventType.USAGE_UPDATE]
        self.assertTrue(len(usage_events) > 0)
        self.assertEqual(session.total_usage.total_tokens, 15)

    def test_usage_fallback_when_provider_omits_usage(self):
        provider = MockProvider(responses=[_text_response_no_usage("Hi")])
        config = RikuganConfig()
        config.auto_context = False
        session = SessionState()
        loop = AgentLoop(provider, ToolRegistry(), config, session)

        events = list(loop.run("Hello"))
        usage_events = [e for e in events if e.type == TurnEventType.USAGE_UPDATE]

        # Local estimation should still drive token/context tracking.
        self.assertGreater(len(usage_events), 0)
        self.assertGreater(session.last_prompt_tokens, 0)
        self.assertGreater(session.total_usage.total_tokens, 0)

    def test_output_limit_finish_reason_is_visible(self):
        provider = MockProvider(responses=[_text_response_with_finish_reason("Partial", "max_tokens")])
        loop = self._make_loop(provider)

        events = list(loop.run("Long answer please"))
        # Truncation is informational: the answer arrived, just cut short. It is
        # a notice so it neither looks like nor counts as a failure.
        notices = [e for e in events if e.type == TurnEventType.NOTICE]

        self.assertTrue(notices)
        self.assertIn("output token limit", notices[0].text)
        self.assertEqual([], [e for e in events if e.type == TurnEventType.ERROR])

    def test_normal_stop_reasons_produce_no_banner(self):
        """A tool-calling turn ends with 'tool_use' on Anthropic and
        'tool_calls' on OpenAI. Neither is a failure."""
        for reason in ("tool_use", "tool_calls", "end_turn", "stop", "stop_sequence", "function_call"):
            with self.subTest(finish_reason=reason):
                provider = MockProvider(responses=[_text_response_with_finish_reason("Done", reason)])
                loop = self._make_loop(provider)

                events = list(loop.run("Analyze this"))

                self.assertEqual([], [e for e in events if e.type == TurnEventType.ERROR])
                self.assertEqual([], [e for e in events if e.type == TurnEventType.NOTICE])

    def test_execute_python_requires_approval_even_in_explore_only(self):
        provider = MockProvider()
        loop = self._make_loop(provider)
        loop._exploration_state = ExplorationState(explore_only=True)  # /explore context

        tc = ToolCall(
            id="call_approval_test",
            name="execute_python",
            arguments={"code": "print('hi')"},
        )

        gate = loop._wait_for_approval(tc)
        event = next(gate)
        self.assertEqual(event.type, TurnEventType.TOOL_APPROVAL_REQUEST)
        self.assertEqual(event.tool_name, "execute_python")

        loop.submit_tool_approval("allow")
        with self.assertRaises(StopIteration) as done:
            next(gate)
        self.assertTrue(done.exception.value)


class TestBackgroundAgentRunner(unittest.TestCase):
    def test_run_in_background(self):
        provider = MockProvider(responses=[_text_response("Background response")])
        config = RikuganConfig()
        config.auto_context = False
        session = SessionState()
        loop = AgentLoop(provider, ToolRegistry(), config, session)
        runner = BackgroundAgentRunner(loop)

        runner.start("Hello from background")

        events = []
        while True:
            event = runner.get_event(timeout=2.0)
            if event is None:
                break
            events.append(event)

        types = [e.type for e in events]
        self.assertIn(TurnEventType.TEXT_DONE, types)
        text_done = next(e for e in events if e.type == TurnEventType.TEXT_DONE)
        self.assertEqual(text_done.text, "Background response")


class TestSkillInvocation(unittest.TestCase):
    def test_skill_rewrite(self):
        """Test that /slug messages get rewritten with skill body."""
        import tempfile
        from rikugan.skills.registry import SkillRegistry

        with tempfile.TemporaryDirectory() as tmpdir:
            skill_dir = os.path.join(tmpdir, "test-skill")
            os.makedirs(skill_dir)
            with open(os.path.join(skill_dir, "SKILL.md"), "w") as f:
                f.write("---\nname: Test Skill\ndescription: A test\n---\nYou are a test skill.\n")

            registry = SkillRegistry(tmpdir)
            registry.discover()

            provider = MockProvider(responses=[_text_response("Skill response")])
            config = RikuganConfig()
            config.auto_context = False
            session = SessionState()
            loop = AgentLoop(provider, ToolRegistry(), config, session, skill_registry=registry)

            list(loop.run("/test-skill do something"))

            # The user message in session should contain the skill body
            user_msg = session.messages[0]
            self.assertIn("[Skill: Test Skill]", user_msg.content)
            self.assertIn("You are a test skill.", user_msg.content)
            self.assertIn("do something", user_msg.content)


class TestProfileEnforcement(unittest.TestCase):
    """Test that analysis profiles are enforced in the agent loop."""

    def _make_loop_with_profile(
        self, profile_name: str, provider: MockProvider,
        tools: ToolRegistry = None, custom_profiles: dict = None,
    ) -> AgentLoop:
        config = RikuganConfig()
        config.auto_context = False
        config.active_profile = profile_name
        if custom_profiles:
            config.custom_profiles = custom_profiles
        session = SessionState(provider_name="mock", model_name="mock-model")
        return AgentLoop(
            provider=provider,
            tool_registry=tools or ToolRegistry(),
            config=config,
            session=session,
        )

    def test_private_profile_skips_binary_info(self):
        """Private profile should not call get_binary_info."""
        config = RikuganConfig()
        config.auto_context = True  # Enable auto-context
        config.active_profile = "private"

        registry = ToolRegistry()
        calls = []

        def track_binary_info():
            calls.append("get_binary_info")
            return "Binary: test.exe"

        registry.register(ToolDefinition(
            name="get_binary_info",
            description="Get binary info",
            parameters=[],
            handler=track_binary_info,
            category="context",
        ))

        provider = MockProvider(responses=[_text_response("Done")])
        session = SessionState(provider_name="mock", model_name="mock-model")
        loop = AgentLoop(provider, registry, config, session)

        list(loop.run("Hi"))
        # get_binary_info should NOT have been called because private profile hides metadata
        self.assertEqual(calls, [])

    def test_ioc_stripping_in_tool_results(self):
        """ioc_filters should strip hashes/IPs from tool results."""
        registry = ToolRegistry()
        registry.register(ToolDefinition(
            name="test_tool",
            description="Returns IOC data",
            parameters=[],
            handler=lambda: "Hash: d41d8cd98f00b204e9800998ecf8427e, IP: 10.0.0.1",
            category="test",
        ))

        # Use private profile which has all ioc_filters enabled
        config = RikuganConfig()
        config.auto_context = False
        config.active_profile = "private"
        session = SessionState(provider_name="mock", model_name="mock-model")

        provider = MockProvider(responses=[
            _tool_call_response("test_tool", {}, call_id="call_ioc"),
            _text_response("Done"),
        ])
        loop = AgentLoop(provider, registry, config, session)

        events = list(loop.run("Run test"))
        tool_result_event = next(
            (e for e in events if e.type == TurnEventType.TOOL_RESULT),
            None,
        )
        self.assertIsNotNone(tool_result_event)
        # IOCs should be redacted
        self.assertIn("[HASH_REDACTED]", tool_result_event.tool_result)
        self.assertIn("[IP_REDACTED]", tool_result_event.tool_result)

    def test_denied_tools_filtered_from_schema(self):
        """Denied tools should not appear in the tools schema."""
        registry = ToolRegistry()
        registry.register(ToolDefinition(
            name="allowed_tool",
            description="Allowed",
            parameters=[],
            handler=lambda: "ok",
            category="test",
        ))
        registry.register(ToolDefinition(
            name="denied_tool",
            description="Denied",
            parameters=[],
            handler=lambda: "ok",
            category="test",
        ))

        custom_profiles = {
            "restricted": {
                "name": "restricted",
                "denied_tools": ["denied_tool"],
            }
        }

        provider = MockProvider(responses=[_text_response("Done")])
        loop = self._make_loop_with_profile(
            "restricted", provider, tools=registry, custom_profiles=custom_profiles,
        )

        schema = loop._build_tools_schema(None, False)
        tool_names = [t.get("function", {}).get("name") for t in schema]
        self.assertIn("allowed_tool", tool_names)
        self.assertNotIn("denied_tool", tool_names)

    def test_granular_ioc_filter_only_selected(self):
        """Only selected IOC categories should be redacted."""
        registry = ToolRegistry()
        registry.register(ToolDefinition(
            name="test_tool",
            description="Returns mixed IOCs",
            parameters=[],
            handler=lambda: "Hash: d41d8cd98f00b204e9800998ecf8427e, IP: 10.0.0.1, url: http://evil.com/bad",
            category="test",
        ))

        # Custom profile with only hashes enabled
        custom_profiles = {
            "hash-only": {
                "name": "hash-only",
                "ioc_filters": {"hashes": True, "ipv4": False, "urls": False},
            }
        }
        config = RikuganConfig()
        config.auto_context = False
        config.active_profile = "hash-only"
        config.custom_profiles = custom_profiles
        session = SessionState(provider_name="mock", model_name="mock-model")

        provider = MockProvider(responses=[
            _tool_call_response("test_tool", {}, call_id="call_granular"),
            _text_response("Done"),
        ])
        loop = AgentLoop(provider, registry, config, session)

        events = list(loop.run("Run test"))
        tool_result_event = next(
            (e for e in events if e.type == TurnEventType.TOOL_RESULT),
            None,
        )
        self.assertIsNotNone(tool_result_event)
        self.assertIn("[HASH_REDACTED]", tool_result_event.tool_result)
        # IP and URL should NOT be redacted
        self.assertIn("10.0.0.1", tool_result_event.tool_result)
        self.assertIn("http://evil.com/bad", tool_result_event.tool_result)

    def test_custom_filter_rule_in_tool_result(self):
        """Custom filter rules should be applied to tool results."""
        registry = ToolRegistry()
        registry.register(ToolDefinition(
            name="test_tool",
            description="Returns sensitive data",
            parameters=[],
            handler=lambda: "hostname: DESKTOP-VICTIM, key: sk-abcdef1234567890",
            category="test",
        ))

        custom_profiles = {
            "custom-rules": {
                "name": "custom-rules",
                "ioc_filters": {},
                "custom_filter_rules": [
                    {"name": "host", "pattern": "DESKTOP-VICTIM", "is_regex": False, "replacement": "[HOST]"},
                    {"name": "key", "pattern": r"sk-[a-zA-Z0-9]+", "is_regex": True, "replacement": "[KEY]"},
                ],
            }
        }
        config = RikuganConfig()
        config.auto_context = False
        config.active_profile = "custom-rules"
        config.custom_profiles = custom_profiles
        session = SessionState(provider_name="mock", model_name="mock-model")

        provider = MockProvider(responses=[
            _tool_call_response("test_tool", {}, call_id="call_custom"),
            _text_response("Done"),
        ])
        loop = AgentLoop(provider, registry, config, session)

        events = list(loop.run("Run test"))
        tool_result_event = next(
            (e for e in events if e.type == TurnEventType.TOOL_RESULT),
            None,
        )
        self.assertIsNotNone(tool_result_event)
        self.assertIn("[HOST]", tool_result_event.tool_result)
        self.assertIn("[KEY]", tool_result_event.tool_result)
        self.assertNotIn("DESKTOP-VICTIM", tool_result_event.tool_result)

    def test_default_profile_no_filtering(self):
        """Default profile should not strip IOCs or hide metadata."""
        registry = ToolRegistry()
        registry.register(ToolDefinition(
            name="test_tool",
            description="Returns data",
            parameters=[],
            handler=lambda: "Hash: d41d8cd98f00b204e9800998ecf8427e",
            category="test",
        ))

        config = RikuganConfig()
        config.auto_context = False
        config.active_profile = "default"
        session = SessionState(provider_name="mock", model_name="mock-model")

        provider = MockProvider(responses=[
            _tool_call_response("test_tool", {}, call_id="call_def"),
            _text_response("Done"),
        ])
        loop = AgentLoop(provider, registry, config, session)

        events = list(loop.run("Run test"))
        tool_result_event = next(
            (e for e in events if e.type == TurnEventType.TOOL_RESULT),
            None,
        )
        self.assertIsNotNone(tool_result_event)
        # Default profile does NOT strip IOCs
        self.assertNotIn("[HASH_REDACTED]", tool_result_event.tool_result)

    def test_denied_tool_blocked_at_execution(self):
        """Denied tools should be blocked at execution time, not just schema filtering."""
        registry = ToolRegistry()
        registry.register(ToolDefinition(
            name="list_functions",
            description="Lists functions",
            parameters=[],
            handler=lambda: "func1\nfunc2\nfunc3",
            category="functions",
        ))

        custom_profiles = {
            "restricted": {
                "name": "restricted",
                "denied_tools": ["list_functions"],
            }
        }

        # LLM tries to call the denied tool anyway
        provider = MockProvider(responses=[
            _tool_call_response("list_functions", {}, call_id="call_denied"),
            _text_response("Done"),
        ])
        loop = self._make_loop_with_profile(
            "restricted", provider, tools=registry, custom_profiles=custom_profiles,
        )

        events = list(loop.run("list functions"))
        tool_result_event = next(
            (e for e in events if e.type == TurnEventType.TOOL_RESULT),
            None,
        )
        self.assertIsNotNone(tool_result_event)
        # Tool should be blocked with an error, not executed
        self.assertIn("denied by the active profile", tool_result_event.tool_result)
        self.assertNotIn("func1", tool_result_event.tool_result)


if __name__ == "__main__":
    unittest.main()

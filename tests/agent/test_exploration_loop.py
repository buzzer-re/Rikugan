"""End-to-end tests for exploration mode event sequence with mock provider."""

from __future__ import annotations

import json
import os
import sys
import unittest
from typing import Any, Dict, List, Optional

sys.path.insert(0, os.path.dirname(os.path.dirname(__file__)))
from tests.mocks.ida_mock import install_ida_mocks
install_ida_mocks()

from rikugan.core.types import (
    Message, ModelInfo, ProviderCapabilities, Role,
    StreamChunk, TokenUsage,
)
from rikugan.core.config import RikuganConfig
from rikugan.agent.loop import AgentLoop
from rikugan.agent.turn import TurnEvent, TurnEventType
from rikugan.tools.base import ParameterSchema, ToolDefinition
from rikugan.tools.registry import ToolRegistry
from rikugan.state.session import SessionState
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
    return [
        StreamChunk(text=text),
        StreamChunk(usage=TokenUsage(prompt_tokens=10, completion_tokens=5, total_tokens=15)),
    ]


def _tool_call_response(tool_name: str, args: Dict[str, Any], call_id: str = "call_1") -> List[StreamChunk]:
    return [
        StreamChunk(is_tool_call_start=True, tool_call_id=call_id, tool_name=tool_name),
        StreamChunk(tool_args_delta=json.dumps(args), tool_call_id=call_id),
        StreamChunk(is_tool_call_end=True, tool_call_id=call_id, tool_name=tool_name),
        StreamChunk(usage=TokenUsage(prompt_tokens=10, completion_tokens=5, total_tokens=15)),
    ]


def _make_registry() -> ToolRegistry:
    """Create a minimal tool registry for tests."""
    registry = ToolRegistry()
    # Register a dummy read-only tool
    defn = ToolDefinition(
        name="decompile_function",
        description="Decompile a function",
        parameters=[ParameterSchema(name="name", type="string")],
        handler=lambda name="": f"int {name}(void) {{ return 0; }}",
    )
    registry.register(defn)
    return registry


class TestExplorationModeEvents(unittest.TestCase):
    """Verify exploration mode emits events in the correct order."""

    def _run_loop(self, loop: AgentLoop, message: str) -> List[TurnEvent]:
        """Consume the generator, collecting all events."""
        events = []
        for event in loop.run(message):
            events.append(event)
        return events

    def test_explore_only_emits_phase_change(self):
        """Explore-only mode should emit exploration_phase_change at start."""
        provider = MockProvider([
            # Turn 1: agent calls exploration_report
            _tool_call_response("exploration_report", {
                "category": "function_purpose",
                "summary": "main() is the entry point",
                "address": 4198400,  # 0x401000
                "function_name": "main",
                "relevance": "high",
            }),
            # Turn 2: text-only response, agent is done
            _text_response("I found that main() is the entry point."),
        ])

        loop = AgentLoop(
            provider=provider,
            tool_registry=_make_registry(),
            config=RikuganConfig(),
            session=SessionState(),
        )

        events = self._run_loop(loop, "/explore Find the entry point")

        # Should have exploration_phase_change event at start
        phase_events = [e for e in events if e.type == TurnEventType.EXPLORATION_PHASE_CHANGE]
        self.assertTrue(len(phase_events) >= 1)
        self.assertEqual(phase_events[0].metadata["to_phase"], "explore")

        # Should have exploration_finding event
        finding_events = [e for e in events if e.type == TurnEventType.EXPLORATION_FINDING]
        self.assertEqual(len(finding_events), 1)
        self.assertEqual(finding_events[0].metadata["category"], "function_purpose")

        # Should have turn_start and turn_end
        starts = [e for e in events if e.type == TurnEventType.TURN_START]
        ends = [e for e in events if e.type == TurnEventType.TURN_END]
        self.assertTrue(len(starts) >= 1)
        self.assertTrue(len(ends) >= 1)

    def test_explore_only_no_plan_phase(self):
        """Explore-only mode should NOT enter plan phase."""
        provider = MockProvider([
            _text_response("Here's what I found about the binary."),
        ])

        loop = AgentLoop(
            provider=provider,
            tool_registry=_make_registry(),
            config=RikuganConfig(),
            session=SessionState(),
        )

        events = self._run_loop(loop, "/explore Analyze this binary")

        # Should NOT have plan or execute phases
        phase_events = [e for e in events if e.type == TurnEventType.EXPLORATION_PHASE_CHANGE]
        to_phases = [e.metadata.get("to_phase") for e in phase_events]
        self.assertNotIn("plan", to_phases)
        self.assertNotIn("execute", to_phases)

    def test_knowledge_base_populated_from_findings(self):
        """exploration_report should populate the knowledge base."""
        provider = MockProvider([
            _tool_call_response("exploration_report", {
                "category": "hypothesis",
                "summary": "Change constant at 0x401248",
                "relevance": "high",
            }, "c1"),
            _tool_call_response("exploration_report", {
                "category": "function_purpose",
                "summary": "Score handler",
                "address": 4198400,
                "function_name": "score_handler",
                "relevance": "high",
            }, "c2"),
            _text_response("Done exploring."),
        ])

        loop = AgentLoop(
            provider=provider,
            tool_registry=_make_registry(),
            config=RikuganConfig(),
            session=SessionState(),
        )

        self._run_loop(loop, "/explore Find score functions")

        # Knowledge base should have findings
        kb = loop.last_knowledge_base
        self.assertIsNotNone(kb)
        self.assertTrue(len(kb.findings) >= 2)
        self.assertTrue(len(kb.hypotheses) >= 1)

    def test_phase_transition_denied_without_findings(self):
        """phase_transition to plan should be denied without sufficient findings."""
        provider = MockProvider([
            _tool_call_response("phase_transition", {
                "to_phase": "plan",
                "reason": "Ready to plan",
            }),
            _text_response("OK, I'll keep exploring."),
        ])

        loop = AgentLoop(
            provider=provider,
            tool_registry=_make_registry(),
            config=RikuganConfig(),
            session=SessionState(),
        )

        events = self._run_loop(loop, "/explore Find something")

        # The phase transition should be denied
        tool_results = [e for e in events if e.type == TurnEventType.TOOL_RESULT]
        denied = any("Cannot transition" in (e.tool_result or "") for e in tool_results)
        self.assertTrue(denied)


def _rename_function_registry() -> ToolRegistry:
    """rename_function as the hosts declare it, with the reader undo needs."""
    registry = ToolRegistry()
    registry.register(ToolDefinition(
        name="rename_function",
        description="Rename a function",
        parameters=[
            ParameterSchema(name="address", type="string"),
            ParameterSchema(name="new_name", type="string"),
        ],
        mutating=True,
        handler=lambda address="", new_name="": f"Renamed {address}: sub_401000 → {new_name}",
    ))
    registry.register(ToolDefinition(
        name="read_function_name",
        description="Read a function's name",
        parameters=[ParameterSchema(name="address", type="string")],
        internal=True,
        handler=lambda address="": "sub_401000",
    ))
    return registry


class TestMutationTracking(unittest.TestCase):
    """Verify mutation log is populated on mutating tool calls."""

    def test_rename_function_recorded(self):
        """rename_function should be recorded in mutation log."""
        provider = MockProvider([
            _tool_call_response("rename_function", {
                "address": "0x401000",
                "new_name": "main",
            }),
            _text_response("Renamed the function."),
        ])

        registry = _rename_function_registry()

        loop = AgentLoop(
            provider=provider,
            tool_registry=registry,
            config=RikuganConfig(),
            session=SessionState(),
        )

        list(loop.run("Rename sub_401000 to main"))

        self.assertEqual(len(loop._mutation_log), 1)
        rec = loop._mutation_log[0]
        self.assertTrue(rec.reversible)
        self.assertEqual(rec.reverse_tool, "rename_function")
        self.assertEqual(rec.reverse_arguments, {"address": "0x401000", "new_name": "sub_401000"})


    def test_mutation_emits_event(self):
        """Mutating tool should emit MUTATION_RECORDED event."""
        provider = MockProvider([
            _tool_call_response("rename_function", {
                "address": "0x401000",
                "new_name": "main",
            }),
            _text_response("Done."),
        ])

        registry = _rename_function_registry()

        loop = AgentLoop(
            provider=provider,
            tool_registry=registry,
            config=RikuganConfig(),
            session=SessionState(),
        )

        events = list(loop.run("Rename sub_401000 to main"))

        mutation_events = [e for e in events if e.type == TurnEventType.MUTATION_RECORDED]
        self.assertEqual(len(mutation_events), 1)
        self.assertEqual(mutation_events[0].tool_name, "rename_function")
        self.assertTrue(mutation_events[0].metadata["reversible"])
        self.assertEqual(mutation_events[0].metadata["reverse_tool"], "rename_function")


class TestSpawnSubagentPseudoTool(unittest.TestCase):
    """Verify spawn_subagent pseudo-tool works."""

    def test_subagent_returns_summary(self):
        """spawn_subagent should return text from the subagent."""
        # The subagent will get its own MockProvider, but we're testing the
        # pseudo-tool handler which creates a SubagentRunner.
        # For this test we just verify the tool is recognized and handled.
        provider = MockProvider([
            _tool_call_response("spawn_subagent", {
                "task": "Analyze the main function",
                "max_turns": 5,
            }),
            _text_response("The subagent found the main function."),
        ])

        loop = AgentLoop(
            provider=provider,
            tool_registry=_make_registry(),
            config=RikuganConfig(),
            session=SessionState(),
        )

        events = list(loop.run("Use a subagent to analyze main"))

        # Should have tool_result event for spawn_subagent
        tool_results = [e for e in events if e.type == TurnEventType.TOOL_RESULT]
        subagent_results = [e for e in tool_results if e.tool_name == "spawn_subagent"]
        self.assertTrue(len(subagent_results) >= 1)


if __name__ == "__main__":
    unittest.main()

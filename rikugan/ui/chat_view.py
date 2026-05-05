"""Chat view: scrollable area containing message widgets."""

from __future__ import annotations

import json
import time

from ..agent.turn import TurnEvent, TurnEventType
from ..core.types import Message, Role
from .message_widgets import (
    AssistantMessageWidget,
    ErrorMessageWidget,
    ExplorationFindingWidget,
    ExplorationPhaseWidget,
    QueuedMessageWidget,
    ResearchNoteWidget,
    SubagentEventWidget,
    ThinkingWidget,
    UserMessageWidget,
    UserQuestionWidget,
)
from .plan_view import PlanView
from .qt_compat import (
    QScrollArea,
    QSizePolicy,
    Qt,
    QTimer,
    QVBoxLayout,
    QWidget,
)
from .tool_widgets import ToolApprovalWidget, ToolCallWidget, ToolGroupWidget

_THINKING_MIN_DISPLAY_MS = 500

# Collapse consecutive tool runs once they reach this many calls.
# A single tool call is shown inline with its name visible;
# only 2+ consecutive calls get grouped into a collapsible widget.
_TOOL_GROUP_MIN_CALLS = 2


def _is_hidden_system_user_message(content: str) -> bool:
    """Internal system hints are persisted as user messages but not shown in UI."""
    if not content:
        return False
    return content.lstrip().startswith("[SYSTEM]")


class ChatView(QScrollArea):
    """Scrollable chat area that renders TurnEvents into widgets."""

    def __init__(self, parent: QWidget = None):
        super().__init__(parent)
        self.setObjectName("chat_scroll")
        self.setWidgetResizable(True)
        self.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)

        self._container = QWidget()
        self._container.setObjectName("chat_container")
        # Prevent the container from requesting more width than the viewport;
        # this is critical for word-wrap to work inside a QScrollArea.
        self._container.setSizePolicy(QSizePolicy.Policy.Preferred, QSizePolicy.Policy.Preferred)
        self._layout = QVBoxLayout(self._container)
        self._layout.setContentsMargins(4, 4, 4, 4)
        self._layout.setSpacing(4)
        self._layout.addStretch()
        self.setWidget(self._container)

        # Track current assistant widget for streaming
        self._current_assistant: AssistantMessageWidget | None = None
        self._tool_widgets: dict[str, ToolCallWidget] = {}
        self._thinking: ThinkingWidget | None = None
        self._thinking_shown_at: float = 0.0
        self._plan_view: PlanView | None = None

        # Consecutive tool run state (collapsed when threshold is reached)
        self._tool_run_ids: list[str] = []
        self._tool_run_names: list[str] = []
        self._tool_run_widgets: list[ToolCallWidget] = []
        # Active collapsible group for the current run
        self._tool_group: ToolGroupWidget | None = None
        # Map tool_call_id -> group it belongs to (for result routing/status)
        self._group_map: dict[str, ToolGroupWidget] = {}

        # Member timer for scroll-to-bottom — coalesce at 80ms to reduce
        # layout thrashing during rapid streaming
        self._scroll_timer = QTimer(self)
        self._scroll_timer.setSingleShot(True)
        self._scroll_timer.setInterval(80)
        self._scroll_timer.timeout.connect(self._do_scroll)

        # Timer for minimum thinking display duration (500ms)
        self._thinking_hide_timer = QTimer(self)
        self._thinking_hide_timer.setSingleShot(True)
        self._thinking_hide_timer.timeout.connect(self._force_hide_thinking)

        # Plain Python callbacks avoid extra Qt signal traffic in the hot chat path.
        self._tool_approval_callback = None
        self._user_answer_callback = None

    def set_tool_approval_callback(self, callback) -> None:
        self._tool_approval_callback = callback

    def set_user_answer_callback(self, callback) -> None:
        self._user_answer_callback = callback

    def add_user_message(self, text: str) -> None:
        widget = UserMessageWidget(text, parent=self._container)
        self._insert_widget(widget)
        self._current_assistant = None

    def add_error_message(self, text: str) -> None:
        self._insert_widget(ErrorMessageWidget(text, parent=self._container))
        self._scroll_to_bottom()

    def add_queued_message(self, text: str) -> None:
        self._insert_widget(QueuedMessageWidget(text, parent=self._container))
        self._scroll_to_bottom()

    def remove_queued_messages(self) -> None:
        """Remove all [queued] message widgets (e.g. on cancel)."""
        for i in reversed(range(self._layout.count())):
            item = self._layout.itemAt(i)
            widget = item.widget() if item else None
            if isinstance(widget, QueuedMessageWidget):
                self._layout.removeWidget(widget)
                widget.deleteLater()

    def pop_first_queued_message(self) -> None:
        """Remove the first [queued] widget (when it gets submitted)."""
        for i in range(self._layout.count()):
            item = self._layout.itemAt(i)
            widget = item.widget() if item else None
            if isinstance(widget, QueuedMessageWidget):
                self._layout.removeWidget(widget)
                widget.deleteLater()
                return

    def _show_thinking(self) -> None:
        if self._thinking is not None:
            return
        self._thinking = ThinkingWidget(parent=self._container)
        self._thinking_shown_at = time.monotonic()
        self._insert_widget(self._thinking)
        self._scroll_to_bottom()

    def _hide_thinking(self) -> None:
        if self._thinking is None:
            return
        elapsed_ms = (time.monotonic() - self._thinking_shown_at) * 1000
        if elapsed_ms < _THINKING_MIN_DISPLAY_MS:
            remaining = int(_THINKING_MIN_DISPLAY_MS - elapsed_ms)
            self._thinking_hide_timer.start(remaining)
            return
        self._force_hide_thinking()

    def _force_hide_thinking(self) -> None:
        if self._thinking is None:
            return
        self._thinking.stop()
        self._layout.removeWidget(self._thinking)
        self._thinking.deleteLater()
        self._thinking = None

    def _reset_tool_run(self) -> None:
        """End the current consecutive tool run (state only)."""
        self._tool_group = None
        self._tool_run_ids.clear()
        self._tool_run_names.clear()
        self._tool_run_widgets.clear()

    def _register_tool_widget(self, tool_name: str, tool_id: str, widget: ToolCallWidget) -> None:
        """Attach a new tool widget to the current run, collapsing at threshold."""
        self._tool_run_ids.append(tool_id)
        self._tool_run_names.append(tool_name)
        self._tool_run_widgets.append(widget)

        run_len = len(self._tool_run_widgets)

        # Below threshold: show tool calls directly.
        if self._tool_group is None and run_len < _TOOL_GROUP_MIN_CALLS:
            self._insert_widget(widget)
            return

        # Threshold reached: move entire run into a new collapsible group.
        if self._tool_group is None and run_len == _TOOL_GROUP_MIN_CALLS:
            self._tool_group = ToolGroupWidget()
            self._insert_widget(self._tool_group)

            for idx, run_widget in enumerate(self._tool_run_widgets):
                self._layout.removeWidget(run_widget)
                run_widget.hide_preview()

                run_tool_id = self._tool_run_ids[idx]
                run_tool_name = self._tool_run_names[idx]
                self._tool_group.add_widget(run_widget, run_tool_name)
                self._group_map[run_tool_id] = self._tool_group
            return

        # Already collapsed: add new call directly to existing group.
        widget.hide_preview()
        if self._tool_group is not None:
            self._tool_group.add_widget(widget, tool_name)
            self._group_map[tool_id] = self._tool_group

    def handle_event(self, event: TurnEvent) -> None:
        """Process a TurnEvent and update the UI accordingly."""
        etype = event.type
        if etype in (TurnEventType.TEXT_DELTA, TurnEventType.TEXT_DONE):
            self._handle_text_event(event)
        elif etype in (
            TurnEventType.TOOL_CALL_START,
            TurnEventType.TOOL_CALL_ARGS_DELTA,
            TurnEventType.TOOL_CALL_DONE,
            TurnEventType.TOOL_RESULT,
            TurnEventType.TOOL_APPROVAL_REQUEST,
        ):
            self._handle_tool_event(event)
        elif etype in (
            TurnEventType.TURN_START,
            TurnEventType.TURN_END,
            TurnEventType.CANCELLED,
        ):
            self._handle_lifecycle_event(event)
        elif etype in (
            TurnEventType.PLAN_GENERATED,
            TurnEventType.PLAN_STEP_START,
            TurnEventType.PLAN_STEP_DONE,
        ):
            self._handle_plan_event(event)
        elif etype in (
            TurnEventType.EXPLORATION_PHASE_CHANGE,
            TurnEventType.EXPLORATION_FINDING,
        ):
            self._handle_exploration_event(event)
        elif etype in (
            TurnEventType.RESEARCH_NOTE_SAVED,
            TurnEventType.RESEARCH_NOTE_REVIEWED,
        ):
            self._handle_research_event(event)
        elif etype in (
            TurnEventType.USER_QUESTION,
            TurnEventType.SAVE_APPROVAL_REQUEST,
        ):
            self._handle_question_event(event)
        elif etype in (
            TurnEventType.SUBAGENT_SPAWNED,
            TurnEventType.SUBAGENT_COMPLETED,
            TurnEventType.SUBAGENT_FAILED,
        ):
            self._handle_subagent_event(event)
        elif etype == TurnEventType.ERROR:
            self._hide_thinking()
            self._reset_tool_run()
            self._insert_widget(ErrorMessageWidget(event.error or "Unknown error", parent=self._container))
            self._scroll_to_bottom()

    def _handle_text_event(self, event: TurnEvent) -> None:
        self._hide_thinking()
        self._reset_tool_run()
        if event.type == TurnEventType.TEXT_DELTA:
            if self._current_assistant is None:
                self._current_assistant = AssistantMessageWidget(parent=self._container)
                self._insert_widget(self._current_assistant)
            self._current_assistant.append_text(event.text)
            self._scroll_to_bottom()
        else:  # TEXT_DONE
            if self._current_assistant is not None:
                self._current_assistant.set_text(event.text)
            self._current_assistant = None

    def _handle_tool_event(self, event: TurnEvent) -> None:
        etype = event.type
        if etype == TurnEventType.TOOL_CALL_START:
            self._hide_thinking()
            tw = ToolCallWidget(event.tool_name, event.tool_call_id, parent=self._container)
            self._tool_widgets[event.tool_call_id] = tw
            self._register_tool_widget(event.tool_name, event.tool_call_id, tw)
            self._scroll_to_bottom()
        elif etype == TurnEventType.TOOL_CALL_ARGS_DELTA:
            existing_tw = self._tool_widgets.get(event.tool_call_id)
            if existing_tw is not None:
                existing_tw.append_args_delta(event.tool_args)
        elif etype == TurnEventType.TOOL_CALL_DONE:
            existing_tw = self._tool_widgets.get(event.tool_call_id)
            if existing_tw is not None:
                existing_tw.set_arguments(event.tool_args)
        elif etype == TurnEventType.TOOL_RESULT:
            self._reset_tool_run()
            existing_tw = self._tool_widgets.get(event.tool_call_id)
            if existing_tw is not None:
                existing_tw.set_result(event.tool_result, event.tool_is_error)
            group = self._group_map.get(event.tool_call_id)
            if group:
                group.notify_result(event.tool_is_error)
            self._scroll_to_bottom()
        elif etype == TurnEventType.TOOL_APPROVAL_REQUEST:
            self._hide_thinking()
            self._reset_tool_run()
            widget = ToolApprovalWidget(
                event.tool_call_id,
                event.tool_name,
                event.tool_args,
                event.text,
                parent=self._container,
            )
            widget.set_approved_callback(self._on_tool_approval)
            self._insert_widget(widget)
            self._scroll_to_bottom()

    def _handle_lifecycle_event(self, event: TurnEvent) -> None:
        etype = event.type
        if etype == TurnEventType.TURN_START:
            self._current_assistant = None
            self._reset_tool_run()
            self._group_map.clear()
            self._show_thinking()
            self._scroll_to_bottom()
        elif etype == TurnEventType.TURN_END:
            self._hide_thinking()
            self._reset_tool_run()
            self._current_assistant = None
        elif etype == TurnEventType.CANCELLED:
            self._hide_thinking()
            self._reset_tool_run()
            self._insert_widget(ErrorMessageWidget("Cancelled by user", parent=self._container))
            self._scroll_to_bottom()

    def _handle_plan_event(self, event: TurnEvent) -> None:
        etype = event.type
        if etype == TurnEventType.PLAN_GENERATED:
            self._hide_thinking()
            self._reset_tool_run()
            self._plan_view = PlanView(parent=self._container)
            if event.plan_steps:
                self._plan_view.set_plan(event.plan_steps)

            def _on_plan_approve(pv=self._plan_view):
                pv.set_buttons_visible(False)
                self._on_user_answer("approve")

            def _on_plan_reject(pv=self._plan_view):
                pv.set_buttons_visible(False)
                self._on_user_answer("reject")

            self._plan_view.set_approved_callback(_on_plan_approve)
            self._plan_view.set_rejected_callback(_on_plan_reject)
            self._insert_widget(self._plan_view)
            self._scroll_to_bottom()
        elif etype == TurnEventType.PLAN_STEP_START:
            if self._plan_view:
                self._plan_view.set_step_status(event.plan_step_index, "active")
                self._plan_view.set_buttons_visible(False)
            self._scroll_to_bottom()
        elif etype == TurnEventType.PLAN_STEP_DONE:
            if self._plan_view:
                self._plan_view.set_step_status(event.plan_step_index, "done")
            self._scroll_to_bottom()

    def _handle_exploration_event(self, event: TurnEvent) -> None:
        meta = event.metadata
        if event.type == TurnEventType.EXPLORATION_PHASE_CHANGE:
            self._hide_thinking()
            self._reset_tool_run()
            self._insert_widget(
                ExplorationPhaseWidget(
                    meta.get("from_phase", ""),
                    meta.get("to_phase", ""),
                    event.text,
                    parent=self._container,
                )
            )
        else:  # EXPLORATION_FINDING
            self._insert_widget(
                ExplorationFindingWidget(
                    meta.get("category", "general"),
                    event.text,
                    meta.get("address"),
                    meta.get("relevance", "medium"),
                    parent=self._container,
                )
            )
        self._scroll_to_bottom()

    def _handle_research_event(self, event: TurnEvent) -> None:
        meta = event.metadata
        if event.type == TurnEventType.RESEARCH_NOTE_SAVED:
            self._hide_thinking()
            self._reset_tool_run()
            self._insert_widget(
                ResearchNoteWidget(
                    title=event.text,
                    genre=meta.get("genre", "general"),
                    path=meta.get("path", ""),
                    preview=meta.get("preview", ""),
                    review_passed=meta.get("review_passed", True),
                    parent=self._container,
                )
            )
            self._scroll_to_bottom()
        # RESEARCH_NOTE_REVIEWED — no separate widget, info is in the saved event

    def _handle_subagent_event(self, event: TurnEvent) -> None:
        meta = event.metadata
        if event.type == TurnEventType.SUBAGENT_SPAWNED:
            name = event.text
            agent_type = meta.get("agent_type", "custom")
            self._insert_widget(SubagentEventWidget("spawned", name, f"type: {agent_type}", parent=self._container))
        elif event.type == TurnEventType.SUBAGENT_COMPLETED:
            name = meta.get("name", "")
            turns = meta.get("turn_count", 0)
            elapsed = meta.get("elapsed", 0.0)
            detail = f"{turns} turns, {elapsed:.0f}s"
            self._insert_widget(SubagentEventWidget("completed", name, detail, parent=self._container))
        elif event.type == TurnEventType.SUBAGENT_FAILED:
            name = meta.get("name", "")
            error = event.error or "Unknown error"
            self._insert_widget(SubagentEventWidget("failed", name, error, parent=self._container))
        self._scroll_to_bottom()

    def _handle_question_event(self, event: TurnEvent) -> None:
        self._hide_thinking()
        self._reset_tool_run()
        if event.type == TurnEventType.SAVE_APPROVAL_REQUEST:
            options = ["Save All", "Discard All"]
        else:  # USER_QUESTION
            options = event.metadata.get("options", [])
        widget = UserQuestionWidget(event.text, options, parent=self._container)
        widget.set_option_selected_callback(self._on_user_answer)
        self._insert_widget(widget)
        self._scroll_to_bottom()

    def _on_tool_approval(self, tool_call_id: str, decision: str) -> None:
        """Forward tool approval decision to the panel/controller."""
        if self._tool_approval_callback is not None:
            self._tool_approval_callback(tool_call_id, decision)

    def _on_user_answer(self, answer: str) -> None:
        """Forward a button-selected answer to the panel/controller."""
        if self._user_answer_callback is not None:
            self._user_answer_callback(answer)

    def restore_from_messages(self, messages: list[Message]) -> None:
        """Replay saved Message objects into the chat view."""
        self.clear_chat()

        # Disable Qt layout/paint passes for the duration of the restore.
        # Without this each _insert_widget call triggers a full O(n) layout
        # recalculation on the growing container, making session restore
        # O(n²) in the number of messages.
        self._container.setUpdatesEnabled(False)
        try:
            self._restore_messages_inner(messages)
        finally:
            self._container.setUpdatesEnabled(True)

        self._current_assistant = None
        self._reset_tool_run()
        self._scroll_to_bottom()

    def _restore_messages_inner(self, messages: list[Message]) -> None:
        for msg in messages:
            if msg.role == Role.USER:
                if _is_hidden_system_user_message(msg.content):
                    continue
                self._reset_tool_run()
                self.add_user_message(msg.content)

            elif msg.role == Role.ASSISTANT:
                self._reset_tool_run()
                if msg.content:
                    w = AssistantMessageWidget(parent=self._container)
                    w.set_text(msg.content)
                    self._insert_widget(w)

                for tc in msg.tool_calls:
                    tw = ToolCallWidget(tc.name, tc.id, parent=self._container)
                    try:
                        args_str = json.dumps(tc.arguments, indent=2)
                    except (TypeError, ValueError):
                        args_str = str(tc.arguments)
                    tw.set_arguments(args_str)
                    tw.mark_done()
                    self._tool_widgets[tc.id] = tw
                    self._register_tool_widget(tc.name, tc.id, tw)

            elif msg.role == Role.TOOL:
                self._reset_tool_run()
                for tr in msg.tool_results:
                    existing_tw = self._tool_widgets.get(tr.tool_call_id)
                    if existing_tw is not None:
                        existing_tw.set_result(tr.content, tr.is_error)
                    group = self._group_map.get(tr.tool_call_id)
                    if group:
                        group.notify_result(tr.is_error)

    def clear_chat(self) -> None:
        self._force_hide_thinking()
        self._thinking_hide_timer.stop()
        while self._layout.count() > 1:
            item = self._layout.takeAt(0)
            widget = item.widget()
            if widget:
                widget.deleteLater()
        self._current_assistant = None
        self._tool_widgets.clear()
        self._plan_view = None
        self._reset_tool_run()
        self._group_map.clear()

    def _insert_widget(self, widget: QWidget) -> None:
        """Insert before the stretch at the end."""
        idx = self._layout.count() - 1
        self._layout.insertWidget(idx, widget)

    def resizeEvent(self, event) -> None:
        """Keep the container width pinned to the viewport width.

        QScrollArea.setWidgetResizable(True) handles this when there is no
        horizontal scrollbar, but QLabel rich-text word-wrap still sometimes
        requests a wider sizeHint.  Explicitly clamping here guarantees text
        wraps to the visible area.
        """
        super().resizeEvent(event)
        if self._container is not None:
            self._container.setFixedWidth(self.viewport().width())

    def _is_near_bottom(self) -> bool:
        """True if the user hasn't scrolled up (within ~60px of bottom)."""
        sb = self.verticalScrollBar()
        return sb.maximum() - sb.value() < 60

    def _scroll_to_bottom(self) -> None:
        if self._is_near_bottom():
            self._scroll_timer.start()

    def _do_scroll(self) -> None:
        sb = self.verticalScrollBar()
        sb.setValue(sb.maximum())

    def shutdown(self) -> None:
        self._scroll_timer.stop()
        self._thinking_hide_timer.stop()
        self._force_hide_thinking()
        self._tool_approval_callback = None
        self._user_answer_callback = None

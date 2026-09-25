"""Tests for rikugan.ui.panel_core — pure logic helpers."""

from __future__ import annotations

import inspect
import sys
import types
import unittest
from unittest.mock import MagicMock

from tests.qt_stubs import ensure_pyside6_stubs

ensure_pyside6_stubs()

# Stub heavy rikugan submodules
# Only modules this file had to invent are stubbed. When a real module is
# already imported (test order varies), leave it alone — overwriting its
# functions leaks into every test that runs afterwards.
_created_stubs: set[str] = set()

for _mod_name in [
    "rikugan.ui.styles",
    "rikugan.ui.chat_view",
    "rikugan.ui.input_area",
    "rikugan.ui.context_bar",
    "rikugan.ui.composer",
    "rikugan.ui.panel_header",
    "rikugan.ui.welcome_view",
    "rikugan.ui.tool_widgets",
    "rikugan.core.config",
    "rikugan.core.logging",
    "rikugan.core.types",
    "rikugan.agent.turn",
    "rikugan.agent.mutation",
    "rikugan.providers.auth_cache",
    "rikugan.providers.anthropic_provider",
    "rikugan.providers.ollama_provider",
    "rikugan.providers.registry",
]:
    _stub = sys.modules.get(_mod_name)
    if _stub is None:
        _stub = types.ModuleType(_mod_name)
        sys.modules[_mod_name] = _stub
        _created_stubs.add(_mod_name)
    for _attr in [
        "DARK_THEME",
        "build_chat_drawer_stylesheet",
        "build_chat_sidebar_stylesheet",
        "build_chat_view_stylesheet",
        "build_composer_stylesheet",
        "build_context_bar_stylesheet",
        "build_input_area_stylesheet",
        "build_mode_bar_stylesheet",
        "build_panel_header_stylesheet",
        "build_welcome_stylesheet",
        "build_theme_stylesheet",
        "blend_theme_color",
        "PRODUCT_ACCENT",
        "PRODUCT_BORDER",
        "PRODUCT_MUTED",
        "PRODUCT_PANEL",
        "PRODUCT_SURFACE",
        "PRODUCT_SURFACE_HI",
        "PRODUCT_TEXT",
        "get_chat_color_tokens",
        "get_effective_palette",
        "get_host_font_tokens",
        "get_host_palette_colors",
        "host_stylesheet",
        "maybe_host_stylesheet",
        "use_native_host_theme",
        "ChatView",
        "Composer",
        "InputArea",
        "ContextBar",
        "PanelHeader",
        "WelcomeView",
        "build_suggestions",
        "shorten_model_name",
        "_SharedSpinnerTimer",
        "RikuganConfig",
        "log_error",
        "log_info",
        "log_debug",
        "log_warning",
        "TurnEvent",
        "TurnEventType",
        "MutationRecord",
        "Role",
        "ModelInfo",
        "resolve_auth_cached",
        "resolve_anthropic_auth",
        "DEFAULT_OLLAMA_URL",
        "ProviderRegistry",
    ]:
        if not hasattr(_stub, _attr):
            setattr(_stub, _attr, MagicMock())

# Ensure DEFAULT_OLLAMA_URL is a string (used in comparisons)
_ollama_stub = sys.modules.get("rikugan.providers.ollama_provider")
if _ollama_stub and not isinstance(getattr(_ollama_stub, "DEFAULT_OLLAMA_URL", None), str):
    _ollama_stub.DEFAULT_OLLAMA_URL = "http://localhost:11434"

_styles_stub = sys.modules.get("rikugan.ui.styles")
if _styles_stub is not None and "rikugan.ui.styles" in _created_stubs:
    _styles_stub.blend_theme_color = lambda color_a, color_b, amount: color_a
    _styles_stub.get_host_palette_colors = lambda source=None: {
        "window": "#1e1e1e",
        "window_text": "#d4d4d4",
        "base": "#252526",
        "alt_base": "#2d2d2d",
        "text": "#d4d4d4",
        "button": "#2d2d2d",
        "button_text": "#d4d4d4",
        "highlight": "#569cd6",
        "highlight_text": "#ffffff",
        "mid": "#808080",
        "dark": "#1a1a1a",
        "light": "#f3f3f3",
    }
    _styles_stub.get_chat_color_tokens = lambda source=None: {
        "panel": "#1e1e1e",
        "chat_canvas": "#202020",
        "assistant_bg": "#252525",
        "tool_bg": "#242424",
        "thinking_bg": "#292929",
        "input_bg": "#2b2b2b",
        "text": "#d4d4d4",
        "muted": "#888888",
        "subtle": "#aaaaaa",
        "border": "#555555",
        "accent": "#569cd6",
        "accent_text": "#ffffff",
        "code_bg": "#303030",
    }
    _styles_stub.host_stylesheet = lambda custom_css, native_css="": custom_css
    _styles_stub.build_chat_view_stylesheet = lambda source=None: ""

# Force-remove any stub that test_binja_panel/test_ida_panel may have registered
# so we always import the real module here.
sys.modules.pop("rikugan.ui.panel_core", None)

from rikugan.ui.panel_core import (  # noqa: E402
    _PLACEHOLDER_APPROVAL,
    _PLACEHOLDER_IDLE,
    _PLACEHOLDER_RUNNING,
    _TOOL_RESULT_TRUNCATE_CHARS,
    ChatThreadList,
    RikuganPanelCore,
    _export_detect_lang,
    _export_format_tool_args,
    _export_format_tool_result,
    _parse_function_info_result,
    _parse_function_page,
)

# ---------------------------------------------------------------------------
# _export_detect_lang
# ---------------------------------------------------------------------------


class TestExportDetectLang(unittest.TestCase):
    def test_arg_key_code_returns_python(self):
        self.assertEqual(_export_detect_lang("anything", arg_key="code"), "python")

    def test_arg_key_python_returns_python(self):
        self.assertEqual(_export_detect_lang("anything", arg_key="python"), "python")

    def test_arg_key_c_code_returns_c(self):
        self.assertEqual(_export_detect_lang("anything", arg_key="c_code"), "c")

    def test_arg_key_c_declaration_returns_c(self):
        self.assertEqual(_export_detect_lang("anything", arg_key="c_declaration"), "c")

    def test_arg_key_prototype_returns_c(self):
        self.assertEqual(_export_detect_lang("anything", arg_key="prototype"), "c")

    def test_tool_name_execute_python(self):
        self.assertEqual(_export_detect_lang("x", tool_name="execute_python"), "python")

    def test_tool_name_decompile_function(self):
        self.assertEqual(_export_detect_lang("x", tool_name="decompile_function"), "c")

    def test_tool_name_get_il(self):
        self.assertEqual(_export_detect_lang("x", tool_name="get_il"), "c")

    def test_tool_name_fetch_disassembly(self):
        self.assertEqual(_export_detect_lang("x", tool_name="fetch_disassembly"), "x86asm")

    def test_hexdump_pattern_returns_text(self):
        hexdump = "00000000  48 65 6c 6c 6f 20 57 6f  72 6c 64 0a\n"
        self.assertEqual(_export_detect_lang(hexdump), "text")

    def test_asm_pattern_returns_x86asm(self):
        asm = "mov eax, 0x1234\ncall 0xdeadbeef\n"
        self.assertEqual(_export_detect_lang(asm), "x86asm")

    def test_c_pattern_returns_c(self):
        c_code = "int foo(void) {\n  if (x > 0) { return 1; }\n}"
        self.assertEqual(_export_detect_lang(c_code), "c")

    def test_python_pattern_returns_python(self):
        py_code = "def foo():\n    return 1\nimport os\n"
        self.assertEqual(_export_detect_lang(py_code), "python")

    def test_empty_returns_empty(self):
        self.assertEqual(_export_detect_lang(""), "")

    def test_plain_text_returns_empty(self):
        self.assertEqual(_export_detect_lang("hello world, nothing special"), "")

    def test_arg_key_takes_priority_over_tool_name(self):
        # arg_key check comes first
        result = _export_detect_lang("x", tool_name="execute_python", arg_key="c_code")
        self.assertEqual(result, "c")


class TestParseFunctionPage(unittest.TestCase):
    def test_parses_total_and_rows(self):
        raw = "Functions 250-252 of 1000:\n  0x401000  sub_401000\n  0x401050  main\n"
        functions, total = _parse_function_page(raw)

        self.assertEqual(total, 1000)
        self.assertEqual(
            functions,
            [
                {"address": 0x401000, "name": "sub_401000", "is_import": False, "instruction_count": 0},
                {"address": 0x401050, "name": "main", "is_import": False, "instruction_count": 0},
            ],
        )

    def test_parses_unknown_total(self):
        raw = "Functions 0-1 of unknown:\n  0x10  start\n"
        _functions, total = _parse_function_page(raw)
        self.assertEqual(total, "unknown")


class TestParseFunctionInfoResult(unittest.TestCase):
    def test_parses_function_info_summary(self):
        raw = "Name: main\nAddress: 0x401000 \u2013 0x401050\nSize: 80 bytes\nInstructions: 12\n"
        function = _parse_function_info_result(raw)

        self.assertEqual(
            function,
            {"address": 0x401000, "name": "main", "is_import": False, "instruction_count": 12},
        )

    def test_ignores_no_function_result(self):
        self.assertIsNone(_parse_function_info_result("No function at 0x401000"))


# ---------------------------------------------------------------------------
# _export_format_tool_args
# ---------------------------------------------------------------------------


class TestExportFormatToolArgs(unittest.TestCase):
    def _make_tc(self, name: str, args: dict):
        tc = MagicMock()
        tc.name = name
        tc.arguments = args
        return tc

    def test_short_value_inline(self):
        tc = self._make_tc("tool", {"key": "val"})
        result = _export_format_tool_args(tc)
        self.assertIn("`key`", result)
        self.assertIn("'val'", result)

    def test_long_value_code_block(self):
        long_val = "x" * 100
        tc = self._make_tc("tool", {"code": long_val})
        result = _export_format_tool_args(tc)
        self.assertIn("```python", result)
        self.assertIn(long_val, result)

    def test_multiline_value_code_block(self):
        tc = self._make_tc("tool", {"body": "line1\nline2"})
        result = _export_format_tool_args(tc)
        self.assertIn("```", result)
        self.assertIn("line1\nline2", result)

    def test_empty_args(self):
        tc = self._make_tc("tool", {})
        result = _export_format_tool_args(tc)
        self.assertEqual(result, "")

    def test_multiple_args(self):
        tc = self._make_tc("tool", {"a": "short", "b": "also short"})
        result = _export_format_tool_args(tc)
        self.assertIn("`a`", result)
        self.assertIn("`b`", result)


# ---------------------------------------------------------------------------
# _export_format_tool_result
# ---------------------------------------------------------------------------


class TestExportFormatToolResult(unittest.TestCase):
    def _make_tr(self, content: str, name: str = "tool"):
        tr = MagicMock()
        tr.content = content
        tr.name = name
        return tr

    def test_short_content_not_truncated(self):
        tr = self._make_tr("short content")
        result = _export_format_tool_result(tr)
        self.assertIn("short content", result)
        self.assertNotIn("truncated", result)

    def test_long_content_truncated(self):
        long_content = "A" * (_TOOL_RESULT_TRUNCATE_CHARS + 100)
        tr = self._make_tr(long_content)
        result = _export_format_tool_result(tr)
        self.assertIn("truncated", result)
        self.assertNotIn("A" * (_TOOL_RESULT_TRUNCATE_CHARS + 1), result)

    def test_returns_code_block(self):
        tr = self._make_tr("output")
        result = _export_format_tool_result(tr)
        self.assertIn("```", result)
        self.assertTrue(result.startswith("```"))

    def test_decompile_tool_gets_c_hint(self):
        tr = self._make_tr("int main(void) {}", "decompile_function")
        result = _export_format_tool_result(tr)
        self.assertIn("```c", result)


# ---------------------------------------------------------------------------
# Panel logic via object.__new__ injection
# ---------------------------------------------------------------------------


def _make_panel():
    panel = object.__new__(RikuganPanelCore)
    panel._is_shutdown = False
    panel._polling = False
    panel._pending_answer = False
    panel._pending_answer_tabs = set()
    panel._awaiting_approval_tabs = set()
    panel._chat_views = {}
    panel._chat_area_stack = None
    panel._chat_sidebar = None
    panel._tab_status = {}
    panel._sidebar_rows = {}
    panel._native_mcp_available = False
    panel._native_mcp_active = False
    panel._native_mcp_probe = None
    panel._native_mcp_user_initiated = False
    panel._native_mcp_timer = None
    panel._panel_header = MagicMock()
    panel._tab_approval = {}
    panel._pending_restore_messages = {}
    panel._context_bar = None
    panel._mutation_panel = None
    panel._skills_refresh_timer = None
    panel._poll_timer = None
    panel._restore_timer = None
    panel._restore_queue = None
    panel._binary_timer = None
    panel._binary_queue = None
    panel._input_area = MagicMock()
    panel._composer = MagicMock()
    panel._panel_header = MagicMock()
    panel._chat_column = None
    panel._scrim = None
    panel._drawer_mode = False
    panel._drawer_open = False
    panel._welcome_views = {}
    panel._mutations_btn = MagicMock()
    panel._count_label = MagicMock()
    panel._tab_widget = MagicMock()
    panel._tab_bar = MagicMock()
    panel._ctrl = MagicMock()
    panel._ctrl.active_tab_id = "active"
    panel._ctrl.is_tab_running.return_value = False
    panel._ctrl.tab_pending_count.return_value = 0
    panel._config = MagicMock()
    panel._ui_hooks = None
    panel._awaiting_button_approval = False
    return panel


class TestTabIdAtIndex(unittest.TestCase):
    def test_returns_none_when_widget_is_none(self):
        panel = _make_panel()
        panel._tab_widget.widget.return_value = None
        result = panel._tab_id_at_index(0)
        self.assertIsNone(result)

    def test_returns_tab_id_from_property(self):
        panel = _make_panel()
        mock_widget = MagicMock()
        mock_widget.property.return_value = "tab123"
        panel._chat_views["tab123"] = mock_widget
        panel._tab_widget.widget.return_value = mock_widget
        result = panel._tab_id_at_index(0)
        self.assertEqual(result, "tab123")

    def test_returns_none_when_property_not_in_chat_views(self):
        panel = _make_panel()
        mock_widget = MagicMock()
        mock_widget.property.return_value = "ghost_id"
        # ghost_id not in _chat_views, and widget itself is not in values either
        panel._tab_widget.widget.return_value = mock_widget
        result = panel._tab_id_at_index(0)
        self.assertIsNone(result)

    def test_fallback_to_widget_identity(self):
        panel = _make_panel()
        mock_widget = MagicMock()
        mock_widget.property.return_value = None  # no property
        panel._chat_views["tab_x"] = mock_widget
        panel._tab_widget.widget.return_value = mock_widget
        result = panel._tab_id_at_index(0)
        self.assertEqual(result, "tab_x")


class TestActiveChatView(unittest.TestCase):
    def test_returns_view_for_active_tab(self):
        panel = _make_panel()
        mock_view = MagicMock()
        panel._ctrl.active_tab_id = "t1"
        panel._chat_views["t1"] = mock_view
        self.assertIs(panel._active_chat_view(), mock_view)

    def test_returns_none_when_active_tab_not_in_views(self):
        panel = _make_panel()
        panel._ctrl.active_tab_id = "missing"
        self.assertIsNone(panel._active_chat_view())


class TestSetRunning(unittest.TestCase):
    def test_running_true_switches_composer_to_stop(self):
        panel = _make_panel()
        panel._set_running(True)
        panel._composer.set_running.assert_called_with(True)

    def test_running_false_switches_composer_to_send(self):
        panel = _make_panel()
        panel._set_running(False)
        panel._composer.set_running.assert_called_with(False)

    def test_running_sets_busy_placeholder(self):
        panel = _make_panel()
        panel._set_running(True)
        panel._composer.set_placeholder.assert_called_with(_PLACEHOLDER_RUNNING)

    def test_idle_sets_idle_placeholder(self):
        panel = _make_panel()
        panel._set_running(False)
        panel._composer.set_placeholder.assert_called_with(_PLACEHOLDER_IDLE)

    def test_button_approval_disables_free_text(self):
        panel = _make_panel()
        panel._awaiting_approval_tabs.add("active")
        panel._set_running(False)
        panel._composer.set_input_enabled.assert_called_with(False)
        panel._composer.set_placeholder.assert_called_with(_PLACEHOLDER_APPROVAL)

    def test_running_marks_context_bar(self):
        panel = _make_panel()
        panel._context_bar = MagicMock()
        panel._set_running(True)
        panel._context_bar.set_state.assert_called_with("running")


class TestUpdateTabBarVisibility(unittest.TestCase):
    def test_single_tab_hides_bar(self):
        panel = _make_panel()
        panel._tab_widget.count.return_value = 1
        panel._update_tab_bar_visibility()
        panel._tab_bar.setVisible.assert_called_with(False)

    def test_two_tabs_keeps_legacy_bar_hidden(self):
        panel = _make_panel()
        panel._tab_widget.count.return_value = 2
        panel._update_tab_bar_visibility()
        panel._tab_bar.setVisible.assert_called_with(False)

    def test_zero_tabs_hides_bar(self):
        panel = _make_panel()
        panel._tab_widget.count.return_value = 0
        panel._update_tab_bar_visibility()
        panel._tab_bar.setVisible.assert_called_with(False)


class TestChatSelection(unittest.TestCase):
    def test_select_chat_switches_controller_even_with_hidden_tabs(self):
        panel = _make_panel()
        mock_view = MagicMock()
        panel._chat_views["tid"] = mock_view
        panel._tab_widget.count.return_value = 1
        panel._tab_widget.widget.return_value = mock_view
        panel._select_chat("tid")
        panel._ctrl.switch_tab.assert_called_with("tid")


class TestChatThreadList(unittest.TestCase):
    def test_toolbar_actions_target_selected_chat(self):
        sidebar = object.__new__(ChatThreadList)
        sidebar._selected_tab_id = "tid"
        sidebar._fork_callback = MagicMock()
        sidebar._export_callback = MagicMock()
        sidebar._delete_callback = MagicMock()

        sidebar._on_fork_selected()
        sidebar._on_export_selected()
        sidebar._on_delete_selected()

        sidebar._fork_callback.assert_called_once_with("tid")
        sidebar._export_callback.assert_called_once_with("tid")
        sidebar._delete_callback.assert_called_once_with("tid")

    def test_status_badges_update_row_widget(self):
        sidebar = object.__new__(ChatThreadList)
        item = MagicMock()
        row = MagicMock()
        # Rows now size themselves to the host font, so the mock must answer
        # sizeHint() with a real number.
        row.sizeHint.return_value.height.return_value = 44
        sidebar._items = {"tid": item}
        sidebar._rows = {"tid": row}
        sidebar._titles = {"tid": "Analyze auth"}
        sidebar._details = {"tid": "2 threads"}
        sidebar._statuses = {}
        sidebar._groups = {"tid": ""}
        sidebar._group_items = {}
        sidebar._group_headers = {}
        sidebar._collapsed = set()
        sidebar._search = MagicMock()
        sidebar._search.text.return_value = ""

        sidebar.set_status("tid", "approval")
        item.setText.assert_called_with("")
        row.set_chat.assert_called_with("Analyze auth", "2 threads", "Approval")
        item.setSizeHint.assert_called()

        sidebar.set_status("tid", "error")
        item.setText.assert_called_with("")
        row.set_chat.assert_called_with("Analyze auth", "2 threads", "Error")


class TestComposerActions(unittest.TestCase):
    def test_input_section_builds_the_composer_widget(self):
        source = inspect.getsource(RikuganPanelCore._build_input_section)
        self.assertIn("Composer()", source)
        # The fixed-width Send column is gone: the button lives in the frame.
        self.assertNotIn("setFixedWidth(64)", source)
        self.assertNotIn('QPushButton("Send")', source)

    def test_chat_actions_are_not_a_button_row(self):
        source = inspect.getsource(ChatThreadList._build_header)
        for label in ("Fork", "Export", "Delete", "Settings"):
            self.assertNotIn(f'"{label}"', source)


class TestOnNewTab(unittest.TestCase):
    def test_new_tab_does_not_show_clear_context_dialog(self):
        panel = _make_panel()
        panel._ctrl.create_tab.return_value = "new_tid"
        panel._create_tab = MagicMock()
        panel._show_new_chat_dialog = MagicMock()
        panel._on_new_tab()
        panel._show_new_chat_dialog.assert_not_called()
        panel._create_tab.assert_called_once_with("new_tid", "Untitled")
        panel._ctrl.switch_tab.assert_called_with("new_tid")


class TestOnCloseTab(unittest.TestCase):
    def test_does_not_close_last_tab(self):
        panel = _make_panel()
        panel._tab_widget.count.return_value = 1
        panel._on_close_tab(0)
        panel._ctrl.close_tab.assert_not_called()

    def test_closes_tab_with_multiple(self):
        panel = _make_panel()
        panel._tab_widget.count.return_value = 2
        mock_widget = MagicMock()
        mock_widget.property.return_value = "tid"
        panel._chat_views["tid"] = mock_widget
        panel._tab_widget.widget.return_value = mock_widget
        panel._on_close_tab(0)
        panel._ctrl.close_tab.assert_called_once_with("tid")

    def test_removes_view_from_chat_views(self):
        panel = _make_panel()
        panel._tab_widget.count.return_value = 2
        mock_widget = MagicMock()
        mock_widget.property.return_value = "tid"
        panel._chat_views["tid"] = mock_widget
        panel._tab_widget.widget.return_value = mock_widget
        panel._on_close_tab(0)
        self.assertNotIn("tid", panel._chat_views)


class TestOnToggleMutationLog(unittest.TestCase):
    def test_noop_when_no_panel(self):
        panel = _make_panel()
        panel._mutation_panel = None
        panel._on_toggle_mutation_log()  # must not raise

    def test_shows_when_hidden(self):
        panel = _make_panel()
        mock_mp = MagicMock()
        mock_mp.isVisible.return_value = False
        panel._mutation_panel = mock_mp
        panel._on_toggle_mutation_log()
        mock_mp.setVisible.assert_called_with(True)

    def test_hides_when_visible(self):
        panel = _make_panel()
        mock_mp = MagicMock()
        mock_mp.isVisible.return_value = True
        panel._mutation_panel = mock_mp
        panel._on_toggle_mutation_log()
        mock_mp.setVisible.assert_called_with(False)

    def test_updates_checked_state(self):
        panel = _make_panel()
        mock_mp = MagicMock()
        mock_mp.isVisible.return_value = False
        panel._mutation_panel = mock_mp
        panel._on_toggle_mutation_log()
        panel._mutations_btn.setChecked.assert_called_with(True)


class TestOnUndoRequested(unittest.TestCase):
    def test_noop_when_shutdown(self):
        panel = _make_panel()
        panel._is_shutdown = True
        panel._on_undo_requested(1)
        # _start_agent should not be called — we can check ctrl is not used
        panel._ctrl.start_agent.assert_not_called()

    def test_starts_undo_agent(self):
        panel = _make_panel()
        panel._ctrl.active_tab_id = "t1"
        mock_view = MagicMock()
        panel._chat_views["t1"] = mock_view
        panel._ctrl.start_agent.return_value = None  # no error
        # Pre-inject a mock poll_timer so _ensure_poll_timer returns early
        panel._poll_timer = MagicMock()
        panel._on_undo_requested(2)
        panel._ctrl.start_agent.assert_called_once_with("/undo 2", tab_id="t1")


class TestShutdownIdempotency(unittest.TestCase):
    def test_double_shutdown_safe(self):
        panel = _make_panel()
        panel._poll_timer = None
        panel._skills_refresh_timer = None
        panel._context_bar = None
        panel._ui_hooks = None
        panel.shutdown()
        panel.shutdown()  # second call must not raise or double-cleanup
        panel._ctrl.shutdown.assert_called_once()

    def test_shutdown_calls_ctrl_shutdown(self):
        panel = _make_panel()
        panel._poll_timer = None
        panel._skills_refresh_timer = None
        panel._context_bar = None
        panel._ui_hooks = None
        panel.shutdown()
        panel._ctrl.shutdown.assert_called_once()


class TestStopSkillsRefreshTimer(unittest.TestCase):
    def test_noop_when_timer_none(self):
        panel = _make_panel()
        panel._skills_refresh_timer = None
        panel._stop_skills_refresh_timer()  # must not raise

    def test_clears_timer_ref(self):
        panel = _make_panel()
        mock_timer = MagicMock()
        panel._skills_refresh_timer = mock_timer
        panel._stop_skills_refresh_timer()
        self.assertIsNone(panel._skills_refresh_timer)
        mock_timer.stop.assert_called_once()
        mock_timer.deleteLater.assert_called_once()


class TestRestoreMessagesIfNeeded(unittest.TestCase):
    def test_noop_when_no_pending_restore(self):
        panel = _make_panel()
        mock_view = MagicMock()
        panel._chat_views["t1"] = mock_view
        panel._restore_messages_if_needed("t1")
        mock_view.restore_from_messages.assert_not_called()

    def test_restores_pending_messages_once(self):
        panel = _make_panel()
        mock_view = MagicMock()
        panel._chat_views["t1"] = mock_view
        panel._pending_restore_messages["t1"] = ["m1", "m2"]
        panel._restore_messages_if_needed("t1")
        mock_view.restore_from_messages.assert_called_once_with(["m1", "m2"])
        self.assertNotIn("t1", panel._pending_restore_messages)


class TestUpdateTokenDisplay(unittest.TestCase):
    def test_noop_when_context_bar_none(self):
        panel = _make_panel()
        panel._context_bar = None
        panel._update_token_display(1000)  # must not raise

    def test_calls_set_tokens_with_given_count(self):
        panel = _make_panel()
        mock_cb = MagicMock()
        panel._context_bar = mock_cb
        panel._ctrl.get_context_window.return_value = 200000
        panel._update_token_display(5000)
        mock_cb.set_tokens.assert_called_once_with(5000, 200000)

    def test_zero_context_window_fallback(self):
        panel = _make_panel()
        mock_cb = MagicMock()
        panel._context_bar = mock_cb
        panel._ctrl.get_context_window.return_value = 0
        panel._update_token_display(1234)
        mock_cb.set_tokens.assert_called_once_with(1234, 0)


class TestDontAutoLoadChats(unittest.TestCase):
    def test_add_unloaded_chat_lists_without_chat_view(self):
        panel = _make_panel()
        panel._chat_sidebar = MagicMock()
        panel._ctrl.tab_label.return_value = "My Chat"
        panel._chat_detail = MagicMock(return_value="1 thread")
        session = MagicMock()
        session.messages = [MagicMock(), MagicMock()]

        panel._add_unloaded_chat("tid1", session)

        # Listed in the sidebar and stashed for replay, but NO ChatView built.
        # The 4th argument is the group header (the binary the chat belongs to).
        listed = panel._chat_sidebar.add_chat.call_args.args
        self.assertEqual(listed[:3], ("tid1", "My Chat", "1 thread"))
        self.assertEqual(panel._pending_restore_messages["tid1"], session.messages)
        self.assertNotIn("tid1", panel._chat_views)

    def test_select_unloaded_chat_materializes_view(self):
        panel = _make_panel()
        panel._chat_sidebar = MagicMock()
        panel._chat_area_stack = MagicMock()
        panel._tab_widget.count.return_value = 0
        panel._restore_messages_if_needed = MagicMock()
        panel._update_token_display = MagicMock()
        panel._set_running = MagicMock()
        session = MagicMock()
        session.messages = []
        panel._ctrl.get_session.return_value = session
        panel._ctrl.tab_label.return_value = "Lazy"
        panel._ctrl.is_tab_running.return_value = False

        def fake_create(tab_id, label, add_to_sidebar=True, select=True):
            panel._chat_views[tab_id] = MagicMock()

        panel._create_tab = MagicMock(side_effect=fake_create)

        panel._select_chat("lazytid")

        panel._create_tab.assert_called_once_with("lazytid", "Lazy", add_to_sidebar=False, select=False)
        panel._ctrl.switch_tab.assert_called_with("lazytid")
        panel._restore_messages_if_needed.assert_called_once_with("lazytid")

    def test_select_unknown_chat_is_noop(self):
        panel = _make_panel()
        panel._ctrl.get_session.return_value = None
        panel._create_tab = MagicMock()

        panel._select_chat("ghost")

        panel._create_tab.assert_not_called()


class TestNativeMcpConsent(unittest.TestCase):
    """Binary Ninja's MCP server is offered once per binary, and remembered."""

    def _panel(self):
        panel = _make_panel()
        panel._config.binja_mcp_consent = {}
        panel._config.binja_mcp_url = ""
        panel._ctrl._db_instance_id = "db-1"
        panel._ctrl._idb_path = "/samples/x.bndb"
        return panel

    def test_consent_is_keyed_to_the_database(self):
        panel = self._panel()
        self.assertEqual(panel._native_mcp_key(), "db-1")

    def test_answer_is_written_to_the_config(self):
        panel = self._panel()
        panel._remember_native_mcp_consent(True)
        self.assertEqual(panel._config.binja_mcp_consent["db-1"], True)
        panel._config.save.assert_called()

    def test_toggling_off_stops_the_server_and_remembers_it(self):
        panel = self._panel()
        panel._native_mcp_available = True
        panel._native_mcp_active = True
        panel._stop_native_mcp = MagicMock()
        panel._toggle_native_mcp()
        panel._stop_native_mcp.assert_called_once()
        self.assertEqual(panel._config.binja_mcp_consent["db-1"], False)

    def test_toggling_on_starts_the_server_and_remembers_it(self):
        panel = self._panel()
        panel._native_mcp_available = True
        panel._native_mcp_active = False
        panel._start_native_mcp = MagicMock()
        panel._toggle_native_mcp()
        panel._start_native_mcp.assert_called_once()
        self.assertEqual(panel._config.binja_mcp_consent["db-1"], True)

    def test_the_toggle_does_nothing_without_a_server(self):
        panel = self._panel()
        panel._native_mcp_available = False
        panel._start_native_mcp = MagicMock()
        panel._toggle_native_mcp()
        panel._start_native_mcp.assert_not_called()

    def test_a_remembered_yes_starts_without_asking_again(self):
        panel = self._panel()
        panel._config.binja_mcp_consent = {"db-1": True}
        panel._native_mcp_probe = MagicMock()
        panel._native_mcp_probe.get_nowait.return_value = types.SimpleNamespace(
            available=True, url="u", tool_count=4, error=""
        )
        panel._stop_native_mcp_timer = MagicMock()
        panel._ask_native_mcp_consent = MagicMock()
        panel._start_native_mcp = MagicMock()
        panel._poll_native_mcp()
        panel._ask_native_mcp_consent.assert_not_called()
        panel._start_native_mcp.assert_called_once()

    def test_a_remembered_no_is_honoured_silently(self):
        panel = self._panel()
        panel._config.binja_mcp_consent = {"db-1": False}
        panel._native_mcp_probe = MagicMock()
        panel._native_mcp_probe.get_nowait.return_value = types.SimpleNamespace(
            available=True, url="u", tool_count=4, error=""
        )
        panel._stop_native_mcp_timer = MagicMock()
        panel._ask_native_mcp_consent = MagicMock()
        panel._start_native_mcp = MagicMock()
        panel._poll_native_mcp()
        panel._ask_native_mcp_consent.assert_not_called()
        panel._start_native_mcp.assert_not_called()
        # Still offered on the toggle, so a "no" is never a dead end.
        self.assertTrue(panel._native_mcp_available)

    def test_clicking_the_toggle_reprobes_when_nothing_was_found(self):
        """The server is a plugin; it can be enabled after Rikugan starts."""
        panel = self._panel()
        panel._native_mcp_available = False
        panel._native_mcp_active = False
        panel._detect_native_mcp = MagicMock()
        panel._toggle_native_mcp()
        panel._detect_native_mcp.assert_called_once_with(user_initiated=True)

    def test_a_user_initiated_find_connects_without_asking(self):
        panel = self._panel()
        panel._native_mcp_probe = MagicMock()
        panel._native_mcp_user_initiated = True
        panel._native_mcp_probe.get_nowait.return_value = types.SimpleNamespace(
            available=True, url="u", tool_count=7, error=""
        )
        panel._stop_native_mcp_timer = MagicMock()
        panel._ask_native_mcp_consent = MagicMock()
        panel._start_native_mcp = MagicMock()
        panel._poll_native_mcp()
        panel._ask_native_mcp_consent.assert_not_called()
        panel._start_native_mcp.assert_called_once()
        self.assertTrue(panel._config.binja_mcp_consent["db-1"])

    def test_a_user_initiated_miss_says_so(self):
        panel = self._panel()
        panel._native_mcp_probe = MagicMock()
        panel._native_mcp_user_initiated = True
        panel._native_mcp_probe.get_nowait.return_value = types.SimpleNamespace(
            available=False, url="http://127.0.0.1:24642/mcp", tool_count=0, error="refused"
        )
        panel._stop_native_mcp_timer = MagicMock()
        panel._report_native_mcp_missing = MagicMock()
        panel._poll_native_mcp()
        panel._report_native_mcp_missing.assert_called_once()

    def test_no_server_means_no_prompt_and_no_toggle(self):
        panel = self._panel()
        panel._native_mcp_probe = MagicMock()
        panel._native_mcp_probe.get_nowait.return_value = types.SimpleNamespace(
            available=False, url="u", tool_count=0, error="refused"
        )
        panel._stop_native_mcp_timer = MagicMock()
        panel._ask_native_mcp_consent = MagicMock()
        panel._poll_native_mcp()
        panel._ask_native_mcp_consent.assert_not_called()
        self.assertFalse(panel._native_mcp_available)


if __name__ == "__main__":
    unittest.main()


class TestChatFolders(unittest.TestCase):
    """Chats are grouped by the binary they were opened against.

    A chat from another binary looks like any other in a flat list until you
    type into it, so each file becomes a folder and the ones you are not
    working in fold away.
    """

    def _list(self, groups):
        sidebar = object.__new__(ChatThreadList)
        sidebar._items = {tid: MagicMock() for tid in groups}
        sidebar._titles = {tid: f"chat {tid}" for tid in groups}
        sidebar._details = dict.fromkeys(groups, "")
        sidebar._groups = dict(groups)
        sidebar._group_order = list(dict.fromkeys(groups.values()))
        sidebar._group_items = {g: MagicMock() for g in sidebar._group_order}
        sidebar._group_headers = {g: MagicMock() for g in sidebar._group_order}
        sidebar._collapsed = set()
        sidebar._current_group = ""
        sidebar._search = MagicMock()
        sidebar._search.text.return_value = ""
        return sidebar

    def _hidden(self, sidebar, tab_id):
        return sidebar._items[tab_id].setHidden.call_args[0][0]

    def test_only_the_binary_in_view_stays_open(self):
        sidebar = self._list({"a": "one.bndb", "b": "two.bndb"})
        sidebar.set_current_group("one.bndb")
        self.assertFalse(self._hidden(sidebar, "a"))
        self.assertTrue(self._hidden(sidebar, "b"))

    def test_a_folder_can_be_opened_again(self):
        sidebar = self._list({"a": "one.bndb", "b": "two.bndb"})
        sidebar.set_current_group("one.bndb")
        sidebar.toggle_group("two.bndb")
        self.assertFalse(self._hidden(sidebar, "b"))

    def test_search_reaches_into_folded_folders(self):
        # A chat you cannot find is worse than a folder that opens itself.
        sidebar = self._list({"a": "one.bndb", "b": "two.bndb"})
        sidebar.set_current_group("one.bndb")
        sidebar._search.text.return_value = "chat b"
        sidebar._apply_filter()
        self.assertFalse(self._hidden(sidebar, "b"))

    def test_an_empty_folder_header_is_hidden(self):
        sidebar = self._list({"a": "one.bndb"})
        sidebar._search.text.return_value = "nothing matches"
        sidebar._apply_filter()
        sidebar._group_items["one.bndb"].setHidden.assert_called_with(True)

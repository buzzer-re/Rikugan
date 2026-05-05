"""Shared Rikugan panel widget used by host-specific wrappers."""

from __future__ import annotations

import os
import re
import threading
import time
from collections.abc import Callable
from typing import Any

from ..agent.mutation import MutationRecord
from ..agent.turn import TurnEvent, TurnEventType
from ..core.config import RikuganConfig
from ..core.logging import log_debug, log_error, log_info, log_warning
from ..core.types import Role
from ..providers.auth_cache import resolve_auth_cached
from ..providers.registry import ProviderRegistry
from .chat_view import ChatView
from .context_bar import ContextBar
from .input_area import InputArea
from .mutation_log_view import MutationLogPanel
from .qt_compat import (
    QCheckBox,
    QDialog,
    QDialogButtonBox,
    QFileDialog,
    QHBoxLayout,
    QLabel,
    QMenu,
    QMessageBox,
    QPushButton,
    QSplitter,
    QStackedWidget,
    Qt,
    QTabBar,
    QTabWidget,
    QTimer,
    QToolButton,
    QVBoxLayout,
    QWidget,
    qt_flags,
    qt_run,
)
from .styles import (
    build_small_button_stylesheet,
    build_theme_stylesheet,
    maybe_host_stylesheet,
    use_native_host_theme,
)
from .tool_widgets import _SharedSpinnerTimer
from .tools_panel import ToolsPanel

_TOOL_RESULT_TRUNCATE_CHARS = 2000
_SMALL_BTN_STYLE = (
    "QPushButton { background: #2d2d2d; color: #d4d4d4; border: 1px solid #3c3c3c; "
    "border-radius: 6px; padding: 4px; font-size: 11px; }"
    "QPushButton:hover { background: #3c3c3c; }"
)
_CANCEL_BTN_STYLE = (
    "QPushButton { background: #2d2d2d; color: #c42b1c; border: 1px solid #3c3c3c; "
    "border-radius: 6px; padding: 4px; font-size: 11px; }"
    "QPushButton:hover { background: #3c3c3c; }"
)

_SANITIZER_TAG_RE = re.compile(
    r"^\[The following is (?:a tool execution result|output from an EXTERNAL MCP server)"
    r"[^\]]*\]\n?",
    re.MULTILINE,
)
_SANITIZER_WRAP_RE = re.compile(
    r"<(?:tool_result|mcp_result|binary_data|persistent_memory|skill)\b[^>]*>\n?"
    r"|</(?:tool_result|mcp_result|binary_data|persistent_memory|skill)>\n?",
)


def _strip_sanitizer_tags(text: str) -> str:
    """Remove sanitization wrappers added for the LLM from exported content."""
    text = _SANITIZER_TAG_RE.sub("", text)
    text = _SANITIZER_WRAP_RE.sub("", text)
    return text.strip()


_TOOL_LANG_MAP = {
    "execute_python": "python",
    "decompile_function": "c",
    "get_il": "c",
    "declare_c_type": "c",
    "define_types": "c",
    "set_function_prototype": "c",
    "fetch_disassembly": "x86asm",
}


def _export_detect_lang(text: str, tool_name: str = "", arg_key: str = "") -> str:
    """Detect markdown language hint from content heuristics and tool/arg context."""
    if arg_key in ("code", "python"):
        return "python"
    if arg_key in ("c_code", "c_declaration", "prototype"):
        return "c"
    if tool_name in _TOOL_LANG_MAP:
        return _TOOL_LANG_MAP[tool_name]

    sample = text[:_TOOL_RESULT_TRUNCATE_CHARS]
    if re.search(r"^[0-9a-fA-F]{8,16}\s+([0-9a-fA-F]{2}\s+){4,}", sample, re.M):
        return "text"

    asm_pat = r"(?:mov|lea|push|pop|call|ret|jmp|je|jne|jz|jnz|cmp|test|xor|add|sub|nop|int)\s"
    if re.search(asm_pat, sample, re.I) and re.search(r"0x[0-9a-fA-F]+", sample):
        return "x86asm"

    c_indicators = 0
    if re.search(r"\b(void|int|char|uint\d+_t|int\d+_t|struct|enum|typedef)\b", sample):
        c_indicators += 1
    if re.search(r"[{};]", sample):
        c_indicators += 1
    if re.search(r"\b(if|while|for|return|switch)\s*\(", sample):
        c_indicators += 1
    if c_indicators >= 2:
        return "c"

    if re.search(r"^(def |class |import |from .+ import |print\()", sample, re.M):
        return "python"

    return ""


def _export_format_tool_args(tc) -> str:
    """Format tool call arguments as markdown with per-argument code blocks."""
    parts = []
    for k, v in tc.arguments.items():
        if isinstance(v, str) and ("\n" in v or len(v) > 80):
            lang = _export_detect_lang(v, tc.name, k)
            parts.append(f"  - `{k}`:\n\n```{lang}\n{v}\n```\n")
        else:
            parts.append(f"  - `{k}`: `{v!r}`")
    return "\n".join(parts)


def _export_format_tool_result(tr) -> str:
    """Format tool result content as a markdown code block."""
    content = _strip_sanitizer_tags(tr.content)
    if len(content) > _TOOL_RESULT_TRUNCATE_CHARS:
        content = content[:_TOOL_RESULT_TRUNCATE_CHARS] + "\n... (truncated)"
    lang = _export_detect_lang(content, tr.name)
    return f"```{lang}\n{content}\n```"


def _export_format_subagent_log(messages) -> str:
    """Format a subagent's message log as a collapsible markdown section."""
    tool_count = sum(len(m.tool_calls) for m in messages if m.role == Role.ASSISTANT)
    parts = [
        f"<details>\n<summary>Subagent Log ({tool_count} tool calls)</summary>\n",
    ]
    for msg in messages:
        if msg.role == Role.USER:
            parts.append(f"> **Task**: {msg.content}\n")
        elif msg.role == Role.ASSISTANT:
            if msg.content:
                parts.append(f"> **Subagent**:\n> {msg.content}\n")
            for tc in msg.tool_calls:
                parts.append(f"> **Tool call**: `{tc.name}`\n")
                parts.append(f"> {_export_format_tool_args(tc)}\n")
        elif msg.role == Role.TOOL:
            for tr in msg.tool_results:
                status = "Error" if tr.is_error else "Result"
                parts.append(f"> **{status}** (`{tr.name}`):\n")
                parts.append(f"> {_export_format_tool_result(tr)}\n")
    parts.append("</details>\n")
    return "\n".join(parts)


class _AddButtonTabBar(QTabBar):
    """Tab bar with an integrated '+' button positioned after the last tab."""

    def __init__(self, parent=None):
        super().__init__(parent)
        self._add_tab_callback: Callable[[], None] | None = None
        self._export_tab_callback: Callable[[int], None] | None = None
        self._fork_tab_callback: Callable[[int], None] | None = None
        self.setContextMenuPolicy(Qt.ContextMenuPolicy.CustomContextMenu)
        self.customContextMenuRequested.connect(self._show_context_menu)
        self._add_btn = QToolButton(self)
        self._add_btn.setText("+")
        self._add_btn.setAutoRaise(True)
        self._add_btn.setFixedSize(20, 20)
        self._add_btn.setStyleSheet(
            maybe_host_stylesheet(
                "QToolButton { color: #d4d4d4; font-size: 14px; font-weight: bold; "
                "border: none; background: transparent; }"
                "QToolButton:hover { background: #3c3c3c; border-radius: 3px; }"
            )
        )
        self._add_btn.clicked.connect(self._handle_add_tab)

    def set_add_tab_callback(self, callback: Callable[[], None] | None) -> None:
        self._add_tab_callback = callback

    def set_export_tab_callback(self, callback: Callable[[int], None] | None) -> None:
        self._export_tab_callback = callback

    def set_fork_tab_callback(self, callback: Callable[[int], None] | None) -> None:
        self._fork_tab_callback = callback

    def _handle_add_tab(self) -> None:
        if self._add_tab_callback is not None:
            self._add_tab_callback()

    def _show_context_menu(self, pos):
        index = self.tabAt(pos)
        if index < 0:
            return
        menu = QMenu(self)
        export_action = menu.addAction("Export Chat")
        fork_action = menu.addAction("Fork Session")
        action = qt_run(menu, self.mapToGlobal(pos))
        if action == export_action and self._export_tab_callback is not None:
            self._export_tab_callback(index)
        elif action == fork_action and self._fork_tab_callback is not None:
            self._fork_tab_callback(index)

    def tabInserted(self, index):
        super().tabInserted(index)
        self._reposition()

    def tabRemoved(self, index):
        super().tabRemoved(index)
        self._reposition()

    def resizeEvent(self, event):
        super().resizeEvent(event)
        self._reposition()

    def _reposition(self):
        count = self.count()
        if count > 0:
            rect = self.tabRect(count - 1)
            y = (self.height() - self._add_btn.height()) // 2
            self._add_btn.move(rect.right() + 2, max(0, y))
        else:
            self._add_btn.move(0, 0)


class RikuganPanelCore(QWidget):
    """Host-agnostic chat panel widget."""

    def __init__(
        self,
        controller_factory: Callable[[RikuganConfig], Any],
        ui_hooks_factory: Callable[[Callable[[], Any]], Any] | None = None,
        tools_form_factory: Callable[..., Any] | None = None,
        parent: QWidget = None,
    ):
        super().__init__(parent)
        self._config = RikuganConfig.load_or_create()
        self._use_native_host_theme = use_native_host_theme()
        self._dependency_warnings = ProviderRegistry().dependency_warnings()
        log_debug(
            f"Config loaded: provider={self._config.provider.name} model={self._config.provider.model}",
        )
        for warning in self._dependency_warnings:
            log_warning(f"Dependency warning: {warning}")
        if self._config.has_encrypted_keys():
            self._prompt_decryption_password()
        self._ctrl = controller_factory(self._config)
        self._poll_timer: QTimer | None = None
        self._polling = False
        self._pending_answer = False
        self._awaiting_button_approval = False
        self._is_shutdown = False
        self._ui_hooks_factory = ui_hooks_factory
        self._ui_hooks = None
        self._tools_form_factory = tools_form_factory
        self._tools_form: Any = None  # IDA PluginForm wrapper (if available)

        # Tab-to-ChatView mapping
        self._chat_views: dict[str, ChatView] = {}
        self._pending_restore_messages: dict[str, list] = {}
        self._context_bar: ContextBar | None = None
        self._mutation_panel: MutationLogPanel | None = None
        self._skills_refresh_timer: QTimer | None = None

        self._check_oauth_consent()

        def _warm_oauth() -> None:
            try:
                resolve_auth_cached()
            except Exception as e:
                log_debug(f"OAuth warm-up failed: {e}")

        threading.Thread(target=_warm_oauth, daemon=True).start()
        self._build_ui()

    def _prompt_decryption_password(self) -> None:
        """Prompt for the encryption password at session start."""
        from .qt_compat import QDialog, QDialogButtonBox, QLabel, QLineEdit, QMessageBox, QVBoxLayout

        for _attempt in range(3):
            dlg = QDialog()
            dlg.setWindowTitle("Rikugan — Encrypted API Keys")
            dlg.setMinimumWidth(350)
            layout = QVBoxLayout(dlg)
            layout.addWidget(QLabel("Enter password to decrypt API keys:"))
            pw_edit = QLineEdit()
            pw_edit.setEchoMode(QLineEdit.EchoMode.Password)
            pw_edit.setPlaceholderText("Password")
            layout.addWidget(pw_edit)
            buttons = QDialogButtonBox(
                qt_flags(
                    QDialogButtonBox.StandardButton.Ok,
                    QDialogButtonBox.StandardButton.Cancel,
                ),
            )
            buttons.accepted.connect(dlg.accept)
            buttons.rejected.connect(dlg.reject)
            layout.addWidget(buttons)

            if qt_run(dlg) != QDialog.DialogCode.Accepted:
                break  # user cancelled — keys stay empty
            if self._config.decrypt_stored_keys(pw_edit.text()):
                log_debug("API keys decrypted successfully")
                return
            QMessageBox.warning(None, "Wrong Password", "Incorrect password. Please try again.")
        log_debug("API key decryption skipped or failed — keys will be empty")

    def _check_oauth_consent(self) -> None:
        """Apply persisted OAuth consent to the auth cache.

        The consent dialog itself is only shown from the settings checkbox
        (``_on_oauth_toggled``).  This method just restores the persisted
        state so the warm-up thread knows whether keychain autoload is
        allowed.
        """
        from ..providers.auth_cache import set_keychain_consent

        set_keychain_consent(self._config.oauth_consent_accepted)

    def _ensure_skills_refresh_timer(self) -> None:
        """Refresh skill autocomplete once background discovery completes."""
        if self._skills_refresh_timer is not None:
            return
        self._skills_refresh_timer = QTimer(self)
        self._skills_refresh_timer.setInterval(300)
        self._skills_refresh_timer.timeout.connect(self._refresh_skill_slugs)
        self._skills_refresh_timer.start()

    def _stop_skills_refresh_timer(self) -> None:
        if self._skills_refresh_timer is None:
            return
        self._skills_refresh_timer.stop()
        try:
            self._skills_refresh_timer.timeout.disconnect(self._refresh_skill_slugs)
        except (RuntimeError, TypeError) as e:
            log_debug(f"skills refresh timer disconnect failed: {e}")
        self._skills_refresh_timer.deleteLater()
        self._skills_refresh_timer = None

    def _refresh_skill_slugs(self) -> None:
        if self._is_shutdown:
            self._stop_skills_refresh_timer()
            return
        slugs = self._ctrl.skill_slugs
        if slugs:
            self._input_area.set_skill_slugs(slugs)
            self._stop_skills_refresh_timer()
            return
        if getattr(self._ctrl, "runtime_ready", False):
            # Runtime init completed but no skills found; stop polling.
            self._stop_skills_refresh_timer()

    _MODE_BAR_STYLE = (
        "QTabBar { background: #2d2d2d; border: none; border-bottom: 1px solid #3c3c3c; }"
        "QTabBar::tab { background: #2d2d2d; color: #808080; padding: 4px 16px; "
        "border: none; border-bottom: 2px solid transparent; font-size: 11px; }"
        "QTabBar::tab:selected { color: #d4d4d4; border-bottom: 2px solid #4ec9b0; }"
        "QTabBar::tab:hover:!selected { color: #d4d4d4; }"
    )

    def _build_ui(self) -> None:
        self.setObjectName("rikugan_panel")
        self.setStyleSheet(build_theme_stylesheet(self))

        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(0)

        # Top-level mode switcher: Chat | Tools.
        # Hosts may optionally provide tools in a separate form.
        self._mode_bar = QTabBar()
        self._mode_bar.setObjectName("mode_bar")
        self._mode_bar.setStyleSheet("" if self._use_native_host_theme else self._MODE_BAR_STYLE)
        self._mode_bar.setExpanding(False)
        self._mode_bar.setDrawBase(False)
        self._mode_bar.addTab("Chat")
        self._mode_bar.addTab("Tools")
        self._mode_bar.currentChanged.connect(self._on_mode_changed)
        if self._tools_form_factory is not None:
            self._mode_bar.setVisible(False)
        layout.addWidget(self._mode_bar)

        # Stacked content: page 0 = chat, page 1 = tools
        self._mode_stack = QStackedWidget()
        layout.addWidget(self._mode_stack, 1)

        self._dependency_banner = QLabel()
        self._dependency_banner.setObjectName("dependency_banner")
        self._dependency_banner.setWordWrap(True)
        self._dependency_banner.setStyleSheet(
            maybe_host_stylesheet(
                "QLabel#dependency_banner {"
                "background: #4b3900; color: #f5d98b; border-top: 1px solid #6b5000; "
                "border-bottom: 1px solid #6b5000; padding: 6px 8px; font-size: 11px; }"
            )
        )
        if self._dependency_warnings:
            self._dependency_banner.setText("Warnings: " + " ".join(self._dependency_warnings))
            layout.insertWidget(1, self._dependency_banner)
        else:
            self._dependency_banner.hide()

        # --- Page 0: Chat ---
        chat_page = QWidget()
        chat_layout = QVBoxLayout(chat_page)
        chat_layout.setContentsMargins(0, 0, 0, 0)
        chat_layout.setSpacing(0)
        self._build_tab_widget()
        self._build_main_splitter(chat_layout)
        self._create_tab(self._ctrl.active_tab_id, "New Chat")
        chat_layout.addWidget(self._build_input_section())
        self._mode_stack.addWidget(chat_page)

        # --- Page 1: Tools (lazily populated on first switch) ---
        self._tools_panel: ToolsPanel | None = ToolsPanel()
        self._tools_panel.hide_header()
        if self._tools_form_factory is not None:
            # Separate tools-form hosts keep a lightweight placeholder in the
            # stack so page indices stay stable while tools live elsewhere.
            _tools_placeholder = QWidget()
            self._mode_stack.addWidget(_tools_placeholder)
        else:
            # Binary Ninja: embed directly in the mode stack.
            self._mode_stack.addWidget(self._tools_panel)
        self._tools_tab_index = -1  # kept for IDA compat

        self._context_bar = ContextBar()
        self._context_bar.set_model(self._config.provider.model)
        layout.addWidget(self._context_bar)

        if self._ui_hooks_factory is not None:
            try:
                self._ui_hooks = self._ui_hooks_factory(lambda: self)
                if self._ui_hooks is not None:
                    self._ui_hooks.hook()
            except Exception as e:
                log_debug(f"UI hook setup failed: {e}")
                self._ui_hooks = None

        self._try_restore_session()

    def _build_tab_widget(self) -> None:
        """Create the tab widget with custom tab bar."""
        self._tab_widget = QTabWidget()
        self._tab_bar = _AddButtonTabBar()
        self._tab_widget.setTabBar(self._tab_bar)
        self._tab_widget.setDocumentMode(True)
        self._tab_widget.setTabsClosable(True)
        self._tab_widget.tabCloseRequested.connect(self._on_close_tab)
        self._tab_widget.currentChanged.connect(self._on_tab_changed)
        self._tab_bar.set_add_tab_callback(self._on_new_tab)
        self._tab_bar.set_export_tab_callback(self._on_export_tab)
        self._tab_bar.set_fork_tab_callback(self._on_fork_tab)
        self._tab_widget.setStyleSheet(
            maybe_host_stylesheet(
                "QTabWidget::pane { border: none; }"
                "QTabBar { background: #1e1e1e; border: none; }"
                "QTabBar::tab { background: #252526; color: #cccccc; padding: 2px 8px; "
                "border: none; border-right: 1px solid #3c3c3c; "
                "font-size: 11px; max-width: 140px; }"
                "QTabBar::tab:selected { background: #1e1e1e; color: #ffffff; }"
                "QTabBar::tab:hover { background: #2d2d2d; }"
                "QTabBar::close-button { image: none; border: none; padding: 1px; }"
                "QTabBar::close-button:hover { background: #c42b1c; border-radius: 2px; }"
            )
        )
        self._tab_bar.setExpanding(False)
        self._tab_bar.setVisible(False)  # hidden until 2+ tabs

    def _build_main_splitter(self, layout: QVBoxLayout) -> None:
        """Create the horizontal splitter (chat | mutation log) and add to layout."""
        self._main_splitter = QSplitter(Qt.Orientation.Horizontal)
        self._main_splitter.setHandleWidth(1)
        self._main_splitter.setStyleSheet(maybe_host_stylesheet("QSplitter::handle { background: #3c3c3c; }"))
        self._main_splitter.addWidget(self._tab_widget)

        self._mutation_panel = MutationLogPanel()
        self._mutation_panel.undo_requested.connect(self._on_undo_requested)
        self._mutation_panel.setVisible(False)
        self._main_splitter.addWidget(self._mutation_panel)

        self._main_splitter.setStretchFactor(0, 3)
        self._main_splitter.setStretchFactor(1, 1)

        layout.addWidget(self._main_splitter, 1)

    def _build_input_section(self) -> QWidget:
        """Build the bottom input area with text field and action buttons."""
        self._input_container = QWidget()
        input_layout = QHBoxLayout(self._input_container)
        input_layout.setContentsMargins(8, 4, 8, 4)

        self._input_area = InputArea(self._input_container)
        self._input_area.set_submit_callback(self._on_submit)
        self._input_area.set_cancel_callback(self._on_cancel)
        self._input_area.set_skill_slugs(self._ctrl.skill_slugs)
        self._ensure_skills_refresh_timer()
        input_layout.addWidget(self._input_area, 1)
        input_layout.addLayout(self._build_action_buttons())
        return self._input_container

    def _build_action_buttons(self) -> QVBoxLayout:
        """Build the vertical stack of action buttons (Send, Stop, New, etc.)."""
        btn_layout = QVBoxLayout()
        btn_layout.setSpacing(4)

        self._send_btn = QPushButton("Send")
        self._send_btn.setObjectName("send_button")
        self._send_btn.setFixedWidth(64)
        self._send_btn.setStyleSheet(maybe_host_stylesheet(_SMALL_BTN_STYLE))
        self._send_btn.clicked.connect(self._on_send_clicked)
        btn_layout.addWidget(self._send_btn)
        self._cancel_btn = QPushButton("Stop")
        self._cancel_btn.setObjectName("cancel_button")
        self._cancel_btn.setFixedWidth(64)
        self._cancel_btn.setStyleSheet(maybe_host_stylesheet(_CANCEL_BTN_STYLE))
        self._cancel_btn.setVisible(False)
        self._cancel_btn.clicked.connect(self._on_cancel)
        btn_layout.addWidget(self._cancel_btn)
        self._new_btn = QPushButton("New")
        self._new_btn.setFixedWidth(64)
        self._new_btn.setStyleSheet(maybe_host_stylesheet(_SMALL_BTN_STYLE))
        self._new_btn.clicked.connect(self._on_new_tab)
        btn_layout.addWidget(self._new_btn)
        self._export_btn = QPushButton("Export")
        self._export_btn.setFixedWidth(64)
        self._export_btn.setStyleSheet(maybe_host_stylesheet(_SMALL_BTN_STYLE))
        self._export_btn.clicked.connect(self._on_export_current)
        btn_layout.addWidget(self._export_btn)
        self._settings_btn = QPushButton("Settings")
        self._settings_btn.setFixedWidth(64)
        self._settings_btn.setStyleSheet(maybe_host_stylesheet(_SMALL_BTN_STYLE))
        self._settings_btn.clicked.connect(self._on_settings)
        btn_layout.addWidget(self._settings_btn)
        self._mutations_btn = QPushButton("Mutations")
        self._mutations_btn.setFixedWidth(64)
        self._mutations_btn.setStyleSheet(maybe_host_stylesheet(_SMALL_BTN_STYLE))
        self._mutations_btn.setCheckable(True)
        self._mutations_btn.clicked.connect(self._on_toggle_mutation_log)
        self._mutations_btn.setVisible(False)  # shown when first mutation is recorded
        btn_layout.addWidget(self._mutations_btn)

        self._tools_btn = QPushButton("Tools")
        self._tools_btn.setFixedWidth(64)
        self._tools_btn.setStyleSheet(maybe_host_stylesheet(_SMALL_BTN_STYLE))
        self._tools_btn.setCheckable(True)
        self._tools_btn.clicked.connect(self._on_toggle_tools)
        btn_layout.addWidget(self._tools_btn)

        if self._use_native_host_theme:
            default_btn_style = build_small_button_stylesheet(self)
            danger_btn_style = build_small_button_stylesheet(self, danger=True)
            self._send_btn.setStyleSheet(default_btn_style)
            self._cancel_btn.setStyleSheet(danger_btn_style)
            self._new_btn.setStyleSheet(default_btn_style)
            self._export_btn.setStyleSheet(default_btn_style)
            self._settings_btn.setStyleSheet(default_btn_style)
            self._mutations_btn.setStyleSheet(default_btn_style)
            self._tools_btn.setStyleSheet(default_btn_style)

        btn_layout.addStretch()
        return btn_layout

    # --- Tab management ---

    def _update_tab_bar_visibility(self) -> None:
        """Show the tab bar only when there are 2+ tabs."""
        self._tab_bar.setVisible(self._tab_widget.count() > 1)

    def _create_tab(self, tab_id: str, label: str) -> ChatView:
        """Create a new ChatView and add it as a tab."""
        chat_view = ChatView()
        chat_view.setProperty("tab_id", tab_id)  # O(1) lookup in _tab_id_at_index
        chat_view.set_tool_approval_callback(self._on_tool_approval)
        chat_view.set_user_answer_callback(self._on_user_answer_submitted)
        self._chat_views[tab_id] = chat_view
        index = self._tab_widget.addTab(chat_view, label)
        self._tab_widget.setCurrentIndex(index)
        self._update_tab_bar_visibility()
        return chat_view

    def _on_new_tab(self) -> None:
        """Create a new chat tab, with optional context clearing."""
        if self._is_shutdown:
            return
        session = self._ctrl.session
        has_messages = session and session.messages
        if has_messages:
            ctx_window = self._config.provider.context_window or 200000
            used = (
                session.last_prompt_tokens
                if session.last_prompt_tokens is not None
                else session.total_usage.total_tokens
            )
            pct = min(int(used * 100 / ctx_window), 100) if ctx_window > 0 else 0
            result = self._show_new_chat_dialog(pct)
            if result == "no":
                return
            if result == "clear":
                # Clear current tab instead of creating a new one
                self._ctrl.new_chat()
                chat_view = self._active_chat_view()
                if chat_view:
                    chat_view.clear_chat()
                self._update_token_display(0)
                self._update_tab_label(self._ctrl.active_tab_id)
                return
            # "yes" — fall through to create a new tab
        tab_id = self._ctrl.create_tab()
        self._create_tab(tab_id, "New Chat")
        self._ctrl.switch_tab(tab_id)

    def _on_fork_tab(self, index: int) -> None:
        """Fork (duplicate) a session into a new tab."""
        source_tab_id = self._tab_id_at_index(index)
        if source_tab_id is None:
            return
        new_tab_id = self._ctrl.fork_session(source_tab_id)
        if new_tab_id is None:
            return
        label = self._ctrl.tab_label(new_tab_id)
        chat_view = self._create_tab(new_tab_id, f"{label} (fork)")
        # Restore messages into the forked chat view
        source_session = self._ctrl.get_session(new_tab_id)
        if source_session and source_session.messages:
            chat_view.restore_from_messages(source_session.messages)
        self._ctrl.switch_tab(new_tab_id)
        log_info(f"Forked tab {source_tab_id} → {new_tab_id}")

    def _on_close_tab(self, index: int) -> None:
        """Close a tab. Prevents closing the last tab."""
        if self._tab_widget.count() <= 1:
            return  # Don't close the last tab
        tab_id = self._tab_id_at_index(index)
        if tab_id is None:
            return
        self._ctrl.close_tab(tab_id)
        chat_view = self._chat_views.pop(tab_id, None)
        self._tab_widget.removeTab(index)
        if chat_view:
            chat_view.shutdown()
            chat_view.deleteLater()
        self._update_tab_bar_visibility()

    def _on_export_tab(self, index: int) -> None:
        """Export a tab's chat to a Markdown file."""
        tab_id = self._tab_id_at_index(index)
        if tab_id is None:
            return
        session = self._ctrl.get_session(tab_id)
        if session is None or not session.messages:
            return

        # Show export options dialog if there are subagent logs
        include_subagents = False
        if session.subagent_logs:
            dlg = QDialog(self)
            dlg.setWindowTitle("Export Options")
            dlg.setStyleSheet(
                maybe_host_stylesheet(
                    "QDialog { background: #1e1e1e; }"
                    "QLabel { color: #d4d4d4; font-size: 12px; }"
                    "QCheckBox { color: #d4d4d4; font-size: 12px; }"
                )
            )
            layout = QVBoxLayout(dlg)
            cb = QCheckBox(f"Include subagent logs ({len(session.subagent_logs)} subagent runs)")
            cb.setChecked(True)
            layout.addWidget(cb)
            buttons = QDialogButtonBox(
                qt_flags(
                    QDialogButtonBox.StandardButton.Ok,
                    QDialogButtonBox.StandardButton.Cancel,
                )
            )
            buttons.accepted.connect(dlg.accept)
            buttons.rejected.connect(dlg.reject)
            layout.addWidget(buttons)
            if not qt_run(dlg):
                return
            include_subagents = cb.isChecked()

        label = self._ctrl.tab_label(tab_id).replace("/", "-").replace("\\", "-")
        default_name = f"rikugan-{label}-{time.strftime('%Y%m%d-%H%M%S')}.md"
        path, _ = QFileDialog.getSaveFileName(
            self,
            "Export Chat",
            default_name,
            "Markdown (*.md);;Text (*.txt);;All Files (*)",
        )
        if not path:
            return
        try:
            self._export_session_to_file(session, path, include_subagents=include_subagents)
            log_info(f"Exported chat to {path}")
        except Exception as e:
            log_error(f"Failed to export chat: {e}")

    @staticmethod
    def _export_session_to_file(
        session,
        path: str,
        include_subagents: bool = False,
    ) -> None:
        """Write session messages to a Markdown file."""
        lines = ["# Rikugan Chat Export\n"]
        lines.append(f"- **Model**: {session.model_name or 'unknown'}")
        lines.append(f"- **Exported**: {time.strftime('%Y-%m-%d %H:%M:%S')}")
        if session.idb_path:
            lines.append(f"- **File**: `{os.path.basename(session.idb_path)}`")
        lines.append("")
        lines.append("---\n")

        subagent_logs = session.subagent_logs if include_subagents else {}

        for msg in session.messages:
            if msg.role == Role.USER:
                lines.append(f"## You\n\n{msg.content}\n")
            elif msg.role == Role.ASSISTANT:
                if msg.content:
                    lines.append(f"## Rikugan\n\n{msg.content}\n")
                for tc in msg.tool_calls:
                    lines.append(f"**Tool call**: `{tc.name}`\n")
                    lines.append(_export_format_tool_args(tc))
                    lines.append("")
            elif msg.role == Role.TOOL:
                for tr in msg.tool_results:
                    status = "Error" if tr.is_error else "Result"
                    lines.append(f"**{status}** (`{tr.name}`):\n")
                    lines.append(_export_format_tool_result(tr))
                    lines.append("")
                    # Insert subagent log after the spawn_subagent result
                    if tr.name == "spawn_subagent" and tr.tool_call_id in subagent_logs:
                        lines.append(
                            _export_format_subagent_log(
                                subagent_logs[tr.tool_call_id],
                            )
                        )

        # Append exploration subagent logs that aren't tied to a tool_call_id
        if include_subagents:
            for key, msgs in subagent_logs.items():
                if key.startswith("exploration_"):
                    lines.append("\n---\n\n### Exploration Subagent Log\n")
                    lines.append(_export_format_subagent_log(msgs))

        with open(path, "w", encoding="utf-8") as f:
            f.write("\n".join(lines))

    def _on_export_current(self) -> None:
        """Export the currently active tab's chat."""
        index = self._tab_widget.currentIndex()
        if index >= 0:
            self._on_export_tab(index)

    def _on_tab_changed(self, index: int) -> None:
        """Handle tab switch."""
        if index < 0 or self._is_shutdown:
            return
        tab_id = self._tab_id_at_index(index)
        if tab_id is None:
            return
        self._ctrl.switch_tab(tab_id)
        self._restore_messages_if_needed(tab_id)
        self._update_token_display()

    def _tab_id_at_index(self, index: int) -> str | None:
        """Find the tab_id for a given tab index via the stored property (O(1))."""
        widget = self._tab_widget.widget(index)
        if widget is None:
            return None
        tid = widget.property("tab_id")
        if tid and tid in self._chat_views:
            return tid
        # Fallback for tabs created before property was set
        for tid, cv in self._chat_views.items():
            if cv is widget:
                return tid
        return None

    def _active_chat_view(self) -> ChatView | None:
        """Return the ChatView for the currently active tab."""
        return self._chat_views.get(self._ctrl.active_tab_id)

    def _restore_messages_if_needed(self, tab_id: str) -> None:
        """Replay deferred restored messages for a tab the first time it is shown."""
        messages = self._pending_restore_messages.pop(tab_id, None)
        if not messages:
            return
        chat_view = self._chat_views.get(tab_id)
        if chat_view is not None:
            chat_view.restore_from_messages(messages)

    def _update_token_display(self, token_count: int | None = None) -> None:
        """Update the context bar token display with context window percentage."""
        if self._context_bar is None:
            return
        if token_count is None:
            session = self._ctrl.session
            # Show current context size (last prompt), not cumulative total
            token_count = (
                session.last_prompt_tokens
                if session.last_prompt_tokens is not None
                else session.total_usage.total_tokens
            )
        ctx_window = self._config.provider.context_window or 0
        self._context_bar.set_tokens(token_count, ctx_window)

    def _update_tab_label(self, tab_id: str) -> None:
        """Update tab label from the first user message."""
        label = self._ctrl.tab_label(tab_id)
        cv = self._chat_views.get(tab_id)
        if cv is None:
            return
        for i in range(self._tab_widget.count()):
            if self._tab_widget.widget(i) is cv:
                self._tab_widget.setTabText(i, label)
                break

    # --- Public API ---

    def prefill_input(self, text: str, auto_submit: bool = False) -> None:
        if self._is_shutdown:
            return
        self._input_area.setPlainText(text)
        if auto_submit:
            self._input_area.clear()
            self._on_submit(text)
        else:
            self._input_area.setFocus()

    def shutdown(self) -> None:
        if self._is_shutdown:
            return
        self._is_shutdown = True
        try:
            tools_form = getattr(self, "_tools_form", None)
            tools_panel = getattr(self, "_tools_panel", None)
            self._stop_poll_timer()
            self._stop_skills_refresh_timer()
            _SharedSpinnerTimer.shutdown()
            if self._context_bar:
                self._context_bar.stop()
            for cv in self._chat_views.values():
                cv.shutdown()
            if self._ui_hooks:
                self._ui_hooks.unhook()
                self._ui_hooks = None
            if tools_form is not None:
                tools_form.hide()
                # In IDA mode, hide() orphans the tools widget via
                # OnClose -> setParent(None).  Schedule it for deletion
                # while Python is still alive to prevent crashes during
                # QApplication::~QApplication() exit cleanup.
                if tools_panel is not None:
                    tools_panel.deleteLater()
            elif tools_panel is not None:
                tools_panel.close()
            self._tools_panel = None
            self._ctrl.shutdown()
        except Exception as e:
            log_error(f"Panel teardown error: {e}")

    def on_database_changed(self, new_path: str) -> None:
        """Called when the user opens a different file."""
        if self._is_shutdown:
            return
        normalized = os.path.normcase(os.path.realpath(os.path.abspath(new_path))) if new_path else ""
        if normalized == self._ctrl._idb_path:
            return
        self._ctrl.reset_for_new_file(normalized)
        # Remove all existing tabs
        for cv in self._chat_views.values():
            cv.shutdown()
        while self._tab_widget.count():
            w = self._tab_widget.widget(0)
            self._tab_widget.removeTab(0)
            if w:
                w.deleteLater()
        self._chat_views.clear()
        self._pending_restore_messages.clear()
        # Create default tab and try to restore saved sessions
        self._create_tab(self._ctrl.active_tab_id, "New Chat")
        self._try_restore_session()

    def _on_submit(self, text: str) -> None:
        if not text or self._is_shutdown:
            return
        chat_view = self._active_chat_view()
        if chat_view is None:
            return
        # Block free-text when awaiting button-only approval (plan/save).
        if self._awaiting_button_approval:
            log_debug(f"Ignoring text input while awaiting button approval: {text!r}")
            return
        if self._pending_answer:
            self._pending_answer = False
            chat_view.add_user_message(text)
            self._set_running(True)
            runner = self._ctrl.get_runner()
            if runner:
                runner.agent_loop.submit_user_answer(text)
            return
        # Queue while the agent is actively running.
        if self._ctrl.is_agent_running:
            self._ctrl.queue_message(text)
            chat_view.add_queued_message(text)
            return
        self._start_agent(text)

    def _on_send_clicked(self) -> None:
        text = self._input_area.toPlainText().strip()
        if text:
            self._input_area.clear()
            self._on_submit(text)

    def _on_cancel(self) -> None:
        if self._is_shutdown:
            return
        self._pending_answer = False
        self._awaiting_button_approval = False
        self._ctrl.cancel()
        # Remove [queued] widgets from the active chat view
        chat_view = self._active_chat_view()
        if chat_view is not None:
            chat_view.remove_queued_messages()

    def _on_settings(self) -> None:
        try:
            from .settings_dialog import SettingsDialog

            dlg = SettingsDialog(
                self._config,
                registry=self._ctrl.provider_registry,
                tool_registry=self._ctrl.tool_registry,
            )
            result = qt_run(dlg)
            if result:
                self._config.save(password=dlg.encryption_password)
                self._ctrl.update_settings()
                self._ctrl.reload_mcp()
                if self._context_bar is not None:
                    self._context_bar.set_model(self._config.provider.model)
                log_info(f"Settings updated: {self._config.provider.name}/{self._config.provider.model}")
            dlg.setParent(None)
        except Exception as e:
            log_error(f"Settings dialog error: {e}")

    def _show_new_chat_dialog(self, context_pct: int) -> str:
        """Show a confirmation dialog with context usage. Returns 'yes', 'clear', or 'no'."""
        dlg = QMessageBox(self)
        dlg.setWindowTitle("New Chat")
        dlg.setText("Start a new chat? Current conversation will be saved.")
        dlg.setInformativeText(f"Context usage: {context_pct}%")
        dlg.setStyleSheet(
            maybe_host_stylesheet(
                "QMessageBox { background: #1e1e1e; color: #d4d4d4; }"
                "QLabel { color: #d4d4d4; font-size: 12px; }"
                "QPushButton { background: #2d2d2d; color: #d4d4d4; border: 1px solid #3c3c3c; "
                "border-radius: 4px; padding: 6px 16px; font-size: 11px; min-width: 80px; }"
                "QPushButton:hover { background: #3c3c3c; }"
            )
        )
        yes_btn = dlg.addButton("Yes", QMessageBox.ButtonRole.AcceptRole)
        clear_btn = dlg.addButton(
            f"Yes, clear context ({context_pct}% used)",
            QMessageBox.ButtonRole.AcceptRole,
        )
        no_btn = dlg.addButton("No", QMessageBox.ButtonRole.RejectRole)
        dlg.setDefaultButton(no_btn)
        qt_run(dlg)
        clicked = dlg.clickedButton()
        if clicked is clear_btn:
            return "clear"
        if clicked is yes_btn:
            return "yes"
        return "no"

    def _start_agent(self, user_message: str) -> None:
        chat_view = self._active_chat_view()
        if chat_view is None:
            return
        chat_view.add_user_message(user_message)
        self._set_running(True)

        # Update tab label after first user message
        self._update_tab_label(self._ctrl.active_tab_id)

        error = self._ctrl.start_agent(user_message)
        if error:
            chat_view.add_error_message(error)
            self._set_running(False)
            return

        self._ensure_poll_timer()
        assert self._poll_timer is not None
        self._poll_timer.start(50)

    def _ensure_poll_timer(self) -> None:
        if self._poll_timer is not None:
            return
        self._poll_timer = QTimer(self)
        self._poll_timer.timeout.connect(self._poll_events)

    def _stop_poll_timer(self) -> None:
        if self._poll_timer is not None:
            self._poll_timer.stop()
            try:
                self._poll_timer.timeout.disconnect(self._poll_events)
            except (RuntimeError, TypeError) as e:
                log_debug(f"panel_core timer disconnect failed: {e}")
            self._poll_timer.deleteLater()
            self._poll_timer = None

    def _poll_events(self) -> None:
        if self._polling or self._is_shutdown:
            return
        self._polling = True
        try:
            chat_view = self._active_chat_view()
            container = chat_view._container if chat_view is not None else None
            # Defer layout/paint passes until the whole batch is processed.
            # When 3 tools complete between ticks, each TOOL_RESULT makes a hidden
            # widget visible which triggers an O(n-widgets) layout cascade on the
            # chat container.  Batching those into one final pass cuts this from
            # O(k·n) to O(n) per tick.
            if container is not None:
                container.setUpdatesEnabled(False)
            try:
                for _ in range(30):
                    event = self._ctrl.get_event(timeout=0)
                    if event is None:
                        if not self._ctrl.is_agent_running:
                            self._on_agent_finished()
                        return
                    self._on_event(event)
            finally:
                if container is not None:
                    container.setUpdatesEnabled(True)
        finally:
            self._polling = False

    def _on_event(self, event: TurnEvent) -> None:
        if self._is_shutdown:
            return
        chat_view = self._active_chat_view()
        if chat_view is None:
            return
        chat_view.handle_event(event)
        if event.usage:
            # Use prompt_tokens from the event directly — session hasn't
            # been updated yet during streaming, so session.last_prompt_tokens
            # would be stale.  prompt_tokens reflects current context size.
            token_count = event.usage.context_tokens if event.usage.context_tokens > 0 else event.usage.total_tokens
            if token_count > 0:
                self._update_token_display(token_count)
        if event.type in (
            TurnEventType.USER_QUESTION,
            TurnEventType.SAVE_APPROVAL_REQUEST,
            TurnEventType.PLAN_GENERATED,
        ):
            self._pending_answer = True
            # Plan approvals, save approvals, and any question with
            # predefined options MUST be answered via buttons only.
            # Disable text input so free-text ("continue", "redo", etc.)
            # cannot bypass the approval gate.
            has_options = bool(event.metadata.get("options")) if event.metadata else False
            allow_text = bool(event.metadata.get("allow_text")) if event.metadata else False
            needs_button = event.type in (
                TurnEventType.PLAN_GENERATED,
                TurnEventType.SAVE_APPROVAL_REQUEST,
            ) or (has_options and not allow_text)
            if needs_button:
                self._awaiting_button_approval = True
            self._set_running(False)
        if event.type == TurnEventType.MUTATION_RECORDED:
            self._on_mutation_recorded(event)

    def _on_tool_approval(self, tool_call_id: str, decision: str) -> None:
        """Forward tool approval to the agent loop."""
        runner = self._ctrl.get_runner()
        if runner:
            runner.agent_loop.submit_tool_approval(decision)

    def _on_user_answer_submitted(self, answer: str) -> None:
        """Handle a button click from UserQuestionWidget (plan/save/ask_user)."""
        if not self._pending_answer:
            return
        self._pending_answer = False
        self._awaiting_button_approval = False
        chat_view = self._active_chat_view()
        if chat_view is not None:
            chat_view.add_user_message(answer)
        self._set_running(True)
        runner = self._ctrl.get_runner()
        if runner:
            runner.agent_loop.submit_user_answer(answer)

    def _on_agent_finished(self) -> None:
        if self._is_shutdown:
            return
        if self._poll_timer:
            self._poll_timer.stop()

        # Clear approval state — if the agent crashed mid-approval the
        # buttons are stale and free-text input must be restored.
        self._pending_answer = False
        self._awaiting_button_approval = False

        self._ctrl.on_agent_finished()
        # Remove any [queued] widgets since the queue was cleared.
        chat_view = self._active_chat_view()
        if chat_view is not None:
            chat_view.remove_queued_messages()
        self._set_running(False)

    def _try_restore_session(self) -> None:
        restored = self._ctrl.restore_sessions()
        if restored:
            # Remove the default empty tab if it was replaced
            for tid, cv in list(self._chat_views.items()):
                if tid not in self._ctrl.tab_ids:
                    # This tab was removed during restore
                    for i in range(self._tab_widget.count()):
                        if self._tab_widget.widget(i) is cv:
                            self._tab_widget.removeTab(i)
                            break
                    cv.shutdown()
                    cv.deleteLater()
                    del self._chat_views[tid]

            for tab_id, session in restored:
                label = self._ctrl.tab_label(tab_id)
                self._pending_restore_messages[tab_id] = session.messages
                self._create_tab(tab_id, label)

            # Activate the last (most recent) tab
            if restored:
                last_tab_id = restored[-1][0]
                last_cv = self._chat_views.get(last_tab_id)
                if last_cv:
                    for i in range(self._tab_widget.count()):
                        if self._tab_widget.widget(i) is last_cv:
                            self._tab_widget.setCurrentIndex(i)
                            break
                    self._restore_messages_if_needed(last_tab_id)
                self._update_token_display()
        else:
            # No saved sessions — try legacy single-session restore
            session = self._ctrl.restore_session()
            if session:
                legacy_cv = self._active_chat_view()
                if legacy_cv:
                    legacy_cv.restore_from_messages(session.messages)
                self._update_token_display()

    # --- Mutation log integration ---

    def _on_mutation_recorded(self, event: TurnEvent) -> None:
        """Handle a MUTATION_RECORDED event by adding it to the mutation log panel."""
        if self._mutation_panel is None:
            return
        meta = event.metadata
        record = MutationRecord(
            tool_name=event.tool_name,
            arguments={},
            reverse_tool=meta.get("reverse_tool", ""),
            reverse_arguments=meta.get("reverse_args", {}),
            description=event.text,
            reversible=meta.get("reversible", False),
        )
        self._mutation_panel.add_mutation(record)
        # Show the mutations button once the first mutation is recorded
        self._mutations_btn.setVisible(True)

    def _on_toggle_mutation_log(self) -> None:
        """Toggle visibility of the mutation log panel."""
        if self._mutation_panel is None:
            return
        visible = not self._mutation_panel.isVisible()
        self._mutation_panel.setVisible(visible)
        self._mutations_btn.setChecked(visible)

    def _on_mode_changed(self, index: int) -> None:
        """Handle the Chat / Tools mode bar switch."""
        self._mode_stack.setCurrentIndex(index)
        if index == 1:
            self._ensure_tools_initialized()
            self._tools_btn.setChecked(True)
        else:
            self._tools_btn.setChecked(False)

    def _on_toggle_tools(self) -> None:
        """Toggle the Tools view (IDA-docked or embedded mode tab)."""
        if self._tools_panel is None:
            return
        self._ensure_tools_initialized()

        if self._tools_form is not None:
            # IDA dockable form
            if self._tools_form.is_visible:
                self._tools_form.hide()
                self._tools_btn.setChecked(False)
            else:
                self._tools_form.show()
                self._tools_btn.setChecked(True)
        else:
            # Toggle mode bar between Chat (0) and Tools (1)
            current = self._mode_bar.currentIndex()
            self._mode_bar.setCurrentIndex(1 if current == 0 else 0)

    def show_tools_panel(self, tab_index: int = 0) -> None:
        """Show the tools view and switch to the given tab.

        Public API used by IDA actions (Open Tools, Send to Bulk Rename).
        """
        if self._tools_panel is None:
            return
        self._ensure_tools_initialized()

        if self._tools_form is not None:
            self._tools_form.show()
            self._tools_form.set_tab(tab_index)
        else:
            self._mode_bar.setCurrentIndex(1)
            if hasattr(self._tools_panel, "_tabs"):
                self._tools_panel._tabs.setCurrentIndex(tab_index)
        self._tools_btn.setChecked(True)

    def show_tools_with_renamer(self, address: int | None = None) -> None:
        """Show the tools panel on the Renamer tab.

        If *address* is given, filter and check that function.
        Called from the IDA "Send to Bulk Rename" right-click action.
        """
        self.show_tools_panel(tab_index=0)
        if address is not None and hasattr(self, "_bulk_renamer"):
            self._bulk_renamer.select_and_filter_address(address)

    def _ensure_tools_initialized(self) -> None:
        """Lazily initialize tools panel contents on first open."""
        if getattr(self, "_tools_initialized", False):
            return
        if self._tools_panel is None:
            return
        self._tools_initialized = True

        from .agent_tree import AgentTreeWidget
        from .bulk_renamer import BulkRenamerWidget

        # Agent tree
        self._agent_tree = AgentTreeWidget()
        self._agent_tree.cancel_requested.connect(self._on_cancel_agent)
        self._agent_tree.inject_summary_requested.connect(self._on_inject_summary)
        self._tools_panel.set_agents_widget(self._agent_tree)

        # Bulk renamer
        self._bulk_renamer = BulkRenamerWidget()
        self._bulk_renamer.start_requested.connect(self._on_renamer_start)
        self._bulk_renamer.pause_requested.connect(self._on_renamer_pause)
        self._bulk_renamer.cancel_requested.connect(self._on_renamer_cancel)
        self._bulk_renamer.undo_requested.connect(self._on_renamer_undo)
        self._bulk_renamer.seek_requested.connect(lambda addr: self._on_renamer_seek(addr))
        self._tools_panel.set_renamer_widget(self._bulk_renamer)

        # Create IDA dockable form wrapper if factory is available
        if self._tools_form_factory is not None and self._tools_form is None:
            self._tools_form = self._tools_form_factory(self._tools_panel)

        # Populate bulk renamer with functions from the binary.
        # Defer to next event-loop tick so the panel paints first.
        QTimer.singleShot(0, self._load_renamer_functions)

        # Start tools polling timer
        self._tools_poll_timer = QTimer(self)
        self._tools_poll_timer.setInterval(100)
        self._tools_poll_timer.timeout.connect(self._poll_tools_events)
        self._tools_poll_timer.start()

    def _get_or_create_subagent_manager(self):
        """Lazily create the SubagentManager."""
        if hasattr(self, "_subagent_manager"):
            return self._subagent_manager

        from ..agent.subagent_manager import SubagentManager

        provider = self._ctrl.get_provider()
        if provider is None:
            return None
        self._subagent_manager = SubagentManager(
            provider=provider,
            tool_registry=self._ctrl.get_tool_registry(),
            config=self._config,
            host_name=self._ctrl.host_name,
            skill_registry=getattr(self._ctrl, "_skill_registry", None),
        )
        return self._subagent_manager

    def _get_or_create_renamer_engine(self, batch_size: int, max_workers: int):
        """Create a BulkRenamerEngine for the current session."""
        from ..agent.bulk_renamer import BulkRenamerEngine

        provider = self._ctrl.get_provider()
        if provider is None:
            return None
        return BulkRenamerEngine(
            provider=provider,
            tool_registry=self._ctrl.get_tool_registry(),
            config=self._config,
            host_name=self._ctrl.host_name,
            skill_registry=getattr(self._ctrl, "_skill_registry", None),
            batch_size=batch_size,
            max_workers=max_workers,
            subagent_manager=self._get_or_create_subagent_manager(),
        )

    def _load_renamer_functions(self) -> None:
        """Populate the bulk renamer widget with functions from the binary.

        Fetches pages of functions one at a time via QTimer so the UI thread
        stays responsive between pages (avoids blocking on large binaries).
        """
        if not hasattr(self, "_bulk_renamer"):
            return

        tool_registry = self._ctrl.get_tool_registry()
        defn = tool_registry.get("list_functions")
        if defn is None or defn.handler is None:
            log_info("list_functions tool not available — renamer table will be empty")
            return

        # State for the incremental page fetcher
        self._renamer_load_funcs: list[dict] = []
        self._renamer_load_offset = 0
        self._renamer_load_batch = 500
        self._renamer_load_defn = defn

        self._renamer_fetch_timer = QTimer(self)
        self._renamer_fetch_timer.setInterval(0)
        self._renamer_fetch_timer.timeout.connect(self._fetch_renamer_page)
        self._renamer_fetch_timer.start()

    def _fetch_renamer_page(self) -> None:
        """Fetch one page of functions and schedule the next or finish."""
        defn = self._renamer_load_defn
        offset = self._renamer_load_offset
        batch = self._renamer_load_batch

        try:
            raw = defn.handler(offset=offset, limit=batch)
        except Exception as e:
            log_error(f"list_functions failed at offset {offset}: {e}")
            raw = None

        page_count = 0
        if raw:
            for line in raw.splitlines():
                m = re.match(r"\s*0x([0-9a-fA-F]+)\s+(.+)", line)
                if m:
                    self._renamer_load_funcs.append(
                        {
                            "address": int(m.group(1), 16),
                            "name": m.group(2).strip(),
                            "is_import": False,
                            "instruction_count": 0,
                        }
                    )
                    page_count += 1

        if page_count >= batch:
            # More pages to fetch
            self._renamer_load_offset += batch
            return

        # All pages fetched — stop timer and load into widget
        self._renamer_fetch_timer.stop()
        self._renamer_fetch_timer.deleteLater()
        self._renamer_fetch_timer = None

        functions = self._renamer_load_funcs

        # Approximate function size from consecutive addresses
        for i in range(len(functions) - 1):
            functions[i]["instruction_count"] = functions[i + 1]["address"] - functions[i]["address"]

        if functions:
            self._bulk_renamer.load_functions(functions)
            log_info(f"Loaded {len(functions)} functions into bulk renamer")
        else:
            log_info("No functions found for bulk renamer")

        # Clean up temporary state
        self._renamer_load_funcs = []
        self._renamer_load_defn = None

    # --- Tools panel event handlers ---

    def _on_cancel_agent(self, agent_id: str) -> None:
        """Handle agent cancel request from AgentTreeWidget."""
        mgr = self._get_or_create_subagent_manager()
        if mgr is not None:
            mgr.cancel(agent_id)

    def _on_inject_summary(self, agent_id: str) -> None:
        """Inject a completed agent's summary into the active chat."""
        mgr = self._get_or_create_subagent_manager()
        if mgr is None:
            return
        info = mgr.get(agent_id)
        if info is None or not info.summary:
            return
        elapsed = (info.completed_at or info.created_at) - info.created_at
        text = (
            f"[Subagent \u201c{info.name}\u201d completed ({info.turn_count} turns, {elapsed:.0f}s)]\n\n{info.summary}"
        )
        self._start_agent(text)

    def _on_renamer_start(self, jobs, mode, batch_size, max_concurrent) -> None:
        """Handle bulk renamer start request."""
        from ..agent.bulk_renamer import RenameJob

        engine = self._get_or_create_renamer_engine(batch_size, max_concurrent)
        if engine is None:
            log_error("Cannot start renamer: LLM provider not available")
            return
        rename_jobs = [RenameJob(address=j["address"], current_name=j["current_name"]) for j in jobs]
        engine.enqueue(rename_jobs)
        self._renamer_engine = engine
        engine.start_renaming(deep=(mode == "deep"))

    def _on_renamer_pause(self) -> None:
        engine = getattr(self, "_renamer_engine", None)
        if engine is not None:
            if engine._paused.is_set():
                engine.pause()
            else:
                engine.resume()

    def _on_renamer_cancel(self) -> None:
        engine = getattr(self, "_renamer_engine", None)
        if engine is not None:
            engine.cancel_renaming()

    def _on_renamer_undo(self) -> None:
        engine = getattr(self, "_renamer_engine", None)
        if engine is None:
            return
        # undo_all calls tool_registry.execute which goes through
        # TPE + idasync — must run off the main thread to avoid deadlock.
        threading.Thread(target=engine.undo_all, daemon=True, name="rikugan-undo-renames").start()

    def _on_renamer_seek(self, address: int) -> None:
        """Navigate the host disassembly view to the given address."""
        from ..core.host import navigate_to

        navigate_to(address)

    def _poll_tools_events(self) -> None:
        """Poll all tools subsystems for events."""
        if self._is_shutdown:
            return

        # Poll subagent manager events
        mgr = getattr(self, "_subagent_manager", None)
        if mgr is not None:
            for _ in range(10):
                event = mgr.poll_event()
                if event is None:
                    break
                # Update agent tree
                if hasattr(self, "_agent_tree"):
                    from .agent_tree import AgentInfo

                    meta = event.metadata or {}
                    agent_id = meta.get("agent_id", "")
                    info = mgr.get(agent_id)
                    if info is not None:
                        elapsed = (info.completed_at or time.time()) - info.created_at
                        self._agent_tree.update_agent(
                            AgentInfo(
                                agent_id=info.id,
                                name=info.name,
                                agent_type=info.agent_type,
                                status=info.status.value.upper(),
                                turns=info.turn_count,
                                elapsed_seconds=elapsed,
                                summary=info.summary,
                                category=info.category,
                            )
                        )
                # Show in chat for spawned/completed/failed — but skip
                # bulk_rename agents to avoid polluting the conversation.
                if event.type in (
                    TurnEventType.SUBAGENT_SPAWNED,
                    TurnEventType.SUBAGENT_COMPLETED,
                    TurnEventType.SUBAGENT_FAILED,
                ):
                    is_bulk = info is not None and info.category == "bulk_rename"
                    if not is_bulk:
                        chat_view = self._active_chat_view()
                        if chat_view is not None:
                            chat_view.handle_event(event)

            # Refresh elapsed time for all RUNNING agents (~1 Hz, not every tick)
            now = time.time()
            last_sweep = getattr(self, "_last_agent_sweep", 0.0)
            if hasattr(self, "_agent_tree") and (now - last_sweep) >= 1.0:
                self._last_agent_sweep = now
                from .agent_tree import AgentInfo

                for info in mgr.list_all():
                    if info.status.value == "running":
                        elapsed = now - info.created_at
                        self._agent_tree.update_agent(
                            AgentInfo(
                                agent_id=info.id,
                                name=info.name,
                                agent_type=info.agent_type,
                                status=info.status.value.upper(),
                                turns=info.turn_count,
                                elapsed_seconds=elapsed,
                                summary=info.summary,
                                category=info.category,
                            )
                        )

        # Poll bulk renamer events
        engine = getattr(self, "_renamer_engine", None)
        if engine is not None:
            from ..agent.bulk_renamer import RenameEventType

            for _ in range(20):
                rename_event = engine.poll_event()
                if rename_event is None:
                    break
                if hasattr(self, "_bulk_renamer"):
                    _RENAME_STATUS_MAP = {
                        RenameEventType.JOB_STARTED: "analyzing",
                        RenameEventType.JOB_COMPLETED: "renamed",
                        RenameEventType.JOB_ERROR: "error",
                    }
                    if rename_event.type in _RENAME_STATUS_MAP:
                        status = _RENAME_STATUS_MAP[rename_event.type]
                        # Undo: JOB_COMPLETED with empty new_name means reverted
                        if rename_event.type == RenameEventType.JOB_COMPLETED and not rename_event.new_name:
                            status = "reverted"
                        self._bulk_renamer.update_job(
                            rename_event.address,
                            rename_event.new_name,
                            status,
                            rename_event.error,
                        )
                    if rename_event.type in (
                        RenameEventType.BATCH_PROGRESS,
                        RenameEventType.ALL_DONE,
                    ):
                        self._bulk_renamer.set_progress(
                            rename_event.completed,
                            rename_event.total,
                        )

    def _on_undo_requested(self, count: int) -> None:
        """Handle undo request from the mutation log panel."""
        if self._is_shutdown:
            return
        # Submit /undo command through the normal agent path
        self._start_agent(f"/undo {count}")

    def _set_running(self, running: bool) -> None:
        # Keep input enabled so users can queue follow-up messages while
        # running — UNLESS we're waiting for a button-only approval.
        if self._awaiting_button_approval:
            self._input_area.set_enabled(False)
            self._input_area.setPlaceholderText("Use the Approve/Reject buttons above to continue.")
        else:
            self._input_area.set_enabled(True)
            if running:
                self._input_area.setPlaceholderText(
                    "Rikugan is thinking... press Enter (or Queue) to queue a follow-up."
                )
            else:
                self._input_area.setPlaceholderText("Ask about this binary... (/ for skills, /modify to patch)")

        self._send_btn.setVisible(True)
        self._send_btn.setEnabled(not self._awaiting_button_approval)
        self._send_btn.setText("Queue" if running else "Send")
        self._cancel_btn.setVisible(running)

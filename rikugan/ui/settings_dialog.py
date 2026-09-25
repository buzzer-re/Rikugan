"""Settings dialog for provider, model, API key, and behavior configuration."""

from __future__ import annotations

import queue
import threading
from typing import Any

from ..core.config import RikuganConfig
from ..core.logging import log_debug, log_error
from ..core.types import ModelInfo
from ..providers.auth_cache import resolve_auth_cached
from ..providers.auth_compat import apply_keychain_consent, invalidate_auth_cache
from ..providers.ollama_provider import DEFAULT_OLLAMA_URL
from ..providers.registry import ProviderRegistry
from .qt_compat import (
    QApplication,
    QCheckBox,
    QComboBox,
    QDialog,
    QDialogButtonBox,
    QFileDialog,
    QFormLayout,
    QGroupBox,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QPushButton,
    QSpinBox,
    Qt,
    QTabWidget,
    QTimer,
    QVBoxLayout,
    QWidget,
)
from .styles import maybe_host_stylesheet

_DEFAULT_MINIMAX_URL = "https://api.minimax.io/anthropic"
_CUSTOM_PROVIDER_URL_PLACEHOLDER = "https://api.example.com/v1"

# Known default API base URLs per provider — used to auto-clear on switch
_PROVIDER_BASES = {
    "ollama": DEFAULT_OLLAMA_URL,
    "minimax": _DEFAULT_MINIMAX_URL,
}

# Placeholder/default keys that should be cleared on provider switch
_PROVIDER_DEFAULT_KEYS = {"ollama"}

# Backwards-compatible alias (tests and external code may reference the old name)
_resolve_auth_cached = resolve_auth_cached


class _ModelFetcher:
    """Fetches models in a background thread. Results collected via queue.

    This is a plain Python class — no QObject, no Qt signals.
    Results are polled from the main thread via a QTimer, eliminating
    all cross-thread Shiboken/PySide6 signal delivery crashes.
    """

    def __init__(self, registry: ProviderRegistry):
        self._registry = registry
        self._queue: queue.Queue = queue.Queue()
        self._alive = True

    def shutdown(self) -> None:
        self._alive = False
        # Drain the queue to unblock any pending puts
        while not self._queue.empty():
            try:
                self._queue.get_nowait()
            except queue.Empty:
                break

    def fetch(self, provider_name: str, api_key: str, api_base: str, extra: dict[str, Any] | None = None) -> None:
        # Create the provider and pre-import its SDK on the MAIN thread.
        # Python 3.14 crashes when heavy C-extension packages are first
        # imported from a background thread.
        try:
            provider = self._registry.create(
                provider_name,
                api_key=api_key,
                api_base=api_base,
                **(extra or {}),
            )
            provider.ensure_ready()
        except Exception as e:
            if self._alive:
                self._queue.put(("error", provider_name, str(e)))
            return

        def _run():
            try:
                models = provider.list_models()
                if self._alive:
                    self._queue.put(("models", provider_name, models))
            except Exception as e:
                if self._alive:
                    self._queue.put(("error", provider_name, str(e)))

        threading.Thread(target=_run, daemon=True).start()

    def poll(self) -> tuple | None:
        """Non-blocking poll. Returns ('models'|'error', provider_name, payload) or None."""
        try:
            return self._queue.get_nowait()
        except queue.Empty:
            return None


_BUILTIN_PROVIDERS = [
    "anthropic",
    "codex",
    "openai",
    "gemini",
    "ollama",
    "minimax",
    "openai_compat",
]


class _AddProviderDialog(QDialog):
    """Mini-dialog to create a new custom OpenAI-compatible connection."""

    def __init__(self, existing_names: list, parent: QWidget = None):
        super().__init__(parent)
        self.setWindowTitle("Add Custom Connection")
        self.setMinimumWidth(400)
        self._existing = {n.lower() for n in existing_names}

        layout = QVBoxLayout(self)
        form = QFormLayout()

        self._name_edit = QLineEdit()
        self._name_edit.setPlaceholderText("e.g. minimax, deepseek, local-vllm")
        form.addRow("Connection Name:", self._name_edit)

        self._base_edit = QLineEdit()
        self._base_edit.setPlaceholderText(_CUSTOM_PROVIDER_URL_PLACEHOLDER)
        form.addRow("API Base URL:", self._base_edit)

        layout.addLayout(form)

        self._error_label = QLabel()
        self._error_label.setStyleSheet(maybe_host_stylesheet("color: #f44747; font-size: 11px;"))
        self._error_label.hide()
        layout.addWidget(self._error_label)

        buttons = QDialogButtonBox(QDialogButtonBox.StandardButton.Ok | QDialogButtonBox.StandardButton.Cancel)
        buttons.accepted.connect(self._validate)
        buttons.rejected.connect(self.reject)
        layout.addWidget(buttons)

    def _validate(self) -> None:
        name = self._name_edit.text().strip().lower().replace(" ", "-")
        if not name:
            self._error_label.setText("Name is required")
            self._error_label.show()
            return
        if name in self._existing:
            self._error_label.setText(f"'{name}' already exists")
            self._error_label.show()
            return
        base = self._base_edit.text().strip()
        if not base:
            self._error_label.setText("API Base URL is required")
            self._error_label.show()
            return
        self._name_edit.setText(name)
        self.accept()

    def provider_name(self) -> str:
        return self._name_edit.text().strip()

    def api_base(self) -> str:
        return self._base_edit.text().strip()

    def provider_settings(self) -> dict[str, Any]:
        return {}


class _CodexSetupDialog(QDialog):
    """Device-code login dialog for Codex / ChatGPT OAuth."""

    def __init__(self, parent: QWidget = None):
        super().__init__(parent)
        self.setWindowTitle("Setup Codex")
        self.setMinimumWidth(420)
        self._queue: queue.Queue = queue.Queue()
        self._device_code: Any = None
        self._done = False
        self._polling = False

        layout = QVBoxLayout(self)
        self._status = QLabel("Requesting device code...")
        self._status.setWordWrap(True)
        layout.addWidget(self._status)

        self._link = QLabel()
        self._link.setTextInteractionFlags(Qt.TextInteractionFlag.TextBrowserInteraction)
        self._link.setOpenExternalLinks(True)
        self._link.hide()
        layout.addWidget(self._link)

        self._code = QLineEdit()
        self._code.setReadOnly(True)
        self._code.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self._code.hide()
        layout.addWidget(self._code)

        self._poll_btn = QPushButton("Check login")
        self._poll_btn.setEnabled(False)
        self._poll_btn.clicked.connect(self._start_poll)
        layout.addWidget(self._poll_btn)

        buttons = QDialogButtonBox(QDialogButtonBox.StandardButton.Cancel)
        buttons.rejected.connect(self.reject)
        layout.addWidget(buttons)

        self._timer = QTimer(self)
        self._timer.timeout.connect(self._poll_queue)
        self._timer.start(150)
        threading.Thread(target=self._request_code, daemon=True).start()

    def _request_code(self) -> None:
        try:
            from ..providers.codex_provider import request_codex_device_code

            self._queue.put(("code", request_codex_device_code()))
        except Exception as exc:
            self._queue.put(("error", str(exc)))

    def _start_poll(self) -> None:
        if self._device_code is None or self._polling:
            return
        self._polling = True
        self._poll_btn.setEnabled(False)
        self._status.setText("Waiting for browser authorization...")
        threading.Thread(target=self._complete_login, daemon=True).start()

    def _complete_login(self) -> None:
        if self._device_code is None:
            self._queue.put(("error", "Codex device code is not ready"))
            return
        try:
            from ..providers.codex_provider import complete_codex_device_code_login

            complete_codex_device_code_login(self._device_code)
            self._queue.put(("done", "Codex OAuth saved"))
        except Exception as exc:
            self._queue.put(("error", str(exc)))

    def _poll_queue(self) -> None:
        try:
            kind, payload = self._queue.get_nowait()
        except queue.Empty:
            return
        if kind == "code":
            self._device_code = payload
            self._status.setText("Open the link and enter this code.")
            self._link.setText(f'<a href="{payload.verification_url}">{payload.verification_url}</a>')
            self._link.show()
            self._code.setText(payload.user_code)
            self._code.show()
            self._poll_btn.setEnabled(True)
            self._start_poll()
        elif kind == "done":
            self._done = True
            self._status.setText(payload)
            self.accept()
        elif kind == "error":
            self._polling = False
            self._status.setText(payload)
            self._poll_btn.setEnabled(self._device_code is not None)

    def done(self, result: int) -> None:
        self._timer.stop()
        super().done(result)


class SettingsDialog(QDialog):
    """Configuration dialog for Rikugan."""

    def __init__(
        self,
        config: RikuganConfig,
        registry: ProviderRegistry | None = None,
        tool_registry: Any | None = None,
        parent: QWidget = None,
    ):
        # Use None parent to avoid lifecycle coupling with IDA PluginForm widgets
        super().__init__(None)
        self.setWindowModality(Qt.WindowModality.ApplicationModal)
        self._config = config
        self._tool_registry = tool_registry
        self._registry = registry or ProviderRegistry()
        self._registry.register_custom_providers(list(self._config.custom_providers.keys()))
        self._fetcher = _ModelFetcher(self._registry)
        self._fetched_models: list[ModelInfo] = []
        self._resolved_token: str = ""
        self._model_restore_hint: str = self._config.provider.model.strip()
        self._shown = False
        self._closed = False
        self.encryption_password: str = ""
        self.setWindowTitle("Rikugan Settings")
        screen = QApplication.primaryScreen()
        if screen:
            avail = screen.availableGeometry()
            self.resize(min(int(avail.width() * 0.45), 900), min(int(avail.height() * 0.7), 800))
        else:
            self.resize(700, 600)
        self.setMinimumWidth(400)
        self._build_ui()
        self._remove_provider_btn.setEnabled(self._config.is_custom_provider(self._config.provider.name))

        # Poll timer for fetcher results — NO cross-thread signals
        self._poll_timer = QTimer(self)
        self._poll_timer.timeout.connect(self._poll_fetcher)
        self._poll_timer.start(150)

        # Deferred init timer — parented to self, safe if dialog closes instantly
        self._init_timer = QTimer(self)
        self._init_timer.setSingleShot(True)
        self._init_timer.setInterval(0)
        self._init_timer.timeout.connect(self._deferred_init)

    def _build_ui(self) -> None:
        layout = QVBoxLayout(self)
        self._tabs = QTabWidget()

        # Tab 0: Provider (existing 3 group boxes)
        provider_tab = QWidget()
        playout = QVBoxLayout(provider_tab)
        self._provider_group = self._build_provider_group()
        playout.addWidget(self._provider_group)
        self._behavior_group = self._build_behavior_group()
        playout.addWidget(self._behavior_group)
        playout.addStretch()
        self._tabs.addTab(provider_tab, "Provider")

        # Tab 1-3: Skills, MCP, Profiles — all use a shared SettingsService
        from .settings_service import SettingsService
        from .tabs.mcp_tab import MCPTab
        from .tabs.profiles_tab import ProfilesTab
        from .tabs.skills_tab import SkillsTab

        self._service = SettingsService(self._config, tool_registry=self._tool_registry)
        self._skills_tab = SkillsTab(self._config, service=self._service)
        self._tabs.addTab(self._skills_tab, "Skills")
        self._mcp_tab = MCPTab(self._config, service=self._service)
        self._tabs.addTab(self._mcp_tab, "MCP")
        self._profiles_tab = ProfilesTab(self._config, service=self._service)
        self._tabs.addTab(self._profiles_tab, "Profiles")

        layout.addWidget(self._tabs)

        self._button_box = QDialogButtonBox(QDialogButtonBox.StandardButton.Ok | QDialogButtonBox.StandardButton.Cancel)
        self._button_box.accepted.connect(self._on_accept)
        self._button_box.rejected.connect(self.reject)
        layout.addWidget(self._button_box)

        # Connect provider/key change signals AFTER everything is built
        self._provider_combo.currentTextChanged.connect(self._on_provider_changed)
        self._api_key_edit.editingFinished.connect(self._on_key_edited)

    def _build_provider_group(self) -> QGroupBox:
        """Build the LLM Provider settings group box."""
        provider_group = QGroupBox("LLM Provider")
        provider_form = QFormLayout(provider_group)

        provider_form.addRow("Provider:", self._build_provider_row())

        # API key — only show explicit user keys, NOT auto-resolved OAuth tokens
        key_layout = QHBoxLayout()
        self._api_key_edit = QLineEdit(self._config.provider.api_key)
        self._api_key_edit.setEchoMode(QLineEdit.EchoMode.Password)
        self._api_key_edit.setPlaceholderText("sk-... or leave empty for auto-detect")
        key_layout.addWidget(self._api_key_edit, 1)
        self._auth_status = QLabel()
        key_layout.addWidget(self._auth_status)
        provider_form.addRow("API Key:", key_layout)

        # OAuth checkbox — controls keychain autoload
        self._oauth_cb = QCheckBox("Use OAuth from Claude Code (macOS Keychain)")
        self._oauth_cb.setChecked(self._config.oauth_consent_accepted)
        self._oauth_cb.setVisible(self._config.provider.name == "anthropic")
        self._oauth_cb.setToolTip(
            "Auto-load your Claude Code OAuth token from the macOS Keychain.\n"
            "Requires accepting Anthropic's credential use policy."
        )
        self._oauth_cb.toggled.connect(self._on_oauth_toggled)
        provider_form.addRow("", self._oauth_cb)

        self._codex_setup_btn = QPushButton("Setup Codex")
        self._codex_setup_btn.setVisible(self._config.provider.name == "codex")
        self._codex_setup_btn.clicked.connect(self._on_setup_codex)
        provider_form.addRow("", self._codex_setup_btn)

        self._api_base_edit = QLineEdit(self._config.provider.api_base)
        self._api_base_edit.setPlaceholderText("Custom endpoint URL (optional)")
        provider_form.addRow("API Base:", self._api_base_edit)

        provider_form.addRow("Model:", self._build_model_row())

        return provider_group

    def _build_provider_row(self) -> QHBoxLayout:
        """Build the provider combo + add/remove buttons row."""
        btn_style = maybe_host_stylesheet(
            "QPushButton { background: #2d2d2d; color: #d4d4d4; border: 1px solid #3c3c3c; "
            "border-radius: 4px; font-size: 13px; font-weight: bold; }"
            "QPushButton:hover { background: #3c3c3c; }"
        )
        row = QHBoxLayout()
        self._provider_combo = QComboBox()
        self._populate_provider_combo()
        idx = self._provider_combo.findText(self._config.provider.name)
        if idx >= 0:
            self._provider_combo.setCurrentIndex(idx)
        row.addWidget(self._provider_combo, 1)

        self._add_provider_btn = QPushButton("+")
        self._add_provider_btn.setFixedSize(28, 28)
        self._add_provider_btn.setToolTip("Add custom OpenAI-compatible connection")
        self._add_provider_btn.setStyleSheet(btn_style)
        self._add_provider_btn.clicked.connect(self._on_add_custom_provider)
        row.addWidget(self._add_provider_btn)

        self._remove_provider_btn = QPushButton("\u2212")  # minus sign
        self._remove_provider_btn.setFixedSize(28, 28)
        self._remove_provider_btn.setToolTip("Remove custom connection")
        self._remove_provider_btn.setStyleSheet(btn_style)
        self._remove_provider_btn.clicked.connect(self._on_remove_custom_provider)
        row.addWidget(self._remove_provider_btn)

        return row  # connected AFTER group is built (in _build_ui)

    def _build_model_row(self) -> QHBoxLayout:
        """Build the model combo + refresh button + status row."""
        model_layout = QHBoxLayout()
        self._model_combo = QComboBox()
        self._model_combo.setEditable(True)
        self._model_combo.setMinimumWidth(300)
        self._model_combo.setCurrentText(self._config.provider.model)
        model_layout.addWidget(self._model_combo, 1)

        self._fetch_btn = QPushButton("Refresh")
        self._fetch_btn.setFixedWidth(70)
        self._fetch_btn.setStyleSheet(
            maybe_host_stylesheet(
                "QPushButton { background: #2d2d2d; color: #d4d4d4; border: 1px solid #3c3c3c; "
                "border-radius: 4px; padding: 4px; font-size: 11px; }"
                "QPushButton:hover { background: #3c3c3c; }"
            )
        )
        self._fetch_btn.clicked.connect(self._fetch_models)
        model_layout.addWidget(self._fetch_btn)

        self._model_status = QLabel()
        self._model_status.setStyleSheet(maybe_host_stylesheet("color: #808080; font-size: 10px;"))
        self._model_status.setWordWrap(True)
        model_layout.addWidget(self._model_status)
        return model_layout

    def _build_behavior_group(self) -> QGroupBox:
        """Build the Behavior settings group box."""
        behavior_group = QGroupBox("Behavior")
        behavior_form = QFormLayout(behavior_group)

        self._auto_context_cb = QCheckBox("Auto-inject binary context into system prompt")
        self._auto_context_cb.setChecked(self._config.auto_context)
        behavior_form.addRow(self._auto_context_cb)

        self._auto_save_cb = QCheckBox("Auto-save sessions")
        self._auto_save_cb.setChecked(self._config.checkpoint_auto_save)
        behavior_form.addRow(self._auto_save_cb)

        self._restore_sessions_cb = QCheckBox("Restore previous chats on startup")
        self._restore_sessions_cb.setChecked(self._config.restore_sessions_on_start)
        self._restore_sessions_cb.setToolTip("Disable this when large analysis sessions make the panel slow to open.")
        behavior_form.addRow(self._restore_sessions_cb)

        self._dont_auto_load_cb = QCheckBox("Don't auto-load chats (show a placeholder, pick from the sidebar)")
        self._dont_auto_load_cb.setChecked(self._config.dont_auto_load_chats)
        self._dont_auto_load_cb.setToolTip(
            "List saved chats in the sidebar but don't open any on startup.\n"
            "Rikugan opens to a placeholder; selecting a chat loads it on demand."
        )
        behavior_form.addRow(self._dont_auto_load_cb)

        storage_row = QHBoxLayout()
        self._session_storage_edit = QLineEdit(self._config.session_storage_dir)
        self._session_storage_edit.setPlaceholderText("Default: Rikugan config/checkpoints")
        storage_row.addWidget(self._session_storage_edit, 1)
        self._session_storage_btn = QPushButton("Browse")
        self._session_storage_btn.setFixedWidth(70)
        self._session_storage_btn.clicked.connect(self._choose_session_storage_dir)
        storage_row.addWidget(self._session_storage_btn)
        behavior_form.addRow("Session storage:", storage_row)

        from ..binja.native_mcp import DEFAULT_URL as _BN_MCP_DEFAULT_URL

        self._binja_mcp_url_edit = QLineEdit(self._config.binja_mcp_url)
        self._binja_mcp_url_edit.setPlaceholderText(f"Default: {_BN_MCP_DEFAULT_URL}")
        self._binja_mcp_url_edit.setToolTip(
            "Where Binary Ninja's own MCP server listens. Rikugan offers to use it\n"
            "once per binary when a server actually answers at this address;\n"
            "the toggle in the chat header turns it on and off afterwards."
        )
        behavior_form.addRow("Binary Ninja MCP URL:", self._binja_mcp_url_edit)

        self._explore_turns_spin = QSpinBox()
        self._explore_turns_spin.setRange(5, 200)
        self._explore_turns_spin.setValue(self._config.exploration_turn_limit)
        self._explore_turns_spin.setToolTip(
            "Maximum turns the agent spends in the exploration phase before "
            "forcing a transition (or reporting an error if findings are insufficient)."
        )
        behavior_form.addRow("Exploration turn limit:", self._explore_turns_spin)

        # --- Rate-limit handling ---
        self._max_retries_spin = QSpinBox()
        self._max_retries_spin.setRange(1, 10)
        self._max_retries_spin.setValue(self._config.max_retries)
        self._max_retries_spin.setToolTip(
            "Number of retry attempts when the API returns a rate-limit or transient error."
        )
        behavior_form.addRow("API retry attempts:", self._max_retries_spin)

        self._silent_retry_cb = QCheckBox("Show loading indicator instead of error messages during retries")
        self._silent_retry_cb.setChecked(self._config.silent_retry_mode)
        self._silent_retry_cb.setToolTip(
            "When enabled, rate-limit retries show a subtle text indicator instead of red error messages."
        )
        behavior_form.addRow(self._silent_retry_cb)

        # --- Context preservation ---
        self._preserve_context_cb = QCheckBox("Preserve full context (disable tool result truncation)")
        self._preserve_context_cb.setChecked(self._config.preserve_context)
        self._preserve_context_cb.setToolTip(
            "Disables tool result truncation and message trimming. "
            "Enable for deep RE sessions where losing decompilation context is worse than higher token cost."
        )
        behavior_form.addRow(self._preserve_context_cb)

        # --- API key encryption ---
        from ..core.crypto import is_available as crypto_available

        self._encrypt_keys_cb = QCheckBox("Encrypt API keys with password")
        self._encrypt_keys_cb.setChecked(self._config.encrypt_api_keys)
        self._encrypt_keys_cb.setEnabled(crypto_available())
        self._encrypt_keys_cb.setToolTip(
            "Encrypt all stored API keys with a password.\nYou must enter this password each time Rikugan starts."
            if crypto_available()
            else "Requires the 'cryptography' package (pip install cryptography)."
        )
        behavior_form.addRow(self._encrypt_keys_cb)

        return behavior_group

    # --- Show event: defer all non-widget work to here ---

    def showEvent(self, event) -> None:
        super().showEvent(event)
        if not self._shown:
            self._shown = True
            # Defer auth resolution and model fetch to AFTER the dialog is painted.
            # This avoids subprocess.run() and background threads during construction.
            self._init_timer.start()

    def _deferred_init(self) -> None:
        """Runs after the dialog is fully painted. Safe for subprocesses/threads."""
        if self._closed:
            return
        try:
            self._update_auth_status()
            self._model_restore_hint = self._config.provider.model.strip()
            self._fetch_models()
        except Exception as e:
            log_error(f"SettingsDialog deferred init error: {e}")

    # --- Cleanup ---

    def done(self, result: int) -> None:
        self._closed = True
        try:
            self._init_timer.stop()
            self._poll_timer.stop()
        except RuntimeError as e:
            log_debug(f"SettingsDialog.done timer cleanup: {e}")
        self._fetcher.shutdown()
        super().done(result)

    # --- Fetcher polling (main thread only, no cross-thread signals) ---

    def _poll_fetcher(self) -> None:
        """Poll the fetcher queue from the main thread. Safe for Shiboken."""
        if self._closed:
            return
        result = self._fetcher.poll()
        if result is None:
            return
        try:
            kind, provider_name, data = result
            # Ignore stale results from previous provider selections.
            if provider_name != self._provider_combo.currentText():
                return
            if kind == "models":
                self._on_models_ready(data)
            elif kind == "error":
                self._on_fetch_error(data)
        except (ValueError, TypeError) as e:
            log_debug(f"Malformed fetcher result: {e}")

    # --- Provider switching ---

    def _on_provider_changed(self, provider: str) -> None:
        # Persist edits from the previous provider before switching.
        # Skip sync if switch_provider was already called externally (e.g. _on_add_custom_provider)
        # to avoid corrupting the new provider's config with stale UI values.
        if self._config.provider.name != provider:
            self._sync_config_from_ui()

        # Use config.switch_provider() to snapshot current & restore saved
        self._config.switch_provider(provider)

        # Enable remove button only for custom providers
        is_custom = self._config.is_custom_provider(provider)
        self._remove_provider_btn.setEnabled(is_custom)

        # Update UI fields from the (possibly restored) config
        self._api_key_edit.setText(self._config.provider.api_key)
        self._api_base_edit.setText(self._config.provider.api_base)
        self._model_combo.setCurrentText(self._config.provider.model)
        self._model_restore_hint = self._config.provider.model.strip()

        # Auto-fill API base for providers that need it
        if provider == "ollama" and not self._api_base_edit.text().strip():
            self._api_base_edit.setText(_PROVIDER_BASES["ollama"])

        # OAuth checkbox only visible for Anthropic
        self._oauth_cb.setVisible(provider == "anthropic")
        self._codex_setup_btn.setVisible(provider == "codex")

        # Update placeholder
        if provider == "anthropic":
            self._api_key_edit.setPlaceholderText("sk-... or leave empty for OAuth auto-detect")
        elif provider == "codex":
            self._api_key_edit.setPlaceholderText("Managed by Codex OAuth")
        elif provider == "ollama":
            self._api_key_edit.setPlaceholderText("Not required for local Ollama")
        elif provider in ("openai_compat",) or is_custom:
            self._api_key_edit.setPlaceholderText("API key for the endpoint")
        else:
            self._api_key_edit.setPlaceholderText("API key")

        self._update_auth_status()
        self._fetch_models()

    def _on_key_edited(self) -> None:
        self._model_restore_hint = self._get_selected_model_id()
        self._update_auth_status()
        self._fetch_models()

    def _on_setup_codex(self) -> None:
        dlg = _CodexSetupDialog(parent=self)
        if dlg.exec() == QDialog.DialogCode.Accepted:
            self._update_auth_status()
            self._fetch_models()

    def _choose_session_storage_dir(self) -> None:
        path = QFileDialog.getExistingDirectory(
            self,
            "Choose Session Storage Directory",
            self._session_storage_edit.text().strip() or self._config.checkpoints_dir,
        )
        if path:
            self._session_storage_edit.setText(path)

    def _on_oauth_toggled(self, checked: bool) -> None:
        """Handle the OAuth checkbox toggle."""
        if checked and not self._config.oauth_consent_accepted:
            from .oauth_consent import show_oauth_consent

            choice = show_oauth_consent(parent=self)
            if choice != "accept":
                # User declined — uncheck without recursion
                self._oauth_cb.blockSignals(True)
                self._oauth_cb.setChecked(False)
                self._oauth_cb.blockSignals(False)
                return
        # Update consent and refresh auth status
        apply_keychain_consent(checked)
        invalidate_auth_cache()
        self._update_auth_status()

    # --- Auth status ---

    _OK_STYLE = maybe_host_stylesheet("color: #4ec9b0; font-size: 11px; font-weight: bold;")
    _ERR_STYLE = maybe_host_stylesheet("color: #f44747; font-size: 11px;")
    _HINT_STYLE = maybe_host_stylesheet("color: #808080; font-size: 10px;")

    def _update_auth_status(self) -> None:
        provider_name = self._provider_combo.currentText()
        explicit_key = self._api_key_edit.text().strip()
        base = self._api_base_edit.text().strip()

        try:
            if provider_name == "codex":
                from ..providers.codex_provider import codex_auth_status

                label, status_type = codex_auth_status()
                self._resolved_token = ""
            else:
                provider = self._registry.create(
                    provider_name,
                    api_key=explicit_key,
                    api_base=base,
                    **self._provider_extra_for(provider_name),
                )
                label, status_type = provider.auth_status()
                self._resolved_token = provider.api_key
        except Exception as e:
            log_debug(f"Auth status check failed for {provider_name}: {e}")
            label, status_type = "", "none"
            self._resolved_token = ""

        if status_type == "ok":
            self._auth_status.setText(label)
            self._auth_status.setStyleSheet(self._OK_STYLE)
        elif status_type == "error":
            if provider_name == "anthropic":
                self._auth_status.setText("run claude setup-token to acquire your oauth")
                self._auth_status.setStyleSheet(self._HINT_STYLE)
            else:
                self._auth_status.setText(label)
                self._auth_status.setStyleSheet(self._ERR_STYLE)
        else:
            self._auth_status.setText("")
            self._auth_status.setStyleSheet("")
        self._codex_setup_btn.setVisible(provider_name == "codex" and status_type != "ok")

    # --- Model fetching ---

    def _fetch_models(self) -> None:
        provider = self._provider_combo.currentText()
        key = self._api_key_edit.text().strip()
        base = self._api_base_edit.text().strip()

        # For providers with auto-detect auth, use resolved token if no explicit key
        if not key and self._resolved_token:
            key = self._resolved_token

        self._model_status.setText("Fetching...")
        self._fetch_btn.setEnabled(False)
        self._fetcher.fetch(provider, key, base, self._provider_extra_for(provider))

    def _on_models_ready(self, models: list) -> None:
        self._fetch_btn.setEnabled(True)
        self._fetched_models = models

        preferred_id = (self._model_restore_hint or "").strip()
        current_id = preferred_id or self._get_selected_model_id()
        self._model_combo.clear()
        for m in models:
            label = f"{m.name}  ({m.id})" if m.name != m.id else m.id
            self._model_combo.addItem(label, m.id)

        # Restore previous selection by model ID
        matched = False
        for i in range(self._model_combo.count()):
            if self._model_combo.itemData(i) == current_id:
                self._model_combo.setCurrentIndex(i)
                matched = True
                break
        if not matched and models:
            # If the previous model ID doesn't match any fetched model
            # (e.g. stale error text, wrong provider), select the first one
            # instead of keeping garbage text in the editable combo.
            self._model_combo.setCurrentIndex(0)
        self._model_restore_hint = ""

        if models:
            self._model_status.setText(f"{len(models)} models")
            self._model_status.setStyleSheet(maybe_host_stylesheet("color: #4ec9b0; font-size: 10px;"))
        else:
            self._model_status.setText("Type model name manually")
            self._model_status.setStyleSheet(maybe_host_stylesheet("color: #808080; font-size: 10px;"))

    def _on_fetch_error(self, error: str) -> None:
        self._fetch_btn.setEnabled(True)
        self._model_status.setText(error)
        self._model_status.setStyleSheet(maybe_host_stylesheet("color: #f44747; font-size: 10px;"))
        self._model_restore_hint = ""

    def _provider_extra_for(self, provider: str) -> dict[str, Any]:
        if self._config.is_custom_provider(provider):
            return dict(self._config.custom_providers.get(provider, {}))
        if provider == self._config.provider.name:
            return dict(self._config.provider.extra or {})
        return {}

    def _get_selected_model_id(self) -> str:
        idx = self._model_combo.currentIndex()
        data = self._model_combo.itemData(idx) if idx >= 0 else None
        if data:
            return data
        return self._model_combo.currentText().strip()

    # --- Custom provider management ---

    def _populate_provider_combo(self) -> None:
        """Fill the provider combo with builtins + custom connections."""
        self._provider_combo.clear()
        self._provider_combo.addItems(_BUILTIN_PROVIDERS)
        custom = sorted(self._config.custom_providers.keys())
        if custom:
            self._provider_combo.insertSeparator(len(_BUILTIN_PROVIDERS))
            self._provider_combo.addItems(custom)

    def _on_add_custom_provider(self) -> None:
        all_names = _BUILTIN_PROVIDERS + list(self._config.custom_providers.keys())
        dlg = _AddProviderDialog(all_names, parent=self)
        if dlg.exec() != QDialog.DialogCode.Accepted:
            return
        name = dlg.provider_name()
        api_base = dlg.api_base()
        # Snapshot current provider settings before switching
        self._sync_config_from_ui()
        # Register in config and registry
        self._config.add_custom_provider(name, dlg.provider_settings())
        self._registry.register_custom_providers([name])
        # Initialize settings for the new provider
        self._config.switch_provider(name)
        self._config.provider.api_base = api_base
        self._config.provider.extra = dlg.provider_settings()
        # Rebuild combo and select the new provider
        self._provider_combo.currentTextChanged.disconnect(self._on_provider_changed)
        self._populate_provider_combo()
        idx = self._provider_combo.findText(name)
        if idx >= 0:
            self._provider_combo.setCurrentIndex(idx)
        self._provider_combo.currentTextChanged.connect(self._on_provider_changed)
        self._on_provider_changed(name)

    def _on_remove_custom_provider(self) -> None:
        name = self._provider_combo.currentText()
        if not self._config.is_custom_provider(name):
            return
        self._config.remove_custom_provider(name)
        self._provider_combo.currentTextChanged.disconnect(self._on_provider_changed)
        self._populate_provider_combo()
        self._provider_combo.setCurrentIndex(0)
        self._provider_combo.currentTextChanged.connect(self._on_provider_changed)
        self._on_provider_changed(self._provider_combo.currentText())

    def _sync_config_from_ui(self) -> None:
        """Copy current UI values into config (without accepting the dialog)."""
        provider_name = self._config.provider.name
        self._config.provider.model = self._get_selected_model_id()
        self._config.provider.api_key = self._api_key_edit.text().strip()
        self._config.provider.api_base = self._api_base_edit.text().strip()
        if self._config.is_custom_provider(provider_name):
            self._config.provider.extra = dict(self._config.custom_providers.get(provider_name, {}))
            self._config.custom_providers[provider_name] = dict(self._config.provider.extra)

    # --- Accept ---

    def _prompt_password(self, title: str, confirm: bool = False) -> str:
        """Show a modal password dialog. Returns empty string on cancel."""
        from .qt_compat import QMessageBox

        dlg = QDialog(self)
        dlg.setWindowTitle(title)
        dlg.setMinimumWidth(320)
        layout = QVBoxLayout(dlg)

        pw_edit = QLineEdit()
        pw_edit.setEchoMode(QLineEdit.EchoMode.Password)
        pw_edit.setPlaceholderText("Password")
        layout.addWidget(pw_edit)

        pw_confirm: QLineEdit | None = None
        if confirm:
            pw_confirm = QLineEdit()
            pw_confirm.setEchoMode(QLineEdit.EchoMode.Password)
            pw_confirm.setPlaceholderText("Confirm password")
            layout.addWidget(pw_confirm)

        from .qt_compat import QDialogButtonBox, qt_flags, qt_run

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
            return ""

        password = pw_edit.text()
        if not password:
            QMessageBox.warning(self, title, "Password cannot be empty.")
            return ""
        if confirm and pw_confirm and pw_confirm.text() != password:
            QMessageBox.warning(self, title, "Passwords do not match.")
            return ""
        return password

    def _on_accept(self) -> None:
        api_key = self._api_key_edit.text().strip()

        # If the user pasted an OAuth token with the checkbox unchecked,
        # show the consent dialog.  Use parent=None to avoid nesting a
        # modal inside this already-modal settings dialog.
        if api_key.startswith("sk-ant-oat") and not self._oauth_cb.isChecked():
            from .oauth_consent import show_oauth_consent

            choice = show_oauth_consent(parent=None)
            if choice == "accept":
                self._oauth_cb.blockSignals(True)
                self._oauth_cb.setChecked(True)
                self._oauth_cb.blockSignals(False)
            else:
                self._api_key_edit.clear()
                return

        self._config.provider.name = self._provider_combo.currentText()
        self._config.provider.model = self._get_selected_model_id()
        # ONLY save what the user explicitly typed — never save auto-resolved OAuth tokens
        self._config.provider.api_key = self._api_key_edit.text().strip()
        self._config.provider.api_base = self._api_base_edit.text().strip()
        self._config.auto_context = self._auto_context_cb.isChecked()
        self._config.checkpoint_auto_save = self._auto_save_cb.isChecked()
        self._config.restore_sessions_on_start = self._restore_sessions_cb.isChecked()
        self._config.dont_auto_load_chats = self._dont_auto_load_cb.isChecked()
        self._config.session_storage_dir = self._session_storage_edit.text().strip()
        self._config.binja_mcp_url = self._binja_mcp_url_edit.text().strip()
        self._config.exploration_turn_limit = self._explore_turns_spin.value()
        self._config.max_retries = self._max_retries_spin.value()
        self._config.silent_retry_mode = self._silent_retry_cb.isChecked()
        self._config.preserve_context = self._preserve_context_cb.isChecked()
        self._config.oauth_consent_accepted = self._oauth_cb.isChecked()
        if self._config.is_custom_provider(self._config.provider.name):
            self._config.provider.extra = dict(self._config.custom_providers.get(self._config.provider.name, {}))
            self._config.custom_providers[self._config.provider.name] = dict(self._config.provider.extra)

        # --- API key encryption handling ---
        wants_encrypt = self._encrypt_keys_cb.isChecked()
        password = ""
        if wants_encrypt:
            if self._config.encrypt_api_keys:
                # Already encrypted — need current password to re-encrypt
                password = self._prompt_password("Enter encryption password", confirm=False)
            else:
                # Newly enabling — prompt for new password with confirmation
                password = self._prompt_password("Set encryption password", confirm=True)
            if not password:
                return  # user cancelled
        elif self._config.encrypt_api_keys:
            # Disabling encryption — need current password to verify ownership
            password = self._prompt_password("Enter current password to disable encryption", confirm=False)
            if not password:
                return
            # Verify the password is correct before disabling
            if self._config.has_encrypted_keys():
                if not self._config.decrypt_stored_keys(password):
                    from .qt_compat import QMessageBox

                    QMessageBox.warning(self, "Wrong Password", "Incorrect password.")
                    return
            password = ""  # save unencrypted

        self._config.encrypt_api_keys = wants_encrypt
        self.encryption_password = password  # consumed by caller's save()

        # Apply new tab settings
        self._skills_tab.apply_to_config(self._config)
        self._mcp_tab.apply_to_config(self._config)
        self._profiles_tab.apply_to_config(self._config)

        self.accept()

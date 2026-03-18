"""OAuth consent dialog shown when enabling OAuth in settings."""

from __future__ import annotations

from .qt_compat import QDialog, QDialogButtonBox, QLabel, QPushButton, QVBoxLayout, QWidget

_POLICY_URL = "https://code.claude.com/docs/en/legal-and-compliance#authentication-and-credential-use"

_DIALOG_STYLE = (
    "QDialog { background: #1e1e1e; }"
    "QLabel { color: #d4d4d4; font-size: 12px; }"
    "QPushButton { background: #2d2d2d; color: #d4d4d4; border: 1px solid #3c3c3c; "
    "border-radius: 4px; padding: 8px 16px; font-size: 11px; min-width: 120px; }"
    "QPushButton:hover { background: #3c3c3c; }"
)


def show_oauth_consent(parent: QWidget | None = None) -> str:
    """Show the OAuth consent dialog.

    Returns:
        ``"accept"`` — user accepts the risk, autoload from Keychain.
        ``"api_key"`` — user prefers to use an API key instead.
        ``"cancel"``  — dialog dismissed without choosing.
    """
    dlg = QDialog(parent)
    dlg.setWindowTitle("OAuth Token Detected")
    dlg.setStyleSheet(_DIALOG_STYLE)
    dlg.setMinimumWidth(480)

    layout = QVBoxLayout(dlg)

    warning = QLabel(
        "<b>Use your Claude Code OAuth token with Rikugan?</b>"
        "<br><br>"
        "Using this token with third-party tools carries risk. "
        "Please read Anthropic's policy before proceeding:"
        "<br><br>"
        f'<a href="{_POLICY_URL}" style="color: #4ec9b0;">{_POLICY_URL}</a>'
        "<br><br>"
        "By clicking <b>Ok</b> below, you acknowledge the risk and allow "
        "Rikugan to use this token. This choice is saved and won't be asked again."
    )
    warning.setWordWrap(True)
    warning.setOpenExternalLinks(True)
    layout.addWidget(warning)

    btn_box = QDialogButtonBox()
    ok_btn = QPushButton("Ok, don't show again")
    api_btn = QPushButton("Use API Key instead")
    btn_box.addButton(ok_btn, QDialogButtonBox.ButtonRole.AcceptRole)
    btn_box.addButton(api_btn, QDialogButtonBox.ButtonRole.RejectRole)
    layout.addWidget(btn_box)

    result = {"choice": "cancel"}

    def _on_accept() -> None:
        result["choice"] = "accept"
        dlg.accept()

    def _on_api_key() -> None:
        result["choice"] = "api_key"
        dlg.reject()

    ok_btn.clicked.connect(_on_accept)
    api_btn.clicked.connect(_on_api_key)

    dlg.exec()

    # Force immediate C++ destruction to prevent the QDialogButtonBox
    # from surviving until QApplication::~QApplication() (same crash
    # pattern as the QFrame orphan fix — PySide6 disconnectNotify on
    # a dead Python interpreter).
    dlg.setParent(None)
    dlg.deleteLater()

    return result["choice"]

"""Session history: persist, list, and restore past sessions.

This is the single persistence layer for all session state.
"""

from __future__ import annotations

import json
import os
from typing import Any

from ..constants import SESSION_SCHEMA_VERSION
from ..core.config import RikuganConfig
from ..core.logging import log_debug
from ..core.types import Message
from .session import SessionState

_SUMMARY_SUFFIX = ".summary.json"


def _normalize_db_path(path: str) -> str:
    """Return a stable canonical DB path for session filtering."""
    if not path:
        return ""
    try:
        return os.path.normcase(os.path.realpath(os.path.abspath(path)))
    except OSError:
        return path


def _build_summary_data(data: dict[str, Any], fallback_id: str) -> dict[str, Any]:
    return {
        "id": data.get("id", fallback_id),
        "created_at": data.get("created_at", 0),
        "last_active_at": data.get("last_active_at", data.get("created_at", 0)),
        "provider": data.get("provider_name", ""),
        "model": data.get("model_name", ""),
        "idb_path": _normalize_db_path(data.get("idb_path", "")),
        "db_instance_id": data.get("db_instance_id", ""),
        "messages": len(data.get("messages", [])),
        "description": data.get("description", ""),
    }


class SessionHistory:
    """Manages saved sessions on disk."""

    def __init__(self, config: RikuganConfig):
        self._dir = os.path.join(config.checkpoints_dir, "sessions")
        os.makedirs(self._dir, exist_ok=True)

    def _session_path(self, session_id: str) -> str:
        return os.path.join(self._dir, f"{session_id}.json")

    def _summary_path(self, session_id: str) -> str:
        return os.path.join(self._dir, f"{session_id}{_SUMMARY_SUFFIX}")

    def save_session(self, session: SessionState, description: str = "") -> str:
        """Save a session and return the file path."""
        path = self._session_path(session.id)
        db_path = _normalize_db_path(session.idb_path)
        data = {
            "schema_version": SESSION_SCHEMA_VERSION,
            "id": session.id,
            "created_at": session.created_at,
            "last_active_at": session.last_active_at,
            "provider_name": session.provider_name,
            "model_name": session.model_name,
            "idb_path": db_path,
            "db_instance_id": session.db_instance_id,
            "current_turn": session.current_turn,
            "metadata": session.metadata,
            "messages": [m.to_dict() for m in session.messages],
        }
        if session.subagent_logs:
            data["subagent_logs"] = {key: [m.to_dict() for m in msgs] for key, msgs in session.subagent_logs.items()}
        if description:
            data["description"] = description
        with open(path, "w", encoding="utf-8") as f:
            json.dump(data, f, indent=2)
        summary_path = self._summary_path(session.id)
        with open(summary_path, "w", encoding="utf-8") as f:
            json.dump(_build_summary_data(data, session.id), f, indent=2)
        return path

    def load_session(self, session_id: str) -> SessionState | None:
        """Load a session by ID. Returns None if not found or corrupt."""
        path = self._session_path(session_id)
        if not os.path.exists(path):
            return None
        try:
            with open(path, encoding="utf-8") as f:
                data = json.load(f)
        except (json.JSONDecodeError, UnicodeDecodeError, OSError) as exc:
            log_debug(f"Failed to load session {session_id}: {exc}")
            return None
        session = SessionState(
            id=data["id"],
            created_at=data.get("created_at", 0),
            last_active_at=data.get("last_active_at", data.get("created_at", 0)),
            provider_name=data.get("provider_name", ""),
            model_name=data.get("model_name", ""),
            idb_path=data.get("idb_path", ""),
            db_instance_id=data.get("db_instance_id", ""),
            current_turn=data.get("current_turn", 0),
            metadata=data.get("metadata", {}),
        )
        for md in data.get("messages", []):
            session.messages.append(Message.from_dict(md))
        for key, msg_dicts in data.get("subagent_logs", {}).items():
            session.subagent_logs[key] = [Message.from_dict(md) for md in msg_dicts]
        return session

    def list_sessions(self, idb_path: str = "", db_instance_id: str = "") -> list[dict[str, Any]]:
        """List saved session summaries, filtered by IDB path and instance ID."""
        sessions = []
        normalized_target = _normalize_db_path(idb_path)
        for fname in sorted(os.listdir(self._dir), reverse=True):
            if not fname.endswith(_SUMMARY_SUFFIX):
                continue
            path = os.path.join(self._dir, fname)
            try:
                with open(path, encoding="utf-8") as f:
                    entry = json.load(f)
                # When db_instance_id is provided, use it as the primary key
                # (UUIDs are globally unique, so path matching is redundant).
                # This handles BN where the path may change between raw binary
                # and .bndb across sessions.
                if db_instance_id:
                    if entry["db_instance_id"] != db_instance_id:
                        continue
                elif normalized_target:
                    if entry["idb_path"] != normalized_target:
                        continue
                else:
                    # No idb_path or instance_id — only return sessions with no idb_path
                    if entry["idb_path"]:
                        continue
                sessions.append(entry)
            except (json.JSONDecodeError, UnicodeDecodeError, OSError, KeyError) as exc:
                log_debug(f"Skipping corrupt session summary {fname}: {exc}")
                continue
        return sessions

    def get_latest_session(self, idb_path: str = "", db_instance_id: str = "") -> SessionState | None:
        """Load the most recently saved session for this IDB."""
        sessions = self.list_sessions(idb_path=idb_path, db_instance_id=db_instance_id)
        if not sessions:
            return None
        sessions.sort(key=lambda s: s.get("created_at", 0), reverse=True)
        return self.load_session(sessions[0]["id"])

    def delete_session(self, session_id: str) -> bool:
        removed = False
        for path in (self._session_path(session_id), self._summary_path(session_id)):
            if os.path.exists(path):
                os.remove(path)
                removed = True
        return removed

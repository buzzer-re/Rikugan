"""Session history: persist, list, and restore past sessions.

This is the single persistence layer for all session state.
"""

from __future__ import annotations

import json
import os
import time
from typing import Any, Dict, List, Optional

from ..constants import SESSION_SCHEMA_VERSION
from ..core.config import RikuganConfig
from ..core.logging import log_debug
from ..core.types import Message
from .session import SessionState


def _normalize_db_path(path: str) -> str:
    """Return a stable canonical DB path for session filtering."""
    if not path:
        return ""
    try:
        return os.path.normcase(os.path.realpath(os.path.abspath(path)))
    except OSError:
        return path


class SessionHistory:
    """Manages saved sessions on disk."""

    def __init__(self, config: RikuganConfig):
        self._dir = os.path.join(config.checkpoints_dir, "sessions")
        os.makedirs(self._dir, exist_ok=True)

    def save_session(self, session: SessionState, description: str = "") -> str:
        """Save a session and return the file path."""
        path = os.path.join(self._dir, f"{session.id}.json")
        db_path = _normalize_db_path(session.idb_path)
        data = {
            "schema_version": SESSION_SCHEMA_VERSION,
            "id": session.id,
            "created_at": session.created_at,
            "provider_name": session.provider_name,
            "model_name": session.model_name,
            "idb_path": db_path,
            "db_instance_id": session.db_instance_id,
            "current_turn": session.current_turn,
            "metadata": session.metadata,
            "messages": [m.to_dict() for m in session.messages],
        }
        if session.subagent_logs:
            data["subagent_logs"] = {
                key: [m.to_dict() for m in msgs]
                for key, msgs in session.subagent_logs.items()
            }
        if description:
            data["description"] = description
        with open(path, "w") as f:
            json.dump(data, f, indent=2)
        return path

    def load_session(self, session_id: str) -> Optional[SessionState]:
        """Load a session by ID. Returns None if not found or corrupt."""
        path = os.path.join(self._dir, f"{session_id}.json")
        if not os.path.exists(path):
            return None
        try:
            with open(path, "r") as f:
                data = json.load(f)
        except (json.JSONDecodeError, OSError) as exc:
            log_debug(f"Failed to load session {session_id}: {exc}")
            return None
        session = SessionState(
            id=data["id"],
            created_at=data.get("created_at", 0),
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

    def list_sessions(
        self, idb_path: str = "", db_instance_id: str = ""
    ) -> List[Dict[str, Any]]:
        """List saved session summaries, filtered by IDB path and instance ID."""
        sessions = []
        normalized_target = _normalize_db_path(idb_path)
        for fname in sorted(os.listdir(self._dir), reverse=True):
            if not fname.endswith(".json"):
                continue
            path = os.path.join(self._dir, fname)
            try:
                with open(path) as f:
                    data = json.load(f)
                entry = {
                    "id": data.get("id", fname[:-5]),
                    "created_at": data.get("created_at", 0),
                    "provider": data.get("provider_name", ""),
                    "model": data.get("model_name", ""),
                    "idb_path": _normalize_db_path(data.get("idb_path", "")),
                    "db_instance_id": data.get("db_instance_id", ""),
                    "messages": len(data.get("messages", [])),
                    "description": data.get("description", ""),
                }
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
            except (json.JSONDecodeError, OSError) as exc:
                log_debug(f"Skipping corrupt session file {fname}: {exc}")
                continue
        return sessions

    def get_latest_session(
        self, idb_path: str = "", db_instance_id: str = ""
    ) -> Optional[SessionState]:
        """Load the most recently saved session for this IDB."""
        sessions = self.list_sessions(idb_path=idb_path, db_instance_id=db_instance_id)
        if not sessions:
            return None
        sessions.sort(key=lambda s: s.get("created_at", 0), reverse=True)
        return self.load_session(sessions[0]["id"])

    def delete_session(self, session_id: str) -> bool:
        path = os.path.join(self._dir, f"{session_id}.json")
        if os.path.exists(path):
            os.remove(path)
            return True
        return False

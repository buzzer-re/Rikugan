"""Which chat the current tool call belongs to.

Tool handlers take only the model's arguments, and several chats can run at once,
so a module-level "current" would mix them up. A context variable keeps them
apart, and the registry carries it onto the thread that runs the handler.
"""

from __future__ import annotations

from collections.abc import Iterator
from contextlib import contextmanager
from contextvars import ContextVar
from threading import Event
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from ..state.session import SessionState

_active_session: ContextVar[SessionState | None] = ContextVar("rikugan_active_session", default=None)
_active_cancel: ContextVar[Event | None] = ContextVar("rikugan_active_cancel", default=None)


@contextmanager
def activate_session(session: SessionState, cancel: Event | None = None) -> Iterator[None]:
    """Make *session* the current chat. Do not yield a TurnEvent inside this."""
    session_token = _active_session.set(session)
    cancel_token = _active_cancel.set(cancel)
    try:
        yield
    finally:
        _active_cancel.reset(cancel_token)
        _active_session.reset(session_token)


def current_session() -> SessionState | None:
    return _active_session.get()


def current_cancel_event() -> Event | None:
    """Set when the user presses Stop, for a tool that runs long enough to notice."""
    return _active_cancel.get()

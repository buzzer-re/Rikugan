"""Which chat the current tool call belongs to.

Tool handlers take only the model's arguments, and several chats can run at once,
so a module-level "current" would mix them up. A context variable keeps them
apart, and the registry carries it onto the thread that runs the handler.
"""

from __future__ import annotations

from collections.abc import Iterator
from contextlib import contextmanager
from contextvars import ContextVar
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from ..state.session import SessionState

_active_session: ContextVar[SessionState | None] = ContextVar("rikugan_active_session", default=None)


@contextmanager
def activate_session(session: SessionState) -> Iterator[None]:
    """Make *session* the current chat. Do not yield a TurnEvent inside this."""
    token = _active_session.set(session)
    try:
        yield
    finally:
        _active_session.reset(token)


def current_session() -> SessionState | None:
    return _active_session.get()

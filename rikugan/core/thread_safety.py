"""Thread-safety utilities for IDA API access."""

from __future__ import annotations

import functools
import importlib
import threading
from typing import Any, Callable, Optional, TypeVar

F = TypeVar("F", bound=Callable[..., Any])

from ..constants import IDA_AVAILABLE as _IDA_AVAILABLE, BINARY_NINJA_AVAILABLE as _BN_AVAILABLE
if _IDA_AVAILABLE:
    ida_kernwin = importlib.import_module("ida_kernwin")
if _BN_AVAILABLE:
    try:
        bn_mainthread = importlib.import_module("binaryninja.mainthread")
    except ImportError:
        bn_mainthread = None
else:
    bn_mainthread = None


_TRACE_ENABLED: Optional[bool] = None


def _log(msg: str) -> None:
    """Low-level log that avoids circular imports with logging.py.

    Skips the call entirely when TRACE-level logging is disabled to avoid
    the overhead of eager f-string formatting on every idasync dispatch.
    """
    global _TRACE_ENABLED
    try:
        import logging as _logging
        from .logging import get_logger, log_trace
        if _TRACE_ENABLED is None:
            _TRACE_ENABLED = get_logger().isEnabledFor(_logging.DEBUG)
        if not _TRACE_ENABLED:
            return
        log_trace(msg)
    except ImportError:
        return  # logging module unavailable during early bootstrap — skip silently


def idasync(func: F) -> F:
    """Decorator: execute *func* on host main thread when required.

    IDA: uses ``ida_kernwin.execute_sync`` with ``MFF_WRITE``.
    Binary Ninja: uses ``binaryninja.mainthread.execute_on_main_thread_and_wait``.
    Other hosts: executes directly.
    """
    @functools.wraps(func)
    def wrapper(*args: Any, **kwargs: Any) -> Any:
        fname = func.__name__
        on_main = threading.current_thread() is threading.main_thread()

        if _IDA_AVAILABLE:
            if on_main:
                _log(f"idasync: {fname} on main thread — direct call")
                return func(*args, **kwargs)

            _log(f"idasync: {fname} on {threading.current_thread().name} — execute_sync START")
            result_holder: list = []
            error_holder: list = []

            def _thunk():
                try:
                    _log(f"idasync: {fname} _thunk executing on main thread")
                    result_holder.append(func(*args, **kwargs))
                    _log(f"idasync: {fname} _thunk OK")
                except Exception as exc:
                    _log(f"idasync: {fname} _thunk ERROR: {exc}")
                    error_holder.append(exc)
                return 0

            rc = ida_kernwin.execute_sync(_thunk, ida_kernwin.MFF_WRITE)
            _log(f"idasync: {fname} execute_sync returned rc={rc}")

            if error_holder:
                raise error_holder[0]
            return result_holder[0] if result_holder else None

        if _BN_AVAILABLE:
            if on_main:
                _log(f"bnsync: {fname} on main thread — direct call")
                return func(*args, **kwargs)

            exec_wait = getattr(bn_mainthread, "execute_on_main_thread_and_wait", None)
            if not callable(exec_wait):
                _log(f"bnsync: {fname} no execute_on_main_thread_and_wait — direct call fallback")
                return func(*args, **kwargs)

            _log(f"bnsync: {fname} on {threading.current_thread().name} — execute_on_main_thread_and_wait START")
            result_holder: list = []
            error_holder: list = []

            def _thunk() -> None:
                try:
                    _log(f"bnsync: {fname} _thunk executing on main thread")
                    result_holder.append(func(*args, **kwargs))
                    _log(f"bnsync: {fname} _thunk OK")
                except Exception as exc:
                    _log(f"bnsync: {fname} _thunk ERROR: {exc}")
                    error_holder.append(exc)

            exec_wait(_thunk)
            _log(f"bnsync: {fname} execute_on_main_thread_and_wait DONE")

            if error_holder:
                raise error_holder[0]
            return result_holder[0] if result_holder else None

        return func(*args, **kwargs)

    return wrapper  # type: ignore[return-value]


def run_in_background(func: Callable[..., Any], *args: Any, **kwargs: Any) -> threading.Thread:
    """Run *func* in a daemon background thread."""
    thread = threading.Thread(target=func, args=args, kwargs=kwargs, daemon=True)
    thread.start()
    return thread

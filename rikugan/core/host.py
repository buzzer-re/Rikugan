"""Host/runtime detection and context utilities.

This module centralizes runtime integration points so Rikugan can run inside
multiple reverse-engineering hosts (IDA Pro, Binary Ninja, or standalone).
"""

from __future__ import annotations

import importlib
import os
import sys
import threading
from pathlib import Path
from typing import Any, Callable, Optional


HOST_IDA = "ida"
HOST_BINARY_NINJA = "binary_ninja"
HOST_STANDALONE = "standalone"

_HOST = HOST_STANDALONE
_idc = None
_idaapi = None
_ida_kernwin = None
try:
    _idaapi = importlib.import_module("idaapi")
    _HOST = HOST_IDA
    # Cache frequently-used IDA modules to avoid repeated importlib lookups.
    # Both are optional — headless/batch IDA may not expose them.
    try:
        _idc = importlib.import_module("idc")
    except ImportError:
        _idc = None  # optional — absent in some IDA headless configurations
    try:
        _ida_kernwin = importlib.import_module("ida_kernwin")
    except ImportError:
        _ida_kernwin = None  # optional — absent in some IDA headless configurations
except ImportError:
    try:
        importlib.import_module("binaryninja")
        _HOST = HOST_BINARY_NINJA
    except ImportError:
        _HOST = HOST_STANDALONE


_ctx_lock = threading.RLock()
_bn_bv: Any = None
_bn_address: Optional[int] = None
_bn_navigate_cb: Optional[Callable[[int], bool]] = None


def host_kind() -> str:
    """Return the active runtime host: ida, binary_ninja, or standalone."""
    return _HOST


def is_ida() -> bool:
    return _HOST == HOST_IDA


def is_binary_ninja() -> bool:
    return _HOST == HOST_BINARY_NINJA


def host_display_name() -> str:
    if _HOST == HOST_IDA:
        return "IDA Pro"
    if _HOST == HOST_BINARY_NINJA:
        return "Binary Ninja"
    return "Standalone Python"


def set_binary_ninja_context(
    bv: Any = None,
    address: Optional[int] = None,
    navigate_cb: Optional[Callable[[int], bool]] = None,
) -> None:
    """Update active Binary Ninja runtime context."""
    with _ctx_lock:
        global _bn_bv, _bn_address, _bn_navigate_cb
        if bv is not None:
            _bn_bv = bv
        if address is not None:
            _bn_address = int(address)
        if navigate_cb is not None:
            _bn_navigate_cb = navigate_cb


def get_binary_ninja_view() -> Any:
    """Return the most recently active Binary Ninja BinaryView."""
    if not is_binary_ninja():
        return None
    with _ctx_lock:
        return _bn_bv


def get_current_address() -> Optional[int]:
    """Return current cursor/address from host context if available."""
    if is_ida():
        try:
            return int(_idc.get_screen_ea()) if _idc else None
        except Exception:
            return None

    if is_binary_ninja():
        with _ctx_lock:
            if _bn_address is not None:
                return int(_bn_address)
        # Best-effort UI query fallback when explicit context isn't set.
        try:
            bnui = importlib.import_module("binaryninjaui")
            ui_ctx = getattr(bnui, "UIContext", None)
            if ui_ctx is None:
                return None
            active = ui_ctx.activeContext()
            if active is None:
                return None
            vf = active.getCurrentViewFrame()
            if vf is None:
                return None
            vi = vf.getCurrentViewInterface()
            if vi is not None and hasattr(vi, "getCurrentOffset"):
                return int(vi.getCurrentOffset())
            if hasattr(vf, "getCurrentOffset"):
                return int(vf.getCurrentOffset())
        except Exception:
            return None

    return None


def set_current_address(address: int) -> None:
    """Set current address in runtime context (used by host integrations)."""
    if is_binary_ninja():
        with _ctx_lock:
            global _bn_address
            _bn_address = int(address)


def navigate_to(address: int) -> bool:
    """Navigate UI to an address when the host supports it."""
    ea = int(address)

    if is_ida():
        try:
            return bool(_ida_kernwin.jumpto(ea)) if _ida_kernwin else False
        except Exception:
            return False

    if is_binary_ninja():
        with _ctx_lock:
            cb = _bn_navigate_cb
        if cb is not None:
            try:
                ok = bool(cb(ea))
                if ok:
                    set_current_address(ea)
                return ok
            except Exception as e:
                sys.stderr.write(f"[Rikugan] navigate_to_address cb failed at 0x{ea:x}: {e}\n")
        return False

    return False


def get_user_config_base_dir() -> str:
    """Return host-specific user base directory for Rikugan config/log files."""
    if is_ida():
        try:
            return _idaapi.get_user_idadir() if _idaapi else os.path.join(str(Path.home()), ".idapro")
        except Exception:
            return os.path.join(str(Path.home()), ".idapro")

    if is_binary_ninja():
        try:
            bn = importlib.import_module("binaryninja")
            user_directory = getattr(bn, "user_directory", None)
            if callable(user_directory):
                return user_directory()
        except Exception as e:
            sys.stderr.write(f"[Rikugan] get_user_config_base_dir failed: {e}\n")
        return os.path.join(str(Path.home()), ".binaryninja")

    return os.path.join(str(Path.home()), ".idapro")


def get_database_path() -> str:
    """Return the loaded database/binary path for the active host."""
    if is_ida():
        try:
            if _idaapi is None:
                return ""
            idb = _idaapi.get_path(_idaapi.PATH_TYPE_IDB)
            if idb:
                return idb
            return _idaapi.get_input_file_path() or ""
        except Exception:
            return ""

    if is_binary_ninja():
        bv = get_binary_ninja_view()
        if bv is None:
            return ""

        try:
            for attr in ("file", "view_file"):
                fobj = getattr(bv, attr, None)
                if fobj is None:
                    continue
                for fattr in ("filename", "original_filename", "raw_filename"):
                    path = getattr(fobj, fattr, None)
                    if path:
                        return str(path)
        except Exception as e:
            sys.stderr.write(f"[Rikugan] get_database_path file attr failed: {e}\n")

        for attr in ("file_name", "filename", "path"):
            try:
                path = getattr(bv, attr, None)
                if path:
                    return str(path)
            except Exception as e:
                sys.stderr.write(f"[Rikugan] get_database_path {attr} failed: {e}\n")

    return ""


def get_database_instance_id() -> str:
    """Read the Rikugan instance UUID stored in the current IDB/BNDB.

    Returns '' if none is stored yet.
    """
    if is_ida():
        try:
            idaapi = _idaapi
            if idaapi is None:
                return ""
            node = idaapi.netnode("$ rikugan", 0, False)
            if node == idaapi.BADNODE:
                return ""
            val = node.supstr(0)
            return val if isinstance(val, str) and val else ""
        except Exception:
            return ""

    if is_binary_ninja():
        bv = get_binary_ninja_view()
        if bv is None:
            return ""
        try:
            val = bv.query_metadata("rikugan_db_id")
            if val is None:
                return ""
            # query_metadata may return a Metadata wrapper; unwrap if needed.
            raw = getattr(val, "value", val)
            return str(raw) if raw else ""
        except (KeyError, Exception):
            return ""

    return ""


def set_database_instance_id(instance_id: str) -> bool:
    """Store a Rikugan instance UUID in the current IDB/BNDB.

    Returns True on success.
    """
    if is_ida():
        try:
            idaapi = _idaapi
            if idaapi is None:
                return False
            node = idaapi.netnode("$ rikugan", 0, True)
            node.supset(0, instance_id)
            return True
        except Exception as e:
            sys.stderr.write(f"[Rikugan] set_database_instance_id IDA failed: {e}\n")
            return False

    if is_binary_ninja():
        bv = get_binary_ninja_view()
        if bv is None:
            return False
        try:
            bv.store_metadata("rikugan_db_id", instance_id)
            return True
        except Exception as e:
            sys.stderr.write(f"[Rikugan] set_database_instance_id BN failed: {e}\n")
            return False

    return False

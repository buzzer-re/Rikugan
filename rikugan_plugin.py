"""Rikugan - Intelligent Reverse-engineering Integrated System.

IDA Pro plugin entry point.
All rikugan.* imports are deferred to avoid crashes during plugin enumeration.
"""

import builtins
import importlib
import threading

import idaapi

# ---------------------------------------------------------------------------
# Shiboken __import__ hook re-entrancy guard
# ---------------------------------------------------------------------------
# PySide6/Shiboken6 patches builtins.__import__ with a hook.  When this
# hook is invoked during Qt signal dispatch (e.g. submit_requested.emit()),
# and the connected slot's code triggers an import, the hook re-enters
# itself.  After 3-4 levels of nesting the hook accesses freed memory
# (UAF → SIGSEGV in ___lldb_unnamed_symbol945, address looks like ASCII
# string fragment — type-name pointer corruption).
#
# Fix: wrap the hook so that first-level calls go through Shiboken
# normally (preserving PySide6 module wrapping), but nested calls
# (re-entrant) are redirected to CPython's standard import, avoiding
# the corruption.  Installed once and never removed.

_import_guard = threading.local()
_shiboken_import = builtins.__import__


def _guarded_import(*args, **kwargs):
    if getattr(_import_guard, "active", False):
        # Re-entrant call — bypass Shiboken's hook
        return importlib.__import__(*args, **kwargs)
    _import_guard.active = True
    try:
        return _shiboken_import(*args, **kwargs)
    finally:
        _import_guard.active = False


_guarded_import._rikugan_guarded = True  # marker to avoid double-wrapping
if not getattr(builtins.__import__, "_rikugan_guarded", False):
    builtins.__import__ = _guarded_import


class RikuganPlugmod(idaapi.plugmod_t):
    """Per-database plugin module."""

    def __init__(self):
        super().__init__()
        self._panel = None

    def run(self, arg: int) -> bool:
        self._toggle_panel()
        return True

    def term(self) -> None:
        _log("RikuganPlugmod.term() called")
        panel = self._panel
        self._panel = None
        if panel is not None:
            try:
                panel.close()
            except Exception as e:
                idaapi.msg(f"[Rikugan] Panel close error: {e}\n")
        # Flush deferred widget deletions while Python is still alive.
        # Without this, orphaned PySide6-wrapped QFrames survive until
        # QApplication::~QApplication() where their C++ destructors call
        # disconnectNotify -> PyErr_Occurred on a dead interpreter -> crash.
        try:
            from PySide6.QtWidgets import QApplication
            QApplication.processEvents()
        except Exception:
            pass

    def _toggle_panel(self) -> None:
        try:
            _log("_toggle_panel: entry")
            if self._panel is not None:
                _log("_toggle_panel: panel exists, calling show()")
                self._panel.show()
                return

            # Bulk-load all rikugan.* modules via importlib.import_module().
            #
            # importlib.import_module() routes through CPython's internal
            # _gcd_import(), which does NOT call builtins.__import__.  This
            # means Shiboken's __import__ hook is never invoked, avoiding
            # both the UAF crash (from swapping builtins.__import__) and the
            # spurious "No module named 'ida_...'" errors (from Shiboken's
            # hook failing to resolve IDA modules during bulk loading).
            #
            # All ida_* imports inside rikugan modules also use
            # importlib.import_module() for the same reason.
            _log("_toggle_panel: importing rikugan modules")
            import pkgutil
            import rikugan

            # Use iter_modules (discovery only, no __import__) + manual
            # recursion via importlib.import_module().  pkgutil.walk_packages()
            # calls __import__() internally for sub-packages, which goes
            # through Shiboken's hook and can trigger UAF crashes.
            def _load_submodules(pkg):
                for _finder, modname, ispkg in pkgutil.iter_modules(
                    pkg.__path__, prefix=pkg.__name__ + "."
                ):
                    try:
                        mod = importlib.import_module(modname)
                        if ispkg:
                            _load_submodules(mod)
                    except Exception as e:
                        import sys; sys.stderr.write(f"[Rikugan] Skipping {modname}: {e}\n")

            # Temporarily bypass Shiboken's __import__ hook during bulk
            # loading.  importlib.import_module() itself avoids __import__,
            # but *executed* module code (e.g. ``from PySide6 import ...``
            # in qt_compat.py) emits IMPORT_NAME bytecode that calls
            # builtins.__import__.  On Python 3.14 this can cause a deep
            # recursive re-entry into Shiboken that triggers a UAF crash.
            #
            # PySide6 modules are already in sys.modules (loaded by IDA's
            # own UI), so CPython's import machinery just needs a dict
            # lookup — Shiboken's type-wrapper initialisation is not needed.
            saved_import = builtins.__import__
            builtins.__import__ = importlib.__import__
            try:
                _load_submodules(rikugan)
            finally:
                builtins.__import__ = saved_import

            _log("_toggle_panel: all rikugan modules loaded")
            RikuganPanel = importlib.import_module("rikugan.ida.ui.panel").RikuganPanel

            _log("_toggle_panel: creating RikuganPanel()")
            self._panel = RikuganPanel()
            _log("_toggle_panel: calling show()")
            self._panel.show()
            _log("_toggle_panel: done")
        except Exception as e:
            import sys
            import traceback
            tb_str = traceback.format_exc()
            idaapi.msg(f"[Rikugan] Failed to open panel: {e}\n{tb_str}\n")
            try:
                importlib.import_module("rikugan.core.logging").log_error(
                    f"Failed to open panel: {e}\n{tb_str}"
                )
            except Exception:
                try:
                    import os
                    log_path = os.path.join(os.path.expanduser("~"), ".idapro", "rikugan", "rikugan_debug.log")
                    with open(log_path, "a") as f:
                        f.write(f"[Rikugan CRASH] {e}\n{tb_str}\n")
                        f.flush()
                        os.fsync(f.fileno())
                except Exception:
                    print(f"[Rikugan CRASH] {e}\n{tb_str}", file=sys.stderr)


class RikuganPlugin(idaapi.plugin_t):
    flags = idaapi.PLUGIN_MULTI | idaapi.PLUGIN_FIX
    comment = "Intelligent Reverse-engineering Integrated System"
    help = ""
    wanted_name = "Rikugan"
    wanted_hotkey = "Ctrl+Shift+I"

    def init(self) -> idaapi.plugmod_t:
        _ver = importlib.import_module("rikugan.constants").PLUGIN_VERSION
        idaapi.msg(f"[Rikugan] Plugin loaded (v{_ver})\n")
        return RikuganPlugmod()


def _log(msg: str) -> None:
    """Best-effort log to IDA output and debug file."""
    idaapi.msg(f"[Rikugan] {msg}\n")
    try:
        importlib.import_module("rikugan.core.logging").log_trace(msg)
    except Exception as e:
        import sys; sys.stderr.write(f"[Rikugan] log_trace unavailable during bootstrap: {e}\n")


def PLUGIN_ENTRY():  # noqa: N802
    return RikuganPlugin()

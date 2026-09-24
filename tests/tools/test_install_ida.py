"""The IDA installer's Python detection.

A macOS framework dylib such as

    .../Python.framework/Versions/3.14/Python

carries the executable bit, so testing it with ``-x`` accepted a shared library
as the interpreter and the install died with "cannot execute binary file".
These run the shell function directly against fabricated layouts.
"""

from __future__ import annotations

import os
import re
import subprocess
import sys
import tempfile
import unittest

_REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
_INSTALLER = os.path.join(_REPO_ROOT, "install_ida.sh")

# Mach-O and ELF magic: executable bit set, but not something exec() can run.
_MACHO = b"\xcf\xfa\xed\xfe\x0c\x00\x00\x01"
_ELF = b"\x7fELF"
_PY_STUB = '#!/bin/sh\necho RIKUGAN_PY3\n'


def _shell_functions() -> str:
    """Extract just the detection helpers; the installer has no source guard."""
    source = open(_INSTALLER, encoding="utf-8").read()
    required = ("_extract_python_version", "_python_target_to_interpreter")
    # Pulled in when present, so removing the guard fails these on what it
    # actually returns rather than on a missing symbol.
    optional = ("_is_python_interpreter",)
    out = []
    for name in required + optional:
        match = re.search(rf"^{name}\(\) \{{.*?^\}}", source, re.MULTILINE | re.DOTALL)
        if match is None:
            assert name in optional, f"{name} not found in install_ida.sh"
            continue
        out.append(match.group(0))
    return "\n\n".join(out)


def _resolve(target: str) -> str:
    """Run _python_target_to_interpreter and return its output ('' on failure)."""
    script = f'{_shell_functions()}\n_python_target_to_interpreter "$1" || true\n'
    result = subprocess.run(
        ["bash", "-c", script, "bash", target],
        capture_output=True,
        text=True,
        timeout=30,
        check=False,
    )
    return result.stdout.strip()


def _write(path: str, data: bytes | str) -> str:
    os.makedirs(os.path.dirname(path), exist_ok=True)
    mode = "wb" if isinstance(data, bytes) else "w"
    with open(path, mode) as f:
        f.write(data)
    os.chmod(path, 0o755)
    return path


@unittest.skipUnless(sys.platform != "win32", "the installer is a shell script")
class TestPythonTargetToInterpreter(unittest.TestCase):
    def test_framework_dylib_resolves_to_the_versioned_interpreter(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = os.path.join(tmp, "Python.framework", "Versions", "3.14")
            dylib = _write(os.path.join(base, "Python"), _MACHO)
            interp = _write(os.path.join(base, "bin", "python3.14"), _PY_STUB)
            self.assertEqual(_resolve(dylib), interp)

    def test_a_dylib_is_never_returned_as_the_interpreter(self):
        """With no interpreter beside it, detection must fail so the caller
        falls back to system Python rather than exec'ing a shared library."""
        with tempfile.TemporaryDirectory() as tmp:
            base = os.path.join(tmp, "Python.framework", "Versions", "9.9")
            dylib = _write(os.path.join(base, "Python"), _MACHO)
            self.assertEqual(_resolve(dylib), "")

    def test_framework_without_a_versioned_name_uses_python3(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = os.path.join(tmp, "Python.framework", "Versions", "3.12")
            dylib = _write(os.path.join(base, "Python"), _MACHO)
            interp = _write(os.path.join(base, "bin", "python3"), _PY_STUB)
            self.assertEqual(_resolve(dylib), interp)

    def test_linux_libpython_resolves_to_the_sibling_bin(self):
        with tempfile.TemporaryDirectory() as tmp:
            lib = _write(os.path.join(tmp, "lib", "libpython3.11.so.1.0"), _ELF)
            _write(os.path.join(tmp, "bin", "python3.11"), _PY_STUB)
            self.assertTrue(_resolve(lib).endswith("bin/python3.11"))

    def test_a_real_interpreter_passes_straight_through(self):
        self.assertEqual(_resolve(sys.executable), sys.executable)

    def test_a_wrapper_that_ignores_its_arguments_is_rejected(self):
        """Exiting zero is not enough; installing into it would be silent."""
        with tempfile.TemporaryDirectory() as tmp:
            base = os.path.join(tmp, "Python.framework", "Versions", "3.9")
            dylib = _write(os.path.join(base, "Python"), _MACHO)
            _write(os.path.join(base, "bin", "python3.9"), "#!/bin/sh\nexit 0\n")
            self.assertEqual(_resolve(dylib), "")


if __name__ == "__main__":
    unittest.main()

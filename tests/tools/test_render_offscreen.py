"""Run the offscreen Qt render checks.

These run in a subprocess on purpose: the rest of the suite installs stub
PySide6 modules in ``sys.modules``, so real Qt widgets can only be built in a
separate process. They cover what no unit test can see — that a bubble is as
tall as its text, that an empty one is hidden, and that body text is readable.

CI installs PySide6 in the Pytest job, so these run there. They are skipped
wherever Qt is absent, including the Mypy job — resolving the Qt bindings
changes what mypy reports about the UI package, so it is deliberately kept out
of the shared dev environment. To run them locally:

    pip install PySide6-Essentials   # plus libegl1/libgl1 on Linux
"""

from __future__ import annotations

import os
import subprocess
import sys
import unittest

_REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
_SCRIPT = os.path.join(_REPO_ROOT, "tests", "render", "render_checks.py")


def _qt_available() -> bool:
    probe = subprocess.run(
        [sys.executable, "-c", "import PySide6.QtWidgets"],
        capture_output=True,
        check=False,
    )
    return probe.returncode == 0


class TestOffscreenRender(unittest.TestCase):
    @unittest.skipUnless(_qt_available(), "PySide6 not installed")
    def test_chat_panel_renders_without_layout_defects(self):
        env = dict(os.environ, QT_QPA_PLATFORM="offscreen")
        result = subprocess.run(
            [sys.executable, _SCRIPT],
            capture_output=True,
            text=True,
            env=env,
            cwd=_REPO_ROOT,
            timeout=120,
            check=False,
        )
        if result.returncode != 0:
            self.fail(f"render checks failed:\n{result.stdout}\n{result.stderr}")


if __name__ == "__main__":
    unittest.main()

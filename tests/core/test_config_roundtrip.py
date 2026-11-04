"""Every setting has to survive a restart.

save() writes every dataclass field, but load() reads from an explicit list, so
a field missing from that list is written faithfully and silently reset on the
next start. Settings dialogs make that look like the checkbox is broken.
"""

from __future__ import annotations

import dataclasses
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(__file__)))
from tests.mocks.ida_mock import install_ida_mocks

install_ida_mocks()

sys.modules.pop("rikugan.core.config", None)

from rikugan.core.config import RikuganConfig  # noqa: E402

# Not plain settings: load() handles these itself, ahead of the list.
_LOADED_SEPARATELY = {"provider", "providers", "custom_providers"}


def _changed(value):
    """A valid value of the same type that differs from the default."""
    if isinstance(value, bool):
        return not value
    if isinstance(value, int):
        return value + 1
    if isinstance(value, str):
        return value + "x"
    if isinstance(value, list):
        return [*value, "x"]
    if isinstance(value, dict):
        return {**value, "x": {}}
    raise AssertionError(f"teach _changed about {type(value).__name__}")


def _settings():
    return [
        f.name
        for f in dataclasses.fields(RikuganConfig)
        if not f.name.startswith("_") and f.name not in _LOADED_SEPARATELY
    ]


class TestSettingsSurviveARestart(unittest.TestCase):
    def test_every_setting_round_trips(self):
        config_dir = tempfile.mkdtemp()
        saved = RikuganConfig()
        saved._config_dir = config_dir
        expected = {}
        for name in _settings():
            expected[name] = _changed(getattr(saved, name))
            setattr(saved, name, expected[name])
        saved.save()

        loaded = RikuganConfig()
        loaded._config_dir = config_dir
        loaded.load()

        for name, value in expected.items():
            with self.subTest(setting=name):
                self.assertEqual(getattr(loaded, name), value)


if __name__ == "__main__":
    unittest.main()

"""Every setting has to survive a restart.

save() writes every dataclass field, but load() reads from an explicit list, so
a field missing from that list is written faithfully and silently reset on the
next start. Settings dialogs make that look like the checkbox is broken.
"""

from __future__ import annotations

import dataclasses
import json
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


class TestSaveSetting(unittest.TestCase):
    def _config_with_file(self, stored):
        config = RikuganConfig()
        config._config_dir = tempfile.mkdtemp()
        with open(config.config_path, "w", encoding="utf-8") as f:
            json.dump(stored, f)
        config.load()
        return config

    def _stored(self, config):
        with open(config.config_path, encoding="utf-8") as f:
            return json.load(f)

    def test_changes_the_value_in_memory_and_on_disk(self):
        config = self._config_with_file({})
        config.save_setting("filesystem_access", True)
        self.assertTrue(config.filesystem_access)
        self.assertIs(self._stored(config)["filesystem_access"], True)

    def test_leaves_encrypted_keys_alone(self):
        # save() with no password turns encryption off, and keys not yet decrypted
        # are gone. Answering a consent prompt must never cost someone their keys.
        block = {"enabled": True, "ciphertext": "SECRET-BLOB", "salt": "s"}
        config = self._config_with_file({"provider": {"api_key": ""}, "encryption": block})
        config.save_setting("binja_mcp_consent", {"db": True})
        self.assertEqual(self._stored(config)["encryption"], block)

    def test_works_with_no_config_file_yet(self):
        config = RikuganConfig()
        config._config_dir = os.path.join(tempfile.mkdtemp(), "new")
        config.save_setting("filesystem_access", True)
        self.assertIs(self._stored(config)["filesystem_access"], True)

    def test_leaves_no_temporary_file(self):
        config = self._config_with_file({})
        config.save_setting("filesystem_access", True)
        self.assertEqual(os.listdir(config.config_dir), [os.path.basename(config.config_path)])


if __name__ == "__main__":
    unittest.main()

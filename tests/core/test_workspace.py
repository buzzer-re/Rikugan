"""The path checks standing between a steered model and the user's files."""

from __future__ import annotations

import os
import stat
import sys
import tempfile
import threading
import unittest
from unittest.mock import patch

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(__file__))))

from rikugan.core.workspace import (  # noqa: E402
    MAX_READ_BYTES,
    MAX_WRITE_BYTES,
    PathRefused,
    Workspace,
    atomic_write,
    require_readable_text,
    require_writable_size,
)


class _Tree(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.top = os.path.realpath(tmp.name)
        self.bin_dir = os.path.join(self.top, "case12")
        self.outside = os.path.join(self.top, "outside")
        os.makedirs(self.bin_dir)
        os.makedirs(self.outside)
        self.binary = os.path.join(self.bin_dir, "sample.exe")
        self._touch(self.binary)
        self._touch(os.path.join(self.outside, "secret.txt"))
        self.ws = self._workspace()

    def _workspace(self, **kwargs):
        return Workspace.for_database(
            self.binary + ".i64",
            os.path.join(self.top, "config"),
            "abc",
            input_path=self.binary,
            home=os.path.join(self.top, "home"),
            **kwargs,
        )

    def _touch(self, path, data=b"x"):
        with open(path, "wb") as f:
            f.write(data)


class TestResolveAcceptsTheWorkspace(_Tree):
    def test_relative_paths_start_at_the_binary_directory(self):
        self.assertEqual(self.ws.resolve("notes.txt"), os.path.join(self.bin_dir, "notes.txt"))

    def test_absolute_path_inside(self):
        path = os.path.join(self.bin_dir, "notes.txt")
        self.assertEqual(self.ws.resolve(path), path)

    def test_scratch_alias(self):
        self.assertEqual(self.ws.resolve("scratch/a.py"), os.path.join(self.ws.scratch_root, "a.py"))

    def test_bare_scratch_is_the_scratch_directory(self):
        self.assertEqual(os.path.normpath(self.ws.resolve("scratch")), self.ws.scratch_root)

    def test_scratch_is_not_beside_the_binary(self):
        # Sample directories are often read-only or tracked for provenance.
        self.assertFalse(self.ws.scratch_root.startswith(self.bin_dir))

    def test_scratch_is_per_database(self):
        other = Workspace.for_database(self.binary, os.path.join(self.top, "config"), "other")
        self.assertNotEqual(self.ws.scratch_root, other.scratch_root)

    def test_an_instance_id_cannot_move_scratch(self):
        ws = Workspace.for_database(self.binary, os.path.join(self.top, "config"), "../../..")
        self.assertTrue(ws.scratch_root.startswith(os.path.join(self.top, "config", "scratch")))

    def test_dotdot_that_stays_inside_is_fine(self):
        self.assertEqual(self.ws.resolve("a/../b.txt"), os.path.join(self.bin_dir, "b.txt"))

    def test_configured_extra_root(self):
        ws = self._workspace(extra_roots=[self.outside])
        secret = os.path.join(self.outside, "secret.txt")
        self.assertEqual(ws.resolve(secret), secret)


class TestResolveRefusesEscapes(_Tree):
    def _refused(self, raw, **kwargs):
        with self.assertRaises(PathRefused) as ctx:
            self.ws.resolve(raw, **kwargs)
        return str(ctx.exception)

    def test_each_escape_for_read_and_write(self):
        escapes = {
            "dotdot": "../outside/secret.txt",
            "absolute": os.path.join(self.outside, "secret.txt"),
            "scratch dotdot": "scratch/../../../../outside/secret.txt",
            "etc": "/etc/passwd",
        }
        for label, raw in escapes.items():
            for for_write in (False, True):
                with self.subTest(label, for_write=for_write):
                    self._refused(raw, for_write=for_write)

    def test_a_refusal_names_the_roots_so_the_model_can_recover(self):
        message = self._refused("/etc/passwd")
        self.assertIn("/etc/passwd", message)
        self.assertIn(self.bin_dir, message)
        self.assertIn("scratch/", message)

    def test_a_sibling_sharing_the_prefix_is_outside(self):
        # A startswith check would accept this.
        evil = self.bin_dir + "_evil"
        os.makedirs(evil)
        self._refused(os.path.join(evil, "x.txt"))

    def test_a_symlink_inside_pointing_out(self):
        link = os.path.join(self.bin_dir, "link")
        os.symlink(self.outside, link)
        self._refused("link/secret.txt")
        self._refused("link/new.txt", for_write=True)

    def test_a_file_symlink_inside_pointing_out(self):
        os.symlink(os.path.join(self.outside, "secret.txt"), os.path.join(self.bin_dir, "s"))
        self._refused("s")

    def test_tilde_is_never_expanded(self):
        self._refused("~/.ssh/id_rsa")

    def test_empty(self):
        self._refused("")
        self._refused("   ")

    def test_nul_byte(self):
        self._refused("a\x00.txt")

    def test_artifacts_are_not_paths(self):
        self.assertIn("read_file", self._refused("artifact://3"))


class TestBinaryDirectoryThatIsTooBroad(_Tree):
    def _ws_for(self, binary, home):
        return Workspace.for_database(binary, os.path.join(self.top, "config"), "abc", home=home)

    def test_a_sample_in_home_does_not_expose_home(self):
        home = os.path.join(self.top, "home")
        os.makedirs(home)
        ws = self._ws_for(os.path.join(home, "sample.exe"), home)
        self.assertEqual(ws.binary_root, "")
        with self.assertRaises(PathRefused):
            ws.resolve(os.path.join(home, ".ssh", "id_rsa"))

    def test_nor_does_an_ancestor_of_home(self):
        home = os.path.join(self.top, "users", "me")
        os.makedirs(home)
        ws = self._ws_for(os.path.join(self.top, "users", "sample.exe"), home)
        self.assertEqual(ws.binary_root, "")

    def test_a_folder_under_home_is_fine(self):
        home = os.path.join(self.top, "home")
        case = os.path.join(home, "cases", "one")
        os.makedirs(case)
        ws = self._ws_for(os.path.join(case, "sample.exe"), home)
        self.assertEqual(ws.binary_root, case)

    def test_relative_paths_fall_back_to_scratch(self):
        home = os.path.join(self.top, "home")
        os.makedirs(home)
        ws = self._ws_for(os.path.join(home, "sample.exe"), home)
        self.assertEqual(ws.resolve("a.txt"), os.path.join(ws.scratch_root, "a.txt"))

    def test_no_database_means_scratch_only(self):
        ws = Workspace.for_database("", os.path.join(self.top, "config"))
        self.assertEqual(ws.roots, (ws.scratch_root,))


class TestWriteRules(_Tree):
    def _refused(self, raw):
        with self.assertRaises(PathRefused):
            self.ws.resolve(raw, for_write=True)

    def test_a_new_file_in_a_new_directory(self):
        self.assertEqual(
            self.ws.resolve("out/deep/a.py", for_write=True),
            os.path.join(self.bin_dir, "out", "deep", "a.py"),
        )

    def test_never_through_a_final_symlink_even_one_pointing_inside(self):
        target = os.path.join(self.bin_dir, "real.txt")
        self._touch(target)
        os.symlink(target, os.path.join(self.bin_dir, "alias.txt"))
        self._refused("alias.txt")

    def test_the_input_binary(self):
        self._refused("sample.exe")

    def test_the_open_database_and_its_companions(self):
        for name in ("sample.exe.i64", "sample.exe.id0", "sample.exe.id1", "sample.exe.id2", "sample.exe.nam"):
            with self.subTest(name):
                self._refused(name)

    def test_any_database_whichever_case(self):
        for name in ("other.idb", "other.i64", "other.bndb", "OTHER.BNDB"):
            with self.subTest(name):
                self._refused(name)

    def test_the_protected_check_ignores_case(self):
        # macOS and Windows filesystems treat these as the same file.
        self._refused("SAMPLE.EXE")

    def test_a_mixed_case_binary_is_protected_however_it_is_spelled(self):
        binary = os.path.join(self.bin_dir, "Sample.EXE")
        self._touch(binary)
        ws = Workspace.for_database(binary, os.path.join(self.top, "config"), "abc", home=os.path.join(self.top, "h"))
        for name in ("Sample.EXE", "sample.exe"):
            with self.subTest(name), self.assertRaises(PathRefused):
                ws.resolve(name, for_write=True)

    def test_an_unrelated_til_is_an_ordinary_file(self):
        self.assertTrue(self.ws.resolve("types.til", for_write=True))

    def test_persistent_memory(self):
        self._refused("RIKUGAN.md")
        self._refused("rikugan.MD")

    def test_undo_backups(self):
        self._refused("scratch/.backups/0000000000000000-00000000")

    def test_a_directory(self):
        os.makedirs(os.path.join(self.bin_dir, "d"))
        self._refused("d")

    def test_paths_that_name_no_file(self):
        self._refused("..")
        self._refused("a/..")

    def test_all_of_those_stay_readable(self):
        self.assertEqual(self.ws.resolve("sample.exe"), self.binary)
        self.assertTrue(self.ws.resolve("RIKUGAN.md"))


class TestReadableText(_Tree):
    def _refused(self, path):
        with self.assertRaises(PathRefused) as ctx:
            require_readable_text(path)
        return str(ctx.exception)

    def test_a_text_file_passes(self):
        path = os.path.join(self.bin_dir, "a.txt")
        self._touch(path, b"hello\n")
        require_readable_text(path)

    def test_missing(self):
        self.assertIn("does not exist", self._refused(os.path.join(self.bin_dir, "nope")))

    def test_directory_points_at_search(self):
        self.assertIn("search_files", self._refused(self.bin_dir))

    def test_binary(self):
        path = os.path.join(self.bin_dir, "b")
        self._touch(path, b"MZ\x00\x01")
        self.assertIn("binary", self._refused(path))

    def test_a_fifo_is_refused_rather_than_hanging(self):
        # On a thread: without the guard open() blocks, and that must fail this
        # test rather than hang the whole run.
        path = os.path.join(self.bin_dir, "pipe")
        os.mkfifo(path)
        messages = []
        worker = threading.Thread(target=lambda: messages.append(self._refused(path)), daemon=True)
        worker.start()
        worker.join(timeout=5)
        if worker.is_alive():
            with open(path, "wb"):  # release the blocked open so the thread can end
                pass
            self.fail("require_readable_text blocked on a FIFO")
        self.assertIn("regular", messages[0])

    def test_over_the_size_limit(self):
        path = os.path.join(self.bin_dir, "big")
        with open(path, "wb") as f:
            f.truncate(MAX_READ_BYTES + 1)
        self.assertIn("limit", self._refused(path))

    def test_write_size(self):
        require_writable_size(b"x" * MAX_WRITE_BYTES)
        with self.assertRaises(PathRefused):
            require_writable_size(b"x" * (MAX_WRITE_BYTES + 1))


class TestAtomicWrite(_Tree):
    def _leftovers(self):
        return [n for n in os.listdir(self.bin_dir) if n.endswith(".tmp")]

    def test_creates_parents(self):
        path = os.path.join(self.bin_dir, "a", "b", "c.txt")
        atomic_write(path, b"data")
        with open(path, "rb") as f:
            self.assertEqual(f.read(), b"data")

    def test_keeps_an_existing_files_mode(self):
        path = os.path.join(self.bin_dir, "run.sh")
        self._touch(path)
        os.chmod(path, 0o755)
        atomic_write(path, b"new")
        self.assertEqual(stat.S_IMODE(os.stat(path).st_mode), 0o755)

    def test_a_new_file_is_not_left_private(self):
        path = os.path.join(self.bin_dir, "new.txt")
        previous = os.umask(0o022)
        self.addCleanup(os.umask, previous)
        atomic_write(path, b"new")
        self.assertEqual(stat.S_IMODE(os.stat(path).st_mode), 0o644)

    def test_failure_keeps_the_old_content_and_no_temp_file(self):
        path = os.path.join(self.bin_dir, "a.txt")
        self._touch(path, b"old")
        with patch("rikugan.core.workspace.os.replace", side_effect=OSError("disk")), self.assertRaises(OSError):
            atomic_write(path, b"new")
        with open(path, "rb") as f:
            self.assertEqual(f.read(), b"old")
        self.assertEqual(self._leftovers(), [])

    def test_success_leaves_no_temp_file(self):
        atomic_write(os.path.join(self.bin_dir, "a.txt"), b"new")
        self.assertEqual(self._leftovers(), [])


class TestBackups(_Tree):
    def _source(self, data=b"before"):
        path = os.path.join(self.bin_dir, "a.txt")
        self._touch(path, data)
        return path

    def test_a_token_finds_its_copy(self):
        token = self.ws.backup_file(self._source())
        with open(self.ws.backup_path(token), "rb") as f:
            self.assertEqual(f.read(), b"before")

    def test_a_token_cannot_traverse(self):
        with self.assertRaises(PathRefused):
            self.ws.backup_path("../../secret")

    def test_a_discarded_backup_says_so(self):
        token = self.ws.backup_file(self._source())
        os.remove(self.ws.backup_path(token))
        with self.assertRaises(PathRefused) as ctx:
            self.ws.backup_path(token)
        self.assertIn("discarded", str(ctx.exception))

    def test_the_file_count_is_capped_oldest_first(self):
        source = self._source()
        with patch("rikugan.core.workspace.MAX_BACKUP_FILES", 3):
            tokens = [self.ws.backup_file(source) for _ in range(5)]
        self.assertEqual(sorted(os.listdir(self.ws.backups_dir)), tokens[-3:])

    def test_the_byte_total_is_capped(self):
        source = self._source(b"x" * 100)
        with patch("rikugan.core.workspace.MAX_BACKUP_BYTES", 250):
            tokens = [self.ws.backup_file(source) for _ in range(4)]
        self.assertEqual(sorted(os.listdir(self.ws.backups_dir)), tokens[-2:])

    def test_the_newest_survives_even_alone_over_the_cap(self):
        source = self._source(b"x" * 100)
        with patch("rikugan.core.workspace.MAX_BACKUP_BYTES", 10):
            token = self.ws.backup_file(source)
        self.assertEqual(os.listdir(self.ws.backups_dir), [token])


if __name__ == "__main__":
    unittest.main()

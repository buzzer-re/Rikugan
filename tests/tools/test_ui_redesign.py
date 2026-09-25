"""Tests for the narrow-panel chat redesign.

Covers the pure logic behind the welcome screen's binary card, the starting
points, the header switcher, the responsive drawer threshold, and the chat
list's group bookkeeping.
"""

from __future__ import annotations

import sys
import unittest
from pathlib import Path
from unittest.mock import MagicMock

from tests.qt_stubs import ensure_pyside6_stubs

ensure_pyside6_stubs()

# test_panel_core registers MagicMock stand-ins for these modules; drop them so
# the real pure-logic implementations are what gets exercised here.
for _stubbed in ("rikugan.ui.binary_summary", "rikugan.ui.panel_header", "rikugan.ui.welcome_view"):
    sys.modules.pop(_stubbed, None)

from rikugan.ui.binary_summary import (  # noqa: E402
    build_binary_summary,
    format_address,
    format_file_type,
    format_processor,
    parse_info_lines,
    parse_total_count,
)
from rikugan.binja import native_mcp  # noqa: E402
from rikugan.ui.panel_header import PanelHeader, elide_title, mcp_label, mcp_tooltip  # noqa: E402
from rikugan.ui.welcome_view import build_suggestions, wrap_chips  # noqa: E402

_IDA_INFO = """File: libcrypt_stub.dylib
Processor: metapc
Bits: 64
Entry point: 0x100F322D0
Min address: 0x100000000
Max address: 0x101000000
File type: Mach-O file (EXECUTE)
Functions: 1284"""

_BN_INFO = """File: /tmp/sample.bndb
Processor: aarch64
Bits: 64
Entry point: 0x4010
File type: Mach-O
Functions: 12"""


# ---------------------------------------------------------------------------
# binary_summary
# ---------------------------------------------------------------------------


class TestParseInfoLines(unittest.TestCase):
    def test_parses_key_value_pairs(self):
        fields = parse_info_lines(_IDA_INFO)
        self.assertEqual(fields["file"], "libcrypt_stub.dylib")
        self.assertEqual(fields["functions"], "1284")

    def test_empty_input_yields_no_fields(self):
        self.assertEqual(parse_info_lines(""), {})

    def test_first_occurrence_wins(self):
        fields = parse_info_lines("File: a\nFile: b")
        self.assertEqual(fields["file"], "a")


class TestParseTotalCount(unittest.TestCase):
    def test_reads_the_pagination_total(self):
        self.assertEqual(parse_total_count("Strings 0-1 of 312:\n  0x1 hello"), 312)

    def test_missing_header_returns_none(self):
        self.assertIsNone(parse_total_count("no pagination here"))


class TestFormatProcessor(unittest.TestCase):
    def test_ida_metapc_becomes_x86_64(self):
        self.assertEqual(format_processor("metapc", "64"), "x86_64")

    def test_arm_gains_its_width(self):
        self.assertEqual(format_processor("arm", "64"), "arm64")

    def test_width_already_present_is_not_repeated(self):
        self.assertEqual(format_processor("aarch64", "64"), "aarch64")

    def test_missing_processor_yields_nothing(self):
        self.assertEqual(format_processor("", "64"), "")


class TestFormatFileType(unittest.TestCase):
    def test_drops_the_file_suffix_and_parenthetical(self):
        self.assertEqual(format_file_type("Mach-O file (EXECUTE)"), "Mach-O")

    def test_prefers_a_short_abbreviation_over_a_long_name(self):
        self.assertEqual(format_file_type("Portable executable for AMD64 (PE)"), "PE")

    def test_long_name_without_abbreviation_is_truncated(self):
        result = format_file_type("Some Extremely Verbose Container Format")
        self.assertEqual(len(result), 14)
        self.assertTrue(result.endswith("…"))


class TestFormatAddress(unittest.TestCase):
    def test_lowercases_the_hex(self):
        self.assertEqual(format_address("0x100F322D0"), "0x100f322d0")

    def test_no_address_yields_nothing(self):
        self.assertEqual(format_address("unavailable"), "")


class TestBuildBinarySummary(unittest.TestCase):
    def test_ida_output_builds_the_card(self):
        summary = build_binary_summary(_IDA_INFO, string_count=312)
        self.assertEqual(summary.name, "libcrypt_stub.dylib")
        self.assertEqual(
            summary.chips,
            ["x86_64", "Mach-O", "1,284 fn", "312 str", "entry 0x100f322d0"],
        )

    def test_binary_ninja_output_builds_the_card(self):
        summary = build_binary_summary(_BN_INFO)
        self.assertEqual(summary.name, "/tmp/sample.bndb")
        self.assertIn("aarch64", summary.chips)
        self.assertIn("12 fn", summary.chips)

    def test_string_count_is_optional(self):
        summary = build_binary_summary(_IDA_INFO)
        self.assertNotIn("312 str", summary.chips)
        self.assertTrue(all("str" not in chip for chip in summary.chips))

    def test_empty_info_is_reported_as_empty(self):
        summary = build_binary_summary("")
        self.assertTrue(summary.is_empty)

    def test_card_shows_the_file_name_not_the_whole_path(self):
        summary = build_binary_summary(_BN_INFO)
        self.assertEqual(summary.short_name, "sample.bndb")
        self.assertEqual(summary.name, "/tmp/sample.bndb")

    def test_short_name_of_an_empty_summary_is_blank(self):
        self.assertEqual(build_binary_summary("").short_name, "")


# ---------------------------------------------------------------------------
# welcome screen
# ---------------------------------------------------------------------------


class TestBuildSuggestions(unittest.TestCase):
    def test_builtin_commands_are_always_offered(self):
        commands = [s.command for s in build_suggestions()]
        self.assertIn("/explore", commands)
        self.assertTrue(any(c.startswith("/modify") for c in commands))

    def test_skill_row_appears_only_when_installed(self):
        without = [s.command for s in build_suggestions([])]
        self.assertNotIn("/deobfuscation", without)
        with_skill = [s.command for s in build_suggestions(["deobfuscation"])]
        self.assertIn("/deobfuscation", with_skill)

    def test_patch_row_shows_the_cursor_address(self):
        rows = build_suggestions(cursor_address=0x100F322D0)
        patch_row = next(s for s in rows if s.prompt.startswith("/modify"))
        self.assertIn("0x100f322d0", patch_row.command)

    def test_first_row_is_a_plain_prompt(self):
        first = build_suggestions()[0]
        self.assertEqual(first.command, "")
        self.assertFalse(first.prompt.startswith("/"))


class TestWrapChips(unittest.TestCase):
    def test_short_chip_set_stays_on_one_row(self):
        self.assertEqual(wrap_chips(["arm64", "Mach-O"]), [["arm64", "Mach-O"]])

    def test_overflowing_chips_wrap(self):
        rows = wrap_chips(["arm64", "Mach-O", "1,284 fn", "312 str", "entry 0x100f322d0"])
        self.assertEqual(len(rows), 2)
        self.assertEqual(rows[0][0], "arm64")
        self.assertEqual(rows[-1][-1], "entry 0x100f322d0")

    def test_no_chips_yields_no_rows(self):
        self.assertEqual(wrap_chips([]), [])

    def test_one_oversized_chip_still_gets_a_row(self):
        self.assertEqual(wrap_chips(["x" * 80]), [["x" * 80]])


# ---------------------------------------------------------------------------
# header
# ---------------------------------------------------------------------------


class TestWelcomeChrome(unittest.TestCase):
    def test_welcome_carries_no_product_mark(self):
        source = Path("rikugan/ui/welcome_view.py").read_text()
        for gone in ("Rikugan", "welcome_glyph", "welcome_title"):
            self.assertNotIn(gone, source)

    def test_header_carries_no_product_mark(self):
        source = Path("rikugan/ui/panel_header.py").read_text()
        self.assertNotIn("panel_mark", source)


class TestElideTitle(unittest.TestCase):
    def test_short_title_untouched(self):
        self.assertEqual(elide_title("RC4 key schedule"), "RC4 key schedule")

    def test_blank_title_falls_back_to_untitled(self):
        self.assertEqual(elide_title("   "), "Untitled")

    def test_long_title_is_trimmed(self):
        result = elide_title("a" * 40)
        self.assertEqual(len(result), 26)
        self.assertTrue(result.endswith("…"))


class TestNativeMcpControl(unittest.TestCase):
    """The control swaps the agent's tool set, so it has to say so."""

    def test_the_label_names_the_host_not_just_the_protocol(self):
        # A bare hexagon gave no clue what it governed, and "MCP" alone could
        # be any of the servers Rikugan can connect to.
        for active in (True, False):
            with self.subTest(active=active):
                self.assertIn("Binary Ninja MCP", mcp_label(active))

    def test_the_label_shows_which_state_it_is_in(self):
        self.assertNotEqual(mcp_label(True), mcp_label(False))

    def test_the_tooltip_says_what_clicking_does(self):
        self.assertIn("Click", mcp_tooltip(True))
        self.assertIn("Click", mcp_tooltip(False))

    def test_both_states_warn_that_all_rikugan_tools_switch_off(self):
        # The swap costs patching, scripting and /undo tracking, which is
        # worth knowing before flipping it rather than after.
        for active in (True, False):
            with self.subTest(active=active):
                self.assertIn("all of rikugan's tools", mcp_tooltip(active).lower())

    def test_the_off_state_names_what_is_given_up(self):
        tip = mcp_tooltip(False)
        for cost in ("patching", "scripting", "/undo"):
            with self.subTest(cost=cost):
                self.assertIn(cost, tip)


class _FakeToolButton:
    """Enough of QToolButton to exercise the header's state handling."""

    def __init__(self):
        self._text = ""
        self._tip = ""
        self._checked = False
        self._enabled = True

    def setText(self, text):
        self._text = text

    def setToolTip(self, tip):
        self._tip = tip

    def setChecked(self, checked):
        self._checked = checked

    def setEnabled(self, enabled):
        self._enabled = enabled

    def isVisible(self):
        return True


def _header_with_fake_button():
    """A PanelHeader with only the MCP state it needs — Qt is not stubbed deeply."""
    header = PanelHeader.__new__(PanelHeader)
    header._mcp_btn = _FakeToolButton()
    header._mcp_active = False
    header._mcp_callback = None
    return header


class TestNativeMcpButtonState(unittest.TestCase):
    """The control must never claim a switch that has not happened.

    Qt flips a checkable button the instant it is clicked, but whether the
    swap happens depends on a probe that may find no server at all.
    """

    def test_clicking_does_not_switch_the_control_on_by_itself(self):
        header = _header_with_fake_button()
        header._on_mcp()
        self.assertFalse(header._mcp_active)
        self.assertFalse(header._mcp_btn._checked)
        self.assertEqual(header._mcp_btn._text, mcp_label(False))

    def test_the_callback_still_runs_on_click(self):
        header = _header_with_fake_button()
        calls = []
        header._mcp_callback = lambda: calls.append(1)
        header._on_mcp()
        self.assertEqual(calls, [1])

    def test_a_probe_in_flight_reads_as_neither_on_nor_off(self):
        header = _header_with_fake_button()
        header.set_native_mcp_busy(True)
        self.assertFalse(header._mcp_btn._checked)
        self.assertFalse(header._mcp_btn._enabled)
        self.assertNotEqual(header._mcp_btn._text, mcp_label(True))

    def test_a_failed_probe_leaves_the_control_off(self):
        header = _header_with_fake_button()
        header.set_native_mcp_busy(True)
        header.set_native_mcp_busy(False)  # probe came back with nothing
        self.assertFalse(header._mcp_btn._checked)
        self.assertTrue(header._mcp_btn._enabled)
        self.assertEqual(header._mcp_btn._text, mcp_label(False))

    def test_turning_it_off_again_restores_the_off_state(self):
        header = _header_with_fake_button()
        header.set_native_mcp_active(True)
        header._on_mcp()  # click while on: stays on until the controller says
        self.assertTrue(header._mcp_btn._checked)
        header.set_native_mcp_active(False)
        self.assertFalse(header._mcp_btn._checked)


class TestMissingServerMessage(unittest.TestCase):
    def test_it_names_the_menu_path(self):
        # The plugin is off by default and the path is not guessable.
        msg = native_mcp.missing_server_message("http://127.0.0.1:24642/mcp")
        self.assertIn("Plugins", msg)
        self.assertIn("MCP", msg)
        self.assertIn("Start Server", msg)

    def test_it_names_the_address_that_was_tried(self):
        msg = native_mcp.missing_server_message("http://127.0.0.1:9999/mcp")
        self.assertIn("http://127.0.0.1:9999/mcp", msg)


# ---------------------------------------------------------------------------
# responsive layout + chat grouping
# ---------------------------------------------------------------------------


class TestShouldUseDrawer(unittest.TestCase):
    def test_sidebar_width_uses_the_drawer(self):
        from rikugan.ui.panel_core import should_use_drawer

        self.assertTrue(should_use_drawer(420))

    def test_wide_dock_keeps_the_split_column(self):
        from rikugan.ui.panel_core import should_use_drawer

        self.assertFalse(should_use_drawer(900))


def _make_sidebar():
    """Build a ChatThreadList with only the grouping state populated."""
    from rikugan.ui.panel_core import ChatThreadList

    sidebar = object.__new__(ChatThreadList)
    sidebar._group_order = []
    sidebar._group_members = {}
    sidebar._group_items = {}
    sidebar._groups = {}
    sidebar._list = MagicMock()
    return sidebar


class TestSidebarTheme(unittest.TestCase):
    def test_refresh_theme_applies_base_and_drawer_rules_together(self):
        """Qt takes the last setStyleSheet wholesale.

        Applying only the base sheet drops the drawer's opaque background,
        which made the overlay see-through.
        """
        import inspect

        from rikugan.ui.panel_core import ChatThreadList

        source = inspect.getsource(ChatThreadList.refresh_theme)
        self.assertIn("build_chat_sidebar_stylesheet", source)
        self.assertIn("build_chat_drawer_stylesheet", source)

    def test_nothing_else_sets_the_sidebar_stylesheet_directly(self):
        panel_core = Path("rikugan/ui/panel_core.py").read_text()
        self.assertNotIn("_chat_sidebar.setStyleSheet(", panel_core)


class TestChatGrouping(unittest.TestCase):
    def test_first_grouped_chat_lands_after_its_header(self):
        sidebar = _make_sidebar()
        sidebar._insert_group_header = MagicMock()
        row = sidebar._register_in_group("t1", "sample.bndb")
        self.assertEqual(row, 1)
        sidebar._insert_group_header.assert_called_once_with("sample.bndb")

    def test_further_chats_append_within_the_group(self):
        sidebar = _make_sidebar()
        sidebar._insert_group_header = MagicMock()
        sidebar._register_in_group("t1", "sample.bndb")
        self.assertEqual(sidebar._register_in_group("t2", "sample.bndb"), 2)

    def test_ungrouped_chats_need_no_header_row(self):
        sidebar = _make_sidebar()
        sidebar._insert_group_header = MagicMock()
        self.assertEqual(sidebar._register_in_group("t1", ""), 0)
        sidebar._insert_group_header.assert_not_called()

    def test_second_group_starts_after_the_first(self):
        sidebar = _make_sidebar()
        sidebar._insert_group_header = MagicMock()
        sidebar._register_in_group("t1", "a.bndb")
        sidebar._register_in_group("t2", "a.bndb")
        self.assertEqual(sidebar._group_start_row("b.bndb"), 3)

    def test_emptying_a_group_removes_its_header(self):
        sidebar = _make_sidebar()
        sidebar._insert_group_header = MagicMock()
        sidebar._register_in_group("t1", "a.bndb")
        sidebar._group_items["a.bndb"] = MagicMock()
        sidebar._list.row.return_value = 0
        sidebar._unregister_from_group("t1", "a.bndb")
        self.assertEqual(sidebar._group_order, [])
        self.assertNotIn("a.bndb", sidebar._group_items)
        sidebar._list.takeItem.assert_called_once_with(0)

    def test_removing_one_of_two_keeps_the_header(self):
        sidebar = _make_sidebar()
        sidebar._insert_group_header = MagicMock()
        sidebar._register_in_group("t1", "a.bndb")
        sidebar._register_in_group("t2", "a.bndb")
        sidebar._group_items["a.bndb"] = MagicMock()
        sidebar._unregister_from_group("t1", "a.bndb")
        self.assertIn("a.bndb", sidebar._group_items)
        sidebar._list.takeItem.assert_not_called()


if __name__ == "__main__":
    unittest.main()

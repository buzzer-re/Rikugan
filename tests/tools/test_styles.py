"""Tests for host-aware UI style helpers."""

from __future__ import annotations

import re
import sys
import unittest
from contextlib import contextmanager

sys.modules.pop("rikugan.ui.styles", None)

from rikugan.ui.styles import (  # noqa: E402
    IDA_NATIVE_THEME,
    _hex_luminance,
    _normalize_ida_palette,
    build_chat_drawer_stylesheet,
    build_chat_sidebar_stylesheet,
    build_chat_view_stylesheet,
    build_composer_stylesheet,
    build_context_bar_stylesheet,
    build_input_area_stylesheet,
    build_mode_bar_stylesheet,
    build_panel_header_stylesheet,
    build_theme_stylesheet,
    build_welcome_stylesheet,
    PRODUCT_ACCENT,
    PRODUCT_BORDER,
    PRODUCT_PANEL,
    PRODUCT_SURFACE,
    PRODUCT_TEXT,
    get_chat_color_tokens,
    get_effective_palette,
    get_host_font_tokens,
)

_DARK_COLORS = {
    "window": "#242424",
    "window_text": "#e6e6e6",
    "base": "#ffffff",
    "alt_base": "#ffffff",
    "text": "#e6e6e6",
    "button": "#303030",
    "button_text": "#e6e6e6",
    "highlight": "#1678aa",
    "highlight_text": "#ffffff",
    "mid": "#666666",
    "dark": "#181818",
    "light": "#4a4a4a",
}

_LIGHT_COLORS = {
    "window": "#f2f2f2",
    "window_text": "#202020",
    "base": "#ffffff",
    "alt_base": "#eeeeee",
    "text": "#202020",
    "button": "#eeeeee",
    "button_text": "#202020",
    "highlight": "#1476a8",
    "highlight_text": "#ffffff",
    "mid": "#9a9a9a",
    "dark": "#d0d0d0",
    "light": "#ffffff",
}


class TestNormalizeIdaPalette(unittest.TestCase):
    def test_dark_window_with_dark_text_uses_readable_text(self):
        colors = {
            "window": "#303030",
            "window_text": "#000000",
            "base": "#ffffff",
            "alt_base": "#ffffff",
            "text": "#000000",
            "button": "#ffffff",
            "button_text": "#000000",
            "highlight": "#007acc",
            "highlight_text": "#ffffff",
            "mid": "#808080",
            "dark": "#101010",
            "light": "#f3f3f3",
        }

        normalized = _normalize_ida_palette(colors)

        self.assertEqual(normalized["window_text"], "#d4d4d4")
        self.assertEqual(normalized["text"], "#d4d4d4")
        self.assertEqual(normalized["button_text"], "#d4d4d4")
        self.assertNotEqual(normalized["button"], "#ffffff")


class TestInputAreaStylesheet(unittest.TestCase):
    def test_dark_theme_input_surface_is_not_black(self):
        css = build_input_area_stylesheet(_DARK_COLORS)

        self.assertNotIn("background-color: #000000", css)
        self.assertNotIn("color: #000000", css)
        self.assertIn("color:", css)


class TestChatColorTokens(unittest.TestCase):
    def test_dark_window_with_white_base_still_uses_dark_chat_canvas(self):
        tokens = get_chat_color_tokens(_DARK_COLORS)

        self.assertLess(_hex_luminance(tokens["chat_canvas"]), 0.25)
        self.assertLess(_hex_luminance(tokens["assistant_bg"]), 0.35)
        self.assertGreater(_hex_luminance(tokens["text"]), 0.75)

    def test_light_window_uses_light_chat_canvas_with_dark_text(self):
        tokens = get_chat_color_tokens(_LIGHT_COLORS)

        self.assertGreater(_hex_luminance(tokens["chat_canvas"]), 0.80)
        self.assertLess(_hex_luminance(tokens["text"]), 0.25)

    def test_non_ida_chat_view_stylesheet_explicitly_styles_scroll_and_container(self):
        import rikugan.ui.styles as _s

        _orig = _s.use_native_host_theme
        _s.use_native_host_theme = lambda: False
        try:
            css = build_chat_view_stylesheet(_DARK_COLORS)
            self.assertIn("QScrollArea#chat_scroll", css)
            self.assertIn("QWidget#chat_container", css)
            self.assertNotIn("background-color: #ffffff", css)
        finally:
            _s.use_native_host_theme = _orig

    def test_non_ida_chat_view_stylesheet_includes_viewport_selector(self):
        import rikugan.ui.styles as _s

        _orig = _s.use_native_host_theme
        _s.use_native_host_theme = lambda: False
        try:
            css = build_chat_view_stylesheet(_DARK_COLORS)
            self.assertIn("QScrollArea#chat_scroll > QWidget {", css)
        finally:
            _s.use_native_host_theme = _orig

    def test_ida_chat_view_stylesheet_only_fixes_viewport(self):
        import rikugan.ui.styles as _s

        _orig = _s.use_native_host_theme
        _s.use_native_host_theme = lambda: True
        try:
            css = build_chat_view_stylesheet(_DARK_COLORS)
            # Only the viewport rule — no explicit background on scroll area or container
            self.assertIn("QScrollArea#chat_scroll > QWidget", css)
            self.assertIn("background: transparent", css)
            self.assertNotIn("QWidget#chat_container", css)
        finally:
            _s.use_native_host_theme = _orig

    def test_ida_sidebar_stylesheet_is_empty(self):
        import rikugan.ui.styles as _s

        _orig = _s.use_native_host_theme
        _s.use_native_host_theme = lambda: True
        try:
            css = build_chat_sidebar_stylesheet(_DARK_COLORS)
            self.assertEqual(css, "")
        finally:
            _s.use_native_host_theme = _orig

    def test_non_ida_sidebar_row_labels_have_transparent_background(self):
        import rikugan.ui.styles as _s

        _orig = _s.use_native_host_theme
        _s.use_native_host_theme = lambda: False
        try:
            css = build_chat_sidebar_stylesheet(_DARK_COLORS)
            self.assertIn("chat_row_title", css)
            self.assertIn("background: transparent", css)
        finally:
            _s.use_native_host_theme = _orig

    def test_non_ida_sidebar_list_has_no_border(self):
        import rikugan.ui.styles as _s

        _orig = _s.use_native_host_theme
        _s.use_native_host_theme = lambda: False
        try:
            css = build_chat_sidebar_stylesheet(_DARK_COLORS)
            self.assertIn("border: none", css)
        finally:
            _s.use_native_host_theme = _orig


class TestIdaNativeTheme(unittest.TestCase):
    def test_makes_panel_transparent(self):
        self.assertIn("QWidget#rikugan_panel", IDA_NATIVE_THEME)
        self.assertIn("transparent", IDA_NATIVE_THEME)

    def test_chat_scroll_is_transparent(self):
        self.assertIn("QScrollArea#chat_scroll", IDA_NATIVE_THEME)
        self.assertIn("QWidget#chat_container", IDA_NATIVE_THEME)


class TestStylesheetsAreWellFormed(unittest.TestCase):
    """Qt silently drops a whole stylesheet it cannot parse.

    A literal ``}}`` left in a non-f-string continuation once cost the chat
    sidebar all of its styling, so every builder is checked for balance.
    """

    BUILDERS = (
        build_chat_drawer_stylesheet,
        build_chat_sidebar_stylesheet,
        build_chat_view_stylesheet,
        build_composer_stylesheet,
        build_context_bar_stylesheet,
        build_mode_bar_stylesheet,
        build_panel_header_stylesheet,
        build_welcome_stylesheet,
    )

    def test_braces_are_balanced(self):
        for builder in self.BUILDERS:
            with self.subTest(builder=builder.__name__):
                css = builder(_DARK_COLORS)
                self.assertEqual(css.count("{"), css.count("}"), css)
                self.assertNotIn("}}", css)
                self.assertNotIn("{{", css)

    def test_flat_input_area_has_no_border(self):
        css = build_input_area_stylesheet(_DARK_COLORS, flat=True)
        self.assertEqual(css.count("{"), css.count("}"))
        self.assertIn("border: none", css)
        self.assertNotIn("border-radius", css)

    def test_global_theme_is_balanced(self):
        for css in (build_theme_stylesheet(_DARK_COLORS), IDA_NATIVE_THEME):
            self.assertEqual(css.count("{"), css.count("}"))


class TestPanelsFollowTheHostTypeScale(unittest.TestCase):
    """The panel must not invent its own type scale.

    Hard-coded pixel sizes are what made the chat read as a web page dropped
    into the dock next to Binary Ninja's own views.
    """

    def test_every_font_size_is_a_host_derived_token(self):
        fonts = get_host_font_tokens(_DARK_COLORS)
        allowed = {fonts["font_base"], fonts["font_small"]}
        for builder in TestStylesheetsAreWellFormed.BUILDERS:
            css = builder(_DARK_COLORS)
            for value in re.findall(r"font-size:\s*([^;]+);", css):
                with self.subTest(builder=builder.__name__, value=value):
                    self.assertIn(value.strip(), allowed)

    def test_input_editor_uses_the_host_size(self):
        fonts = get_host_font_tokens(_DARK_COLORS)
        for flat in (True, False):
            css = build_input_area_stylesheet(_DARK_COLORS, flat=flat)
            sizes = re.findall(r"font-size:\s*([^;]+);", css)
            self.assertEqual([v.strip() for v in sizes], [fonts["font_base"]])

    def test_monospace_stack_avoids_the_courier_fallback(self):
        for builder in TestStylesheetsAreWellFormed.BUILDERS:
            css = builder(_DARK_COLORS)
            self.assertNotIn("Courier New", css, builder.__name__)

    def test_two_sizes_only(self):
        fonts = get_host_font_tokens(_DARK_COLORS)
        self.assertNotEqual(fonts["font_base"], fonts["font_small"])
        self.assertEqual(len({fonts["font_base"], fonts["font_small"]}), 2)


@contextmanager
def _host_theme(native: bool):
    """Pin the host-theme decision.

    Another test module imports the IDA session controller, which leaves an
    ``idaapi`` stub in ``sys.modules``; ``is_ida()`` then reports True for the
    rest of the run. These tests state which host they mean instead of
    inheriting whatever ran before them.
    """
    import rikugan.ui.styles as _s

    original = _s.use_native_host_theme
    _s.use_native_host_theme = lambda: native
    try:
        yield _s
    finally:
        _s.use_native_host_theme = original


class TestProductTheme(unittest.TestCase):
    """The chat and the Tools panel are two tabs of one dock.

    They must paint the same greys and the same accent, so the chat adopts the
    Tools panel's long-standing colors on any dark host.
    """

    def test_dark_host_gets_the_product_palette(self):
        with _host_theme(native=False) as styles:
            tokens = styles.get_chat_color_tokens(_DARK_COLORS)
        self.assertEqual(tokens["panel"], PRODUCT_PANEL)
        self.assertEqual(tokens["chat_canvas"], PRODUCT_PANEL)
        self.assertEqual(tokens["input_bg"], PRODUCT_SURFACE)
        self.assertEqual(tokens["border"], PRODUCT_BORDER)
        self.assertEqual(tokens["text"], PRODUCT_TEXT)

    def test_dark_host_accent_is_the_product_accent_not_the_host_highlight(self):
        with _host_theme(native=False) as styles:
            tokens = styles.get_chat_color_tokens(_DARK_COLORS)
        self.assertEqual(tokens["accent"], PRODUCT_ACCENT)
        self.assertNotEqual(tokens["accent"], _DARK_COLORS["highlight"])

    def test_light_host_still_adapts(self):
        with _host_theme(native=False) as styles:
            tokens = styles.get_chat_color_tokens(_LIGHT_COLORS)
        self.assertNotEqual(tokens["panel"], PRODUCT_PANEL)
        self.assertEqual(tokens["accent"], _LIGHT_COLORS["highlight"])

    def test_ida_keeps_the_host_palette(self):
        with _host_theme(native=True) as styles:
            tokens = styles.get_chat_color_tokens(_DARK_COLORS)
        self.assertNotEqual(tokens["accent"], PRODUCT_ACCENT)

    def test_chat_list_shares_the_palette(self):
        with _host_theme(native=False) as styles:
            palette = styles.get_effective_palette(_DARK_COLORS)
            css = styles.build_chat_sidebar_stylesheet(_DARK_COLORS)
        self.assertEqual(palette["highlight"], PRODUCT_ACCENT)
        self.assertIn(PRODUCT_PANEL, css)

    def test_panel_surfaces_paint_only_product_colors(self):
        allowed = {
            PRODUCT_PANEL,
            PRODUCT_SURFACE,
            PRODUCT_BORDER,
            PRODUCT_TEXT,
            PRODUCT_ACCENT,
        }
        with _host_theme(native=False) as styles:
            css = styles.build_composer_stylesheet(_DARK_COLORS) + styles.build_panel_header_stylesheet(_DARK_COLORS)
        host_only = {_DARK_COLORS["highlight"], _DARK_COLORS["window"], _DARK_COLORS["text"]}
        for color in host_only - allowed:
            self.assertNotIn(color, css)


if __name__ == "__main__":
    unittest.main()

"""Contrast floors for host-derived colors.

Secondary text is derived by blending the host's text color toward its
background. That keeps a hierarchy on a dark theme and collapses on a light
one: the same blend that reads as dim grey on black is pale grey on white,
which is what made IDA's light theme unreadable.
"""

from __future__ import annotations

import unittest

from rikugan.ui.styles import contrast_ratio, ensure_contrast

# WCAG AA: 4.5 for body text, 3.0 for large or incidental text.
_BODY = 4.5
_LARGE = 3.5

_IDA_LIGHT = {
    "window": "#efefef",
    "window_text": "#000000",
    "base": "#ffffff",
    "alt_base": "#f7f7f7",
    "text": "#000000",
    "button": "#efefef",
    "button_text": "#000000",
    "highlight": "#3874d8",
    "highlight_text": "#ffffff",
    "mid": "#b8b8b8",
    "dark": "#9f9f9f",
    "light": "#ffffff",
}

_IDA_DARK = dict(
    _IDA_LIGHT,
    window="#2b2b2b",
    window_text="#e0e0e0",
    base="#1e1e1e",
    alt_base="#252525",
    text="#e0e0e0",
    button="#2b2b2b",
    button_text="#e0e0e0",
    mid="#555555",
    dark="#1a1a1a",
    light="#3a3a3a",
)


class TestEnsureContrast(unittest.TestCase):
    def test_a_readable_color_is_returned_unchanged(self):
        self.assertEqual(ensure_contrast("#000000", "#ffffff"), "#000000")

    def test_a_washed_out_color_is_darkened_on_a_light_ground(self):
        fixed = ensure_contrast("#bbbbbb", "#ffffff")
        self.assertGreaterEqual(contrast_ratio(fixed, "#ffffff"), _BODY)

    def test_a_dim_color_is_lightened_on_a_dark_ground(self):
        fixed = ensure_contrast("#3a3a3a", "#1e1e1e")
        self.assertGreaterEqual(contrast_ratio(fixed, "#1e1e1e"), _BODY)

    def test_it_stops_at_the_requested_threshold(self):
        # Walking all the way to black would flatten the palette's hues.
        fixed = ensure_contrast("#569cd6", "#ffffff", _LARGE)
        self.assertGreaterEqual(contrast_ratio(fixed, "#ffffff"), _LARGE)
        self.assertNotEqual(fixed, "#000000")

    def test_contrast_ratio_is_symmetric(self):
        self.assertAlmostEqual(contrast_ratio("#000000", "#ffffff"), contrast_ratio("#ffffff", "#000000"))
        self.assertAlmostEqual(contrast_ratio("#777777", "#777777"), 1.0)


class TestHostTokensStayReadable(unittest.TestCase):
    """The token tiers must clear their floor on either host theme."""

    def _tokens(self, palette):
        from rikugan.ui.styles import _extended_tokens

        return _extended_tokens(palette)

    def test_light_theme_text_tiers(self):
        self._assert_tiers(_IDA_LIGHT)

    def test_dark_theme_text_tiers(self):
        self._assert_tiers(_IDA_DARK)

    def _assert_tiers(self, palette):
        t = self._tokens(palette)
        canvas = t["chat_canvas"]
        for tier, floor in (("muted", _BODY), ("subtle", _BODY), ("faint", _LARGE)):
            with self.subTest(tier=tier):
                self.assertGreaterEqual(
                    contrast_ratio(t[tier], canvas),
                    floor,
                    f"{tier}={t[tier]} on {canvas}",
                )

    def test_the_send_arrow_is_readable_on_its_own_fill(self):
        # It used to be the accent painted on 70% of that same accent, which
        # all but vanished on a light host.
        for palette in (_IDA_LIGHT, _IDA_DARK):
            t = self._tokens(palette)
            glyph = ensure_contrast(t["accent_text"], t["accent"])
            with self.subTest(window=palette["window"]):
                self.assertGreaterEqual(contrast_ratio(glyph, t["accent"]), _LARGE)


if __name__ == "__main__":
    unittest.main()

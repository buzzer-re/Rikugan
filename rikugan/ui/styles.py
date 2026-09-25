"""Theme helpers and dark-theme stylesheet for Rikugan UI."""

from __future__ import annotations

from ..core.host import is_ida

_FALLBACK_COLORS = {
    "window": "#1e1e1e",
    "window_text": "#d4d4d4",
    "base": "#252526",
    "alt_base": "#2d2d2d",
    "text": "#d4d4d4",
    "button": "#2d2d2d",
    "button_text": "#d4d4d4",
    "highlight": "#569cd6",
    "highlight_text": "#ffffff",
    "mid": "#808080",
    "dark": "#1a1a1a",
    "light": "#f3f3f3",
}

# Warm tint targets for the assistant bubble (and its code blocks). The bubble
# blends the host-derived surface toward one of these so it reads as a warm
# taupe instead of a flat cold grey, while still adapting to the host palette.
_WARM_CREAM = "#ffecd2"  # lighten + warm dark surfaces
_WARM_BROWN = "#4a3c28"  # darken + warm light surfaces


# Rikugan's own dark theme. The Tools panel has always painted these exact
# values, so the chat uses them too rather than deriving its own greys from the
# host palette — the two tabs of one dock must not read as two products.
# Light host themes still fall through to the palette-derived path below.
PRODUCT_PANEL = "#1e1e1e"
PRODUCT_SURFACE = "#2d2d2d"
PRODUCT_SURFACE_ALT = "#252526"
PRODUCT_SURFACE_HI = "#3c3c3c"
PRODUCT_BORDER = "#3c3c3c"
PRODUCT_BORDER_SOFT = "#333333"
PRODUCT_TEXT = "#d4d4d4"
PRODUCT_SUBTLE = "#a8a8a8"
PRODUCT_MUTED = "#808080"
PRODUCT_FAINT = "#6a6a6a"
PRODUCT_ACCENT = "#4ec9b0"

_PRODUCT_PALETTE: dict[str, str] = {
    "window": PRODUCT_PANEL,
    "window_text": PRODUCT_TEXT,
    "base": PRODUCT_SURFACE_ALT,
    "alt_base": PRODUCT_SURFACE,
    "text": PRODUCT_TEXT,
    "button": PRODUCT_SURFACE,
    "button_text": PRODUCT_TEXT,
    "highlight": PRODUCT_ACCENT,
    "highlight_text": PRODUCT_PANEL,
    "mid": PRODUCT_BORDER,
    "dark": "#141414",
    "light": PRODUCT_SURFACE_HI,
}


def _hex_luminance(color: str) -> float:
    color = color.lstrip("#")
    if len(color) != 6:
        return 0.0
    r = int(color[0:2], 16) / 255.0
    g = int(color[2:4], 16) / 255.0
    b = int(color[4:6], 16) / 255.0
    return 0.2126 * r + 0.7152 * g + 0.0722 * b


def _blend_channel(a: int, b: int, amount: float) -> int:
    amount = max(0.0, min(1.0, amount))
    return round(a + (b - a) * amount)


def blend_theme_color(color_a: str, color_b: str, amount: float) -> str:
    """Blend two ``#rrggbb`` colors."""
    a = color_a.lstrip("#")
    b = color_b.lstrip("#")
    if len(a) != 6 or len(b) != 6:
        return color_a

    ar, ag, ab = int(a[0:2], 16), int(a[2:4], 16), int(a[4:6], 16)
    br, bg, bb = int(b[0:2], 16), int(b[2:4], 16), int(b[4:6], 16)
    return (
        f"#{_blend_channel(ar, br, amount):02x}{_blend_channel(ag, bg, amount):02x}{_blend_channel(ab, bb, amount):02x}"
    )


def _normalize_ida_palette(colors: dict[str, str]) -> dict[str, str]:
    """Derive control surfaces from IDA's dock background instead of native widget roles."""
    window = colors["window"]
    window_text = colors["window_text"]
    is_dark = _hex_luminance(window) < 0.5
    if is_dark and _hex_luminance(window_text) < 0.55:
        window_text = _FALLBACK_COLORS["window_text"]
    elif not is_dark and _hex_luminance(window_text) > 0.55:
        window_text = "#1f1f1f"
    toward = "#ffffff" if is_dark else "#000000"
    surface = blend_theme_color(window, toward, 0.08 if is_dark else 0.035)
    alt_surface = blend_theme_color(window, toward, 0.14 if is_dark else 0.07)
    mid = blend_theme_color(window, toward, 0.22 if is_dark else 0.16)
    colors = dict(colors)
    colors["window_text"] = window_text
    colors["base"] = surface
    colors["button"] = surface
    colors["alt_base"] = alt_surface
    colors["text"] = window_text
    colors["button_text"] = window_text
    colors["mid"] = mid
    colors["dark"] = blend_theme_color(window, "#000000", 0.20 if is_dark else 0.08)
    colors["light"] = blend_theme_color(window, "#ffffff", 0.14 if is_dark else 0.20)
    return colors


def _palette_colors(palette, qpalette) -> dict[str, str]:
    return {
        "window": palette.color(_palette_role(qpalette, "Window")).name(),
        "window_text": palette.color(_palette_role(qpalette, "WindowText")).name(),
        "base": palette.color(_palette_role(qpalette, "Base")).name(),
        "alt_base": palette.color(_palette_role(qpalette, "AlternateBase")).name(),
        "text": palette.color(_palette_role(qpalette, "Text")).name(),
        "button": palette.color(_palette_role(qpalette, "Button")).name(),
        "button_text": palette.color(_palette_role(qpalette, "ButtonText")).name(),
        "highlight": palette.color(_palette_role(qpalette, "Highlight")).name(),
        "highlight_text": palette.color(_palette_role(qpalette, "HighlightedText")).name(),
        "mid": palette.color(_palette_role(qpalette, "Mid")).name(),
        "dark": palette.color(_palette_role(qpalette, "Dark")).name(),
        "light": palette.color(_palette_role(qpalette, "Light")).name(),
    }


def _palette_role(qpalette, role_name: str):
    """Return a Qt palette role compatible with Qt5/Qt6 enum layouts."""
    role = getattr(qpalette, role_name, None)
    if role is not None:
        return role
    color_role = getattr(qpalette, "ColorRole", None)
    if color_role is not None:
        return getattr(color_role, role_name)
    raise AttributeError(role_name)


def get_host_palette_colors(source=None) -> dict[str, str]:
    """Return the current Qt palette colors, with stable fallbacks."""
    try:
        from .qt_compat import QApplication, QPalette
    except ImportError:
        return dict(_FALLBACK_COLORS)

    try:
        palette = None
        if source is not None and hasattr(source, "palette"):
            palette = source.palette()
        if palette is None:
            instance = getattr(QApplication, "instance", None)
            app = instance() if callable(instance) else None
            if app is None or not hasattr(app, "palette"):
                return dict(_FALLBACK_COLORS)
            palette = app.palette()
        colors = _palette_colors(palette, QPalette)
        if use_native_host_theme():
            colors = _normalize_ida_palette(colors)
        return colors
    except Exception:
        return dict(_FALLBACK_COLORS)


def _use_product_theme(colors: dict[str, str]) -> bool:
    """Whether to paint Rikugan's own dark theme instead of deriving one.

    IDA owns its dock, and a light host theme needs palette-derived colors to
    stay readable; everywhere else the product theme wins so the chat matches
    the Tools panel exactly.
    """
    return not use_native_host_theme() and _hex_luminance(colors["window"]) < 0.5


def _product_chat_tokens() -> dict[str, str]:
    """Chat tokens for the product theme, in the Tools panel's own colors."""
    assistant_bg = blend_theme_color(PRODUCT_PANEL, _WARM_CREAM, 0.14)
    return {
        "panel": PRODUCT_PANEL,
        "chat_canvas": PRODUCT_PANEL,
        "assistant_bg": assistant_bg,
        "assistant_border": blend_theme_color(assistant_bg, _WARM_CREAM, 0.10),
        "tool_bg": PRODUCT_SURFACE_ALT,
        "thinking_bg": PRODUCT_SURFACE_ALT,
        "input_bg": PRODUCT_SURFACE,
        "text": PRODUCT_TEXT,
        "muted": PRODUCT_MUTED,
        "subtle": PRODUCT_SUBTLE,
        "border": PRODUCT_BORDER,
        "accent": PRODUCT_ACCENT,
        "accent_text": PRODUCT_PANEL,
        "code_bg": blend_theme_color(assistant_bg, _WARM_CREAM, 0.10),
        # Extras the redesigned surfaces need, pinned to the same palette.
        "surface": PRODUCT_SURFACE,
        "surface_hi": PRODUCT_SURFACE_HI,
        "border_soft": PRODUCT_BORDER_SOFT,
        "faint": PRODUCT_FAINT,
        "accent_soft": blend_theme_color(PRODUCT_ACCENT, PRODUCT_PANEL, 0.62),
        "accent_deep": blend_theme_color(PRODUCT_ACCENT, PRODUCT_PANEL, 0.30),
    }


def get_effective_palette(source=None) -> dict[str, str]:
    """Return the palette the panel paints with: product theme or host."""
    if isinstance(source, dict) and "chat_canvas" not in source:
        colors = _normalize_ida_palette(source)
    elif isinstance(source, dict):
        colors = dict(_PRODUCT_PALETTE)
    else:
        colors = get_host_palette_colors(source)
    if _use_product_theme(colors):
        return dict(_PRODUCT_PALETTE)
    return colors


def get_chat_color_tokens(source=None) -> dict[str, str]:
    """Return semantic colors for chat surfaces.

    Dark hosts get Rikugan's own theme (the Tools panel's colors); light hosts
    and IDA keep colors derived from the live host palette.
    """
    if isinstance(source, dict):
        if "chat_canvas" in source:
            return source
        colors = _normalize_ida_palette(source)
    else:
        colors = get_host_palette_colors(source)
    if _use_product_theme(colors):
        return _product_chat_tokens()
    panel = colors["window"]
    text = colors["window_text"]
    is_dark = _hex_luminance(panel) < 0.5
    toward = "#ffffff" if is_dark else "#000000"

    chat_canvas = blend_theme_color(panel, toward, 0.04 if is_dark else 0.018)
    # The assistant bubble (and its code blocks) warm toward a cream/brown so it
    # reads as a soft warm taupe rather than a flat cold grey. Other surfaces
    # keep the neutral blend toward white/black.
    warm = _WARM_CREAM if is_dark else _WARM_BROWN
    assistant_bg = blend_theme_color(chat_canvas, warm, 0.14 if is_dark else 0.06)
    assistant_border = blend_theme_color(assistant_bg, warm, 0.10 if is_dark else 0.08)
    tool_bg = blend_theme_color(chat_canvas, toward, 0.06 if is_dark else 0.028)
    thinking_bg = blend_theme_color(chat_canvas, toward, 0.10 if is_dark else 0.05)
    input_bg = blend_theme_color(chat_canvas, toward, 0.12 if is_dark else 0.045)
    border = blend_theme_color(colors["mid"], panel, 0.35)
    muted = blend_theme_color(text, panel, 0.38)
    subtle = blend_theme_color(text, panel, 0.22)
    code_bg = blend_theme_color(assistant_bg, warm, 0.10 if is_dark else 0.05)

    return {
        "panel": panel,
        "chat_canvas": chat_canvas,
        "assistant_bg": assistant_bg,
        "assistant_border": assistant_border,
        "tool_bg": tool_bg,
        "thinking_bg": thinking_bg,
        "input_bg": input_bg,
        "text": text,
        "muted": muted,
        "subtle": subtle,
        "border": border,
        "accent": colors["highlight"],
        "accent_text": colors["highlight_text"],
        "code_bg": code_bg,
    }


def use_native_host_theme() -> bool:
    """Return True when the host should keep its own native theme.

    IDA owns the overall dock styling, but Rikugan still derives local
    widget surfaces from the live host colors so the chat remains readable
    in both light and dark themes.
    """
    return is_ida()


def maybe_host_stylesheet(css: str) -> str:
    """Return the stylesheet unless the host should keep its native theme."""
    return "" if use_native_host_theme() else css


def host_stylesheet(custom_css: str, native_css: str = "") -> str:
    """Return the stylesheet for the active host theme mode."""
    return native_css if use_native_host_theme() else custom_css


# Stop / error accents are the one hue that cannot be derived from the host
# palette — a host has no "danger" role — so the base tone is fixed here and
# resolved against whatever surface it lands on.
DANGER_RED = "#e05252"


# Qt resolves the first family that exists; Menlo covers macOS, DejaVu Sans Mono
# Linux, and Consolas Windows. Courier New is deliberately absent — it is what
# the old "Consolas, Courier New" stack fell back to on macOS.
MONO_FONT_STACK = 'Menlo, "SF Mono", Consolas, "DejaVu Sans Mono", monospace'

# Only used when the host cannot be asked for its font (headless tests).
_FALLBACK_FONT_PT = 9.0


def _host_font_size(source=None) -> tuple[float, str]:
    """Return the host UI font's size as ``(value, css_unit)``."""
    font = None
    get_font = getattr(source, "font", None)
    if callable(get_font):
        try:
            font = get_font()
        except Exception:
            font = None
    if font is None:
        try:
            from .qt_compat import QApplication

            instance = getattr(QApplication, "instance", None)
            app = instance() if callable(instance) else None
            font = app.font() if app is not None and hasattr(app, "font") else None
        except Exception:
            font = None
    if font is not None:
        try:
            points = float(font.pointSizeF())
            if points > 0:
                return points, "pt"
            pixels = float(font.pixelSize())
            if pixels > 0:
                return pixels, "px"
        except (AttributeError, TypeError, ValueError):
            pass
    return _FALLBACK_FONT_PT, "pt"


def get_host_font_tokens(source=None) -> dict[str, str]:
    """Return the two type sizes these panels use, matched to the host font.

    Two sizes only: body text at the host's own size and one step down for
    secondary text. Fixed pixel sizes are what made the panel look foreign
    next to native views on a HiDPI Mac.
    """
    size, unit = _host_font_size(source)
    return {
        "font_base": f"{size:g}{unit}",
        "font_small": f"{max(size - 1.0, 1.0):g}{unit}",
        "mono": MONO_FONT_STACK,
    }


def build_theme_stylesheet(source=None) -> str:
    """Return the active panel stylesheet for the current host."""
    if use_native_host_theme():
        return IDA_NATIVE_THEME
    return DARK_THEME


def build_chat_sidebar_stylesheet(source=None) -> str:
    """Return the chat list stylesheet, in the same tokens as every other surface.

    It used to blend its own greys out of the raw host palette, which left the
    list a slightly different shade from the chat beside it.
    """
    t = _extended_tokens(source)
    css = (
        f"QWidget#chat_sidebar {{ background-color: {t['panel']}; color: {t['text']}; }}"
        f"QLabel#chat_sidebar_title {{ color: {t['text']}; font-size: {t['font_base']}; }}"
        f"QLabel#chat_row_title {{ color: {t['text']}; font-size: {t['font_base']}; background: transparent; }}"
        f"QLabel#chat_row_detail {{ color: {t['muted']}; font-size: {t['font_small']}; "
        "background: transparent; }"
        f"QLineEdit#chat_search {{ background-color: {t['input_bg']}; color: {t['text']}; "
        f"border: 1px solid {t['border']}; border-radius: 3px; padding: 4px 6px; "
        f"font-size: {t['font_base']}; }}"
        f"QListWidget#chat_thread_list {{ border: none; background-color: {t['panel']}; outline: none; }}"
        "QListWidget#chat_thread_list::item { border: none; padding: 0px; }"
        f"QListWidget#chat_thread_list::item:hover {{ background-color: {t['surface']}; }}"
        # A tinted surface, not an accent fill: the row's muted detail line was
        # unreadable on saturated teal.
        f"QListWidget#chat_thread_list::item:selected {{ background-color: {t['surface_hi']}; "
        f"border-left: 2px solid {t['accent']}; }}"
    )
    # In IDA let the dock's native Qt theme color all containers; only
    # Binary Ninja / standalone mode needs explicit palette-derived colors.
    return maybe_host_stylesheet(css)


def build_chat_view_stylesheet(source=None) -> str:
    """Return palette-aware stylesheet for the scrollable chat viewport."""
    tokens = get_chat_color_tokens(source)
    canvas = tokens["chat_canvas"]
    if use_native_host_theme():
        # IDA applies its dock theme natively; any explicit background-color
        # here overrides IDA's own colors and looks wrong. The one exception is
        # the QScrollArea viewport (unnamed direct QWidget child) which defaults
        # to Base=white in IDA. Making it transparent lets the dock background
        # show instead, matching the rest of the panel.
        return "QScrollArea#chat_scroll > QWidget { background: transparent; }"
    return (
        f"QScrollArea#chat_scroll {{ border: none; background-color: {canvas}; }}"
        f"QScrollArea#chat_scroll > QWidget {{ background-color: {canvas}; }}"
        f"QWidget#chat_container {{ background-color: {canvas}; color: {tokens['text']}; }}"
    )


def build_input_area_stylesheet(source=None, flat: bool = False) -> str:
    """Return a palette-aware input editor stylesheet.

    ``flat`` drops the border and background so the editor can sit inside the
    composer frame, which draws both itself.
    """
    tokens = get_chat_color_tokens(source)
    if flat:
        return (
            "QPlainTextEdit#input_area { "
            f"background: transparent; color: {tokens['text']}; "
            f"border: none; padding: 2px; font-size: {get_host_font_tokens(source)['font_base']}; "
            f"selection-background-color: {tokens['accent']}; "
            f"selection-color: {tokens['accent_text']}; }}"
            f"QPlainTextEdit#input_area:disabled {{ color: {tokens['muted']}; }}"
        )
    return (
        "QPlainTextEdit#input_area { "
        f"background-color: {tokens['input_bg']}; color: {tokens['text']}; "
        f"border: 1px solid {tokens['border']}; border-radius: 8px; "
        f"padding: 8px; font-size: {get_host_font_tokens(source)['font_base']}; "
        f"selection-background-color: {tokens['accent']}; "
        f"selection-color: {tokens['accent_text']}; }}"
        f"QPlainTextEdit#input_area:disabled {{ color: {tokens['muted']}; }}"
        f"QPlainTextEdit#input_area:focus {{ border-color: {tokens['accent']}; }}"
    )


IDA_NATIVE_THEME = """
QWidget#rikugan_panel {
    background: transparent;
}

QScrollArea#chat_scroll {
    border: none;
    background: transparent;
}

QWidget#chat_container {
    background: transparent;
}

QToolButton#collapse_button {
    border: none;
    background: transparent;
    padding: 0px;
}

QLabel#tool_content {
    font-family: Consolas, "Courier New", monospace;
    font-size: 10px;
}
"""


DARK_THEME = """
QWidget#rikugan_panel {
    background-color: #1e1e1e;
    color: #d4d4d4;
}

QScrollArea#chat_scroll {
    background-color: #1e1e1e;
    border: none;
}

QWidget#chat_container {
    background-color: #1e1e1e;
}

/* Message frames (#message_user, #message_assistant, #message_tool,
   #message_thinking, #message_question, #message_notice) are styled by the
   widgets themselves from the shared tokens. Repeating them here made a second,
   competing color source that fought the tokens in every non-IDA host. */

QLabel#tool_content {
    font-family: monospace;
}

QPlainTextEdit#input_area {
    background-color: #2d2d2d;
    color: #d4d4d4;
    border: 1px solid #3c3c3c;
    border-radius: 8px;
    padding: 8px;
    font-size: 13px;
    selection-background-color: #264f78;
}

QPlainTextEdit#input_area:focus {
    border-color: #007acc;
}

QPushButton#send_button {
    background-color: #007acc;
    color: white;
    border: none;
    border-radius: 6px;
    padding: 6px 16px;
    font-weight: bold;
}

QPushButton#send_button:hover {
    background-color: #1a8ad4;
}

QPushButton#send_button:pressed {
    background-color: #005a9e;
}

QPushButton#send_button:disabled {
    background-color: #3c3c3c;
    color: #6c6c6c;
}

QPushButton#cancel_button {
    background-color: #c72e2e;
    color: white;
    border: none;
    border-radius: 6px;
    padding: 6px 16px;
    font-weight: bold;
}

QFrame#context_bar {
    background-color: #252526;
    border-top: 1px solid #3c3c3c;
    padding: 4px 8px;
}

QLabel#context_label {
    color: #808080;
    font-size: 11px;
}

QLabel#context_value {
    color: #cccccc;
    font-size: 11px;
}

QFrame#plan_step {
    background-color: #252526;
    border: 1px solid #3c3c3c;
    border-radius: 4px;
    padding: 4px 8px;
    margin: 2px;
}

QFrame#plan_step_active {
    background-color: #252526;
    border: 1px solid #007acc;
    border-radius: 4px;
    padding: 4px 8px;
    margin: 2px;
}

QFrame#plan_step_done {
    background-color: #252526;
    border: 1px solid #4ec9b0;
    border-radius: 4px;
    padding: 4px 8px;
    margin: 2px;
}

QToolButton#collapse_button {
    border: none;
    color: #808080;
    font-size: 10px;
}

QToolButton#collapse_button:hover {
    color: #d4d4d4;
}

QComboBox, QLineEdit, QSpinBox, QDoubleSpinBox {
    background-color: #2d2d2d;
    color: #d4d4d4;
    border: 1px solid #3c3c3c;
    border-radius: 4px;
    padding: 4px;
}

QGroupBox {
    color: #d4d4d4;
    border: 1px solid #3c3c3c;
    border-radius: 4px;
    margin-top: 8px;
    padding-top: 16px;
}

QGroupBox::title {
    subcontrol-origin: margin;
    left: 8px;
    padding: 0 4px;
}

QFrame#tools_panel {
    background-color: #1e1e1e;
    border-left: 1px solid #3c3c3c;
}

QFrame#tools_panel QTabWidget::pane {
    border: none;
}

QFrame#tools_panel QTabBar {
    background: #1e1e1e;
    border: none;
}

QFrame#tools_panel QTabBar::tab {
    background: #252526;
    color: #cccccc;
    padding: 4px 12px;
    border: none;
    border-right: 1px solid #3c3c3c;
    font-size: 11px;
}

QFrame#tools_panel QTabBar::tab:selected {
    background: #1e1e1e;
    color: #ffffff;
}

QFrame#tools_panel QTabBar::tab:hover {
    background: #2d2d2d;
}

QTreeWidget {
    background-color: #1e1e1e;
    color: #d4d4d4;
    border: none;
    font-size: 11px;
}

QTreeWidget::item {
    padding: 2px 4px;
}

QTreeWidget::item:selected {
    background-color: #264f78;
}

QTreeWidget::item:hover {
    background-color: #2d2d2d;
}

QHeaderView::section {
    background-color: #252526;
    color: #cccccc;
    border: none;
    border-right: 1px solid #3c3c3c;
    padding: 3px 6px;
    font-size: 11px;
}

QTableWidget {
    background-color: #1e1e1e;
    color: #d4d4d4;
    border: none;
    gridline-color: #3c3c3c;
    font-size: 11px;
}

QTableWidget::item {
    padding: 2px 4px;
}

QTableWidget::item:selected {
    background-color: #264f78;
}

QProgressBar {
    background-color: #2d2d2d;
    border: 1px solid #3c3c3c;
    border-radius: 3px;
    text-align: center;
    color: #d4d4d4;
    font-size: 10px;
    height: 14px;
}

QProgressBar::chunk {
    background-color: #4ec9b0;
    border-radius: 2px;
}

QRadioButton {
    color: #d4d4d4;
    font-size: 11px;
    spacing: 4px;
}

QTextEdit {
    background-color: #1e1e1e;
    color: #d4d4d4;
    border: 1px solid #3c3c3c;
    border-radius: 4px;
    font-size: 11px;
}
"""


# ---------------------------------------------------------------------------
# Redesigned chat surfaces (header, drawer, welcome, composer, context bar)
#
# Colors come from ``get_chat_color_tokens`` and type comes from the host's own
# UI font, so these panels read as part of IDA / Binary Ninja rather than as a
# web page pasted into the dock. Only the outermost container background is
# dropped in IDA so the dock's own color shows through.
# ---------------------------------------------------------------------------


def _srgb_luminance(color: str) -> float:
    """Relative luminance per WCAG, which weights the channels by eye response."""
    h = color.lstrip("#")
    if len(h) != 6:
        return 0.5
    out = []
    for i in (0, 2, 4):
        c = int(h[i : i + 2], 16) / 255.0
        out.append(c / 12.92 if c <= 0.03928 else ((c + 0.055) / 1.055) ** 2.4)
    return 0.2126 * out[0] + 0.7152 * out[1] + 0.0722 * out[2]


def contrast_ratio(fg: str, bg: str) -> float:
    """WCAG contrast ratio between two colors, from 1.0 (equal) to 21.0."""
    a, b = _srgb_luminance(fg), _srgb_luminance(bg)
    hi, lo = max(a, b), min(a, b)
    return (hi + 0.05) / (lo + 0.05)


def ensure_contrast(fg: str, bg: str, minimum: float = 4.5) -> str:
    """Push *fg* away from *bg* until it is readable on it.

    Secondary text is derived by blending the host's text color toward its
    background, which keeps a hierarchy on a dark theme but collapses on a
    light one: the same blend that reads as dim grey on black becomes pale
    grey on white. Rather than pick per-theme constants, walk the color back
    toward black or white until it clears the threshold.
    """
    if contrast_ratio(fg, bg) >= minimum:
        return fg
    target = "#000000" if _srgb_luminance(bg) > 0.5 else "#ffffff"
    candidate = fg
    for step in range(1, 21):
        candidate = blend_theme_color(fg, target, step / 20.0)
        if contrast_ratio(candidate, bg) >= minimum:
            return candidate
    return target


def _extended_tokens(source=None) -> dict[str, str]:
    """Chat tokens plus the extra shades and type sizes these surfaces need."""
    tokens = dict(get_chat_color_tokens(source))
    if "surface" not in tokens:
        # Host-derived path: the product theme already carries these pinned.
        panel = tokens["panel"]
        text = tokens["text"]
        is_dark = _hex_luminance(panel) < 0.5
        toward = "#ffffff" if is_dark else "#000000"
        tokens["faint"] = blend_theme_color(text, panel, 0.58)
        tokens["surface"] = blend_theme_color(tokens["chat_canvas"], toward, 0.06 if is_dark else 0.03)
        tokens["surface_hi"] = blend_theme_color(tokens["chat_canvas"], toward, 0.11 if is_dark else 0.055)
        tokens["border_soft"] = blend_theme_color(tokens["border"], panel, 0.45)
        tokens["accent_soft"] = blend_theme_color(tokens["accent"], panel, 0.62)
        tokens["accent_deep"] = blend_theme_color(tokens["accent"], panel, 0.30)
    # A light host collapses the blended shades: enforce a readable floor
    # rather than trusting the blend. 4.5 is the WCAG threshold for body text;
    # the faintest tier is held to the large-text threshold since it only ever
    # carries short labels.
    canvas = tokens["chat_canvas"]
    tokens["muted"] = ensure_contrast(tokens["muted"], canvas, 4.5)
    tokens["subtle"] = ensure_contrast(tokens["subtle"], canvas, 4.5)
    tokens["faint"] = ensure_contrast(tokens["faint"], canvas, 3.5)
    tokens.update(get_host_font_tokens(source))
    return tokens


def _container_bg(color: str) -> str:
    """Container background: transparent in IDA, palette-derived elsewhere."""
    return "transparent" if use_native_host_theme() else color


def build_mode_bar_stylesheet(source=None) -> str:
    """Return the Chat / Tools switcher stylesheet at the host's type size."""
    t = _extended_tokens(source)
    return (
        f"QTabBar#mode_bar {{ background-color: {_container_bg(t['panel'])}; border: none; "
        f"border-bottom: 1px solid {t['border_soft']}; }}"
        f"QTabBar#mode_bar::tab {{ background: transparent; color: {t['muted']}; padding: 5px 14px; "
        f"border: none; border-bottom: 2px solid transparent; font-size: {t['font_base']}; }}"
        f"QTabBar#mode_bar::tab:selected {{ color: {t['text']}; border-bottom: 2px solid {t['accent']}; }}"
        f"QTabBar#mode_bar::tab:hover:!selected {{ color: {t['text']}; }}"
    )


def build_panel_header_stylesheet(source=None) -> str:
    """Return the stylesheet for the chat header (switcher and icon buttons)."""
    t = _extended_tokens(source)
    return (
        f"QWidget#panel_header {{ background-color: {_container_bg(t['panel'])}; "
        f"border-bottom: 1px solid {t['border_soft']}; }}"
        f"QToolButton#chat_switcher {{ background: transparent; color: {t['text']}; "
        "border: 1px solid transparent; border-radius: 4px; padding: 3px 8px; "
        f"font-size: {t['font_base']}; text-align: left; }}"
        f"QToolButton#chat_switcher:hover {{ background-color: {t['surface']}; border-color: {t['border']}; }}"
        f"QToolButton#header_icon {{ background: transparent; color: {t['subtle']}; "
        f"border: 1px solid transparent; border-radius: 4px; font-size: {t['font_base']}; }}"
        f"QToolButton#header_icon:hover {{ background-color: {t['surface']}; "
        f"border-color: {t['border']}; color: {t['text']}; }}"
        # The MCP control carries a word, not a glyph: nothing about a shape
        # says which tools it governs, and it changes what the agent can do.
        f"QToolButton#mcp_toggle {{ background-color: {t['surface']}; "
        f"color: {ensure_contrast(t['muted'], t['surface'])}; "
        f"border: 1px solid {t['border']}; border-radius: 9px; padding: 2px 8px; "
        f"font-size: {t['font_small']}; }}"
        f"QToolButton#mcp_toggle:hover {{ background-color: {t['surface_hi']}; color: {t['text']}; }}"
        f"QToolButton#mcp_toggle:checked {{ background-color: {t['accent_soft']}; "
        f"color: {t['text']}; border-color: {t['accent']}; }}"
    )


def build_chat_drawer_stylesheet(source=None) -> str:
    """Return the stylesheet for the overlay chat drawer and its scrim."""
    t = _extended_tokens(source)
    return (
        "QWidget#chat_drawer_scrim { background-color: rgba(0, 0, 0, 110); }"
        # As an overlay the drawer must be opaque in every host, so this rule
        # deliberately sets a background even where IDA owns the dock theme.
        f'QWidget#chat_sidebar[drawer="true"] {{ background-color: {t["panel"]}; '
        f"border-right: 1px solid {t['border']}; }}"
        f"QLabel#chat_group_header {{ color: {t['faint']}; font-size: {t['font_small']}; "
        "background: transparent; padding: 6px 4px 3px 4px; }"
        f"QWidget#chat_sidebar_footer {{ border-top: 1px solid {t['border_soft']}; }}"
        f"QToolButton#drawer_chip {{ background-color: {t['surface']}; "
        f"color: {ensure_contrast(t['muted'], t['surface'])}; "
        f"border: 1px solid {t['border']}; border-radius: 4px; padding: 4px 8px; "
        f"font-size: {t['font_small']}; }}"
        f"QToolButton#drawer_chip:hover {{ background-color: {t['surface_hi']}; color: {t['text']}; }}"
    )


def build_welcome_stylesheet(source=None) -> str:
    """Return the stylesheet for the empty-chat welcome screen."""
    t = _extended_tokens(source)
    return (
        f"QWidget#welcome_view {{ background-color: {_container_bg(t['chat_canvas'])}; }}"
        f"QLabel#welcome_subtitle {{ color: {t['muted']}; font-size: {t['font_base']}; "
        "background: transparent; }"
        f"QFrame#binary_card {{ background-color: {t['surface']}; border: 1px solid {t['border_soft']}; "
        "border-radius: 4px; }"
        f"QLabel#binary_name {{ color: {t['text']}; font-size: {t['font_base']}; background: transparent; "
        f"font-family: {t['mono']}; }}"
        f"QLabel#binary_chip {{ color: {t['muted']}; font-size: {t['font_small']}; "
        f"background-color: {t['chat_canvas']}; border: 1px solid {t['border_soft']}; "
        f"border-radius: 3px; padding: 2px 6px; font-family: {t['mono']}; }}"
        f"QLabel#welcome_section {{ color: {t['faint']}; font-size: {t['font_small']}; "
        "background: transparent; }"
        f"QFrame#suggestion_row {{ background: transparent; border: 1px solid {t['border']}; "
        "border-radius: 4px; }"
        f"QFrame#suggestion_row:hover {{ background-color: {t['surface']}; }}"
        f"QLabel#suggestion_icon {{ color: {t['muted']}; font-size: {t['font_base']}; "
        "background: transparent; }"
        f"QLabel#suggestion_title {{ color: {t['text']}; font-size: {t['font_base']}; "
        "background: transparent; }"
        f"QLabel#suggestion_command {{ color: {t['faint']}; font-size: {t['font_small']}; "
        f"background: transparent; font-family: {t['mono']}; }}"
        f"QLabel#suggestion_chevron {{ color: {t['border']}; font-size: {t['font_base']}; "
        "background: transparent; }"
        f"QLabel#welcome_hint {{ color: {t['faint']}; font-size: {t['font_small']}; "
        "background: transparent; }"
    )


def build_composer_stylesheet(source=None) -> str:
    """Return the stylesheet for the composer frame and its control row."""
    t = _extended_tokens(source)
    send_glyph = ensure_contrast(t["accent_text"], t["accent"])
    stop_glyph = ensure_contrast(DANGER_RED, t["surface_hi"])
    return (
        f"QWidget#composer {{ background-color: {_container_bg(t['panel'])}; }}"
        f"QFrame#composer_frame {{ background-color: {t['input_bg']}; border: 1px solid {t['border']}; "
        "border-radius: 5px; }"
        f'QFrame#composer_frame[focused="true"] {{ border-color: {t["accent"]}; }}'
        f"QToolButton#composer_chip {{ background: transparent; color: {t['muted']}; "
        "border: 1px solid transparent; border-radius: 4px; padding: 3px 7px; "
        f"font-size: {t['font_small']}; }}"
        f"QToolButton#composer_chip:hover {{ background-color: {t['surface_hi']}; color: {t['text']}; }}"
        f"QLabel#composer_model {{ color: {t['muted']}; font-size: {t['font_small']}; "
        f"background: transparent; font-family: {t['mono']}; }}"
        # The arrow sits on the accent itself, so its color has to be resolved
        # against that fill: the old pairing put accent on a 70% accent ground,
        # which all but vanished on a light host.
        f"QToolButton#composer_send {{ background-color: {t['accent']}; color: {send_glyph}; "
        f"border: 1px solid {t['accent']}; border-radius: 4px; font-size: {t['font_base']}; }}"
        f"QToolButton#composer_send:hover {{ background-color: {t['accent_deep']}; "
        f"color: {ensure_contrast(send_glyph, t['accent_deep'])}; }}"
        f"QToolButton#composer_send:disabled {{ background-color: {t['surface']}; color: {t['faint']}; "
        f"border-color: {t['border']}; }}"
        f"QToolButton#composer_stop {{ background-color: {t['surface_hi']}; color: {stop_glyph}; "
        f"border: 1px solid {stop_glyph}; border-radius: 4px; font-size: {t['font_base']}; }}"
        f"QToolButton#composer_stop:hover {{ background-color: {stop_glyph}; color: {t['panel']}; }}"
    )


def build_context_bar_stylesheet(source=None) -> str:
    """Return the stylesheet for the two-group context bar."""
    t = _extended_tokens(source)
    return (
        f"QFrame#context_bar {{ background-color: {_container_bg(t['panel'])}; "
        f"border-top: 1px solid {t['border_soft']}; }}"
        f"QLabel#context_value {{ color: {t['subtle']}; font-size: {t['font_small']}; "
        f"font-family: {t['mono']}; background: transparent; }}"
        f"QLabel#context_label {{ color: {t['muted']}; font-size: {t['font_small']}; "
        f"font-family: {t['mono']}; background: transparent; }}"
        f"QLabel#context_separator {{ color: {t['border']}; font-size: {t['font_small']}; "
        "background: transparent; }"
        f'QLabel#context_dot[state="idle"] {{ color: {t["accent"]}; font-size: {t["font_small"]}; '
        "background: transparent; }"
        f'QLabel#context_dot[state="running"] {{ color: #d7ba7d; font-size: {t["font_small"]}; '
        "background: transparent; }"
        f'QLabel#context_dot[state="error"] {{ color: {ensure_contrast(DANGER_RED, t["panel"], 3.5)}; '
        f"font-size: {t['font_small']}; "
        "background: transparent; }"
    )

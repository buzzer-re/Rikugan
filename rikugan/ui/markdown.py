"""Markdown to HTML converter targeting Qt's rich-text (QLabel) subset.

Supports the Markdown that LLMs commonly produce:

- Fenced code blocks (``` / ~~~), with optional language tag
- Inline code (`code`)
- Bold (**), italic (*), bold-italic (***), strikethrough (~~)
- Headers (# through ######)
- Bullet lists (-, *, +) and numbered lists (1. / 1)), with nesting
- Task lists (- [ ] / - [x])
- GitHub-flavored tables (with column alignment)
- Blockquotes (>), including nesting
- Links [text](url) and bare-URL autolinking
- Horizontal rules (---, ***, ___)
- Paragraphs (blank-line separated)

No external dependencies. Output targets the HTML subset Qt renders in
QLabel/QTextDocument (which includes tables, blockquote, <s>, nested lists).
"""

from __future__ import annotations

import html
import re

from .styles import blend_theme_color, get_chat_color_tokens

_MARKDOWN_HINT_RE = re.compile(
    r"(^#{1,6}\s)"  # headings
    r"|(^\s*[-*+]\s+)"  # bullet / task list
    r"|(^\s*\d+[.)]\s+)"  # numbered list
    r"|(^\s*>\s?)"  # blockquote
    r"|(\|.*\|)"  # table row
    r"|```|~~~"  # fenced code
    r"|`[^`]+`"  # inline code
    r"|\*\*|__|~~"  # bold / strikethrough
    r"|(?<!\w)\*(.+?)\*(?!\w)"  # italic *
    r"|(?<!\w)_(.+?)_(?!\w)"  # italic _
    r"|\[[^\]]+\]\([^)]+\)"  # link
    r"|https?://"  # bare URL
    r"|^[-*_]{3,}\s*$",  # horizontal rule
    re.MULTILINE,
)

_HEADING_RE = re.compile(r"^(#{1,6})\s+(.*)")
_HR_RE = re.compile(r"^[-*_]{3,}\s*$")
_BLOCK_PLACEHOLDER_RE = re.compile(r"^\x00BLOCK\d+\x00$")
_LIST_ITEM_RE = re.compile(r"^(\s*)([-*+]|\d+[.)])\s+(.*)$")
_TASK_ITEM_RE = re.compile(r"^(\s*)[-*+]\s+\[([ xX])\]\s+(.*)$")
_TASK_TEXT_RE = re.compile(r"^\[([ xX])\]\s+(.*)$")
_BLOCKQUOTE_RE = re.compile(r"^\s*>")
_TABLE_SEP_RE = re.compile(r"^\s*\|?(\s*:?-{1,}:?\s*\|)*\s*:?-{1,}:?\s*\|?\s*$")
_BARE_URL_RE = re.compile(r"(https?://[^\s<>()]+)")
_FENCE_RE = re.compile(r"(?:```|~~~)(\w*)\n(.*?)(?:```|~~~)", re.DOTALL)

_HEADING_SIZES = {1: 18, 2: 16, 3: 14, 4: 13, 5: 12, 6: 11}


def resolve_markdown_theme(source=None) -> dict[str, str]:
    """Public wrapper around the markdown style resolution.

    Callers that render repeatedly (streaming message widgets) resolve this
    once and pass the result to ``md_to_html`` to avoid per-frame palette work.
    """
    return _theme_markdown_styles(source)


def _theme_markdown_styles(source=None) -> dict[str, str]:
    colors = get_chat_color_tokens(source)
    code_bg = colors["code_bg"]
    inline_fg = blend_theme_color(colors["accent"], colors["text"], 0.3)
    border = colors["border"]
    heading = blend_theme_color(colors["accent"], colors["text"], 0.15)
    muted = colors["muted"]
    return {
        "inline_code_style": (
            f"background-color:{code_bg}; color:{inline_fg}; "
            "padding:1px 4px; border-radius:3px; font-family:monospace; font-size:12px;"
        ),
        "block_code_style": (
            f"background-color:{code_bg}; color:{colors['text']}; "
            f"border:1px solid {border}; border-radius:4px; "
            "padding:8px; font-family:monospace; font-size:12px; "
            "white-space:pre-wrap; word-break:break-all;"
        ),
        "link_style": f"color:{colors['accent']};",
        "hr_style": f"border:1px solid {border};",
        "heading_style": f"color:{heading}; font-weight:bold;",
        "lang_tag_style": f"color:{colors['muted']};font-size:10px;",
        "table_style": "border-collapse:collapse; margin:4px 0;",
        "th_style": f"border:1px solid {border}; padding:3px 8px; background-color:{code_bg}; font-weight:bold;",
        "td_style": f"border:1px solid {border}; padding:3px 8px;",
        "blockquote_style": f"border-left:3px solid {colors['accent']}; color:{muted}; padding:2px 10px; margin:4px 0;",
    }


def _has_markdown_syntax(text: str) -> bool:
    """Return True when the input likely needs markdown processing."""
    return bool(text and _MARKDOWN_HINT_RE.search(text))


def md_to_html(text: str, source=None, theme: dict[str, str] | None = None) -> str:
    """Convert a Markdown string to Qt-compatible HTML.

    ``theme`` may be a pre-resolved style dict (see ``_theme_markdown_styles``)
    to skip palette resolution.  Streaming widgets pass a cached theme so the
    per-frame render cost doesn't include re-reading the host palette and
    re-blending colors on every delta.
    """
    if not text:
        return ""
    theme = theme or _theme_markdown_styles(source)
    if not _has_markdown_syntax(text):
        escaped = html.escape(text).replace("\n", "<br>")
        return re.sub(r"(<br>\s*){3,}", "<br><br>", escaped)

    # Phase 1: extract fenced code blocks to protect them from inline processing.
    blocks: list[str] = []

    def _stash_block(m: re.Match) -> str:
        lang = m.group(1) or ""
        code = html.escape(m.group(2).strip("\n"))
        lang_tag = f'<span style="{theme["lang_tag_style"]}">{html.escape(lang)}</span><br>' if lang else ""
        blocks.append(f'<div style="{theme["block_code_style"]}">{lang_tag}{code}</div>')
        return f"\x00BLOCK{len(blocks) - 1}\x00"

    text = _FENCE_RE.sub(_stash_block, text)

    # Phase 2: block-level parsing.
    result = "<br>".join(_render_blocks(text.split("\n"), theme))

    # Phase 3: restore code blocks.
    for idx, block_html in enumerate(blocks):
        result = result.replace(f"\x00BLOCK{idx}\x00", block_html)

    return collapse_breaks(result)


def collapse_breaks(html_text: str) -> str:
    """Normalize runs of ``<br>`` in assembled HTML.

    Streaming renders a message as a series of committed segments, so these runs
    can straddle a segment boundary. Running this over the *joined* HTML — not
    just each segment — is what keeps a streamed message the same height as the
    one-shot render of the same text.
    """
    # Keep at most a blank line between paragraphs.
    result = re.sub(r"(<br>\s*){3,}", "<br><br>", html_text)
    # Block-level elements (lists, tables, blockquotes, code/heading divs, rules)
    # carry their own margins, so collapse <br> runs sitting directly against
    # them to a single newline — otherwise they gain an extra blank line and the
    # layout looks too loose.
    result = re.sub(r"(?:<br>\s*)+(<(?:ul|ol|table|blockquote|hr|div)\b)", r"<br>\1", result)
    result = re.sub(r"(</(?:ul|ol|table|blockquote|div)>)(?:\s*<br>)+", r"\1<br>", result)
    return result


def _render_blocks(lines: list[str], theme: dict[str, str]) -> list[str]:
    """Render a list of source lines into a list of block-level HTML strings."""
    out: list[str] = []
    i = 0
    n = len(lines)
    while i < n:
        line = lines[i]
        stripped = line.strip()

        if _BLOCK_PLACEHOLDER_RE.match(stripped):
            out.append(stripped)
            i += 1
        elif not stripped:
            out.append("<br>")
            i += 1
        elif _HR_RE.match(stripped):
            hr_style = f' style="{theme["hr_style"]}"' if theme["hr_style"] else ""
            out.append(f"<hr{hr_style}>")
            i += 1
        elif _HEADING_RE.match(stripped):
            out.append(_render_heading(stripped, theme))
            i += 1
        elif "|" in line and i + 1 < n and _is_table_separator(lines[i + 1]):
            html_str, i = _parse_table(lines, i, theme)
            out.append(html_str)
        elif _BLOCKQUOTE_RE.match(line):
            html_str, i = _parse_blockquote(lines, i, theme)
            out.append(html_str)
        elif _TASK_ITEM_RE.match(line):
            html_str, i = _parse_task_list(lines, i, theme)
            out.append(html_str)
        elif _LIST_ITEM_RE.match(line):
            html_str, i = _parse_list(lines, i, theme)
            out.append(html_str)
        else:
            out.append(_inline(stripped, theme))
            i += 1
    return out


def _render_heading(stripped: str, theme: dict[str, str]) -> str:
    m = _HEADING_RE.match(stripped)
    assert m is not None
    level = len(m.group(1))
    size = _HEADING_SIZES.get(level, 13)
    return (
        f'<div style="{theme["heading_style"]}font-size:{size}px;margin:6px 0 2px 0;">'
        f"{_inline(m.group(2), theme)}</div>"
    )


# ---------------------------------------------------------------------------
# Tables
# ---------------------------------------------------------------------------


def _is_table_separator(line: str) -> bool:
    return "-" in line and bool(_TABLE_SEP_RE.match(line))


def _split_table_row(line: str) -> list[str]:
    s = line.strip()
    if s.startswith("|"):
        s = s[1:]
    if s.endswith("|"):
        s = s[:-1]
    return [cell.strip() for cell in s.split("|")]


def _parse_alignments(sep_line: str) -> list[str]:
    aligns: list[str] = []
    for cell in _split_table_row(sep_line):
        cell = cell.strip()
        left = cell.startswith(":")
        right = cell.endswith(":")
        if left and right:
            aligns.append("center")
        elif right:
            aligns.append("right")
        elif left:
            aligns.append("left")
        else:
            aligns.append("")
    return aligns


def _parse_table(lines: list[str], start: int, theme: dict[str, str]) -> tuple[str, int]:
    header = _split_table_row(lines[start])
    aligns = _parse_alignments(lines[start + 1])
    cols = len(header)

    i = start + 2
    rows: list[list[str]] = []
    while i < len(lines) and lines[i].strip() and "|" in lines[i]:
        rows.append(_split_table_row(lines[i]))
        i += 1

    def _align_attr(idx: int) -> str:
        a = aligns[idx] if idx < len(aligns) else ""
        return f' align="{a}"' if a else ""

    parts = [f'<table border="1" cellspacing="0" style="{theme["table_style"]}"><tr>']
    for idx, cell in enumerate(header):
        parts.append(f'<th{_align_attr(idx)} style="{theme["th_style"]}">{_inline(cell, theme)}</th>')
    parts.append("</tr>")
    for row in rows:
        parts.append("<tr>")
        for idx in range(cols):
            cell = row[idx] if idx < len(row) else ""
            parts.append(f'<td{_align_attr(idx)} style="{theme["td_style"]}">{_inline(cell, theme)}</td>')
        parts.append("</tr>")
    parts.append("</table>")
    return "".join(parts), i


# ---------------------------------------------------------------------------
# Blockquotes
# ---------------------------------------------------------------------------


def _parse_blockquote(lines: list[str], start: int, theme: dict[str, str]) -> tuple[str, int]:
    inner: list[str] = []
    i = start
    while i < len(lines) and _BLOCKQUOTE_RE.match(lines[i]):
        inner.append(re.sub(r"^\s*>\s?", "", lines[i]))
        i += 1
    inner_html = "<br>".join(_render_blocks(inner, theme))
    return f'<blockquote style="{theme["blockquote_style"]}">{inner_html}</blockquote>', i


# ---------------------------------------------------------------------------
# Lists
# ---------------------------------------------------------------------------


def _parse_task_list(lines: list[str], start: int, theme: dict[str, str]) -> tuple[str, int]:
    rows: list[str] = []
    i = start
    while i < len(lines):
        m = _TASK_ITEM_RE.match(lines[i])
        if not m:
            break
        indent = len(m.group(1).expandtabs(4))
        box = "☑" if m.group(2).lower() == "x" else "☐"  # checked / unchecked box
        pad = "&nbsp;&nbsp;" * (indent // 2)
        rows.append(f'<div style="margin:1px 0 1px 4px;">{pad}{box} {_inline(m.group(3), theme)}</div>')
        i += 1
    return "".join(rows), i


def _parse_list(lines: list[str], start: int, theme: dict[str, str]) -> tuple[str, int]:
    items: list[tuple[int, bool, str]] = []
    i = start
    while i < len(lines):
        m = _LIST_ITEM_RE.match(lines[i])
        if not m:
            break
        indent = len(m.group(1).expandtabs(4))
        ordered = m.group(2)[0].isdigit()
        items.append((indent, ordered, m.group(3)))
        i += 1
    return _build_nested_list(items, theme), i


def _build_nested_list(items: list[tuple[int, bool, str]], theme: dict[str, str]) -> str:
    if not items:
        return ""

    def build(pos: int, base_indent: int) -> tuple[str, int]:
        tag = "ol" if items[pos][1] else "ul"
        html_parts = [f"<{tag} style='margin:2px 0 2px 18px;'>"]
        while pos < len(items):
            indent, _ordered, text = items[pos]
            if indent < base_indent:
                break
            if indent > base_indent:
                nested, pos = build(pos, indent)
                # Nest under the previous sibling <li> when there is one.
                if html_parts[-1].endswith("</li>"):
                    html_parts[-1] = html_parts[-1][:-5] + nested + "</li>"
                else:
                    html_parts.append(f"<li>{nested}</li>")
                continue
            html_parts.append(f"<li>{_render_list_item(text, theme)}</li>")
            pos += 1
        html_parts.append(f"</{tag}>")
        return "".join(html_parts), pos

    out, _ = build(0, items[0][0])
    return out


def _render_list_item(text: str, theme: dict[str, str]) -> str:
    """Render list-item text, mapping inline task markers to a checkbox glyph."""
    m = _TASK_TEXT_RE.match(text)
    if m:
        box = "☑" if m.group(1).lower() == "x" else "☐"  # checked / unchecked box
        return f"{box} {_inline(m.group(2), theme)}"
    return _inline(text, theme)


# ---------------------------------------------------------------------------
# Inline formatting
# ---------------------------------------------------------------------------


def _inline(text: str, theme: dict[str, str] | None = None) -> str:
    """Apply inline Markdown formatting to a span of text."""
    theme = theme or _theme_markdown_styles()
    text = html.escape(text)

    # Stash inline code spans so bold/italic/links don't mangle their contents.
    code_spans: list[str] = []

    def _stash_code(m: re.Match) -> str:
        code_spans.append(f'<span style="{theme["inline_code_style"]}">{m.group(1)}</span>')
        return f"\x01CODE{len(code_spans) - 1}\x01"

    text = re.sub(r"`([^`]+)`", _stash_code, text)

    text = _inline_formatting(text, theme["link_style"])
    text = _autolink(text, theme["link_style"])

    for idx, span_html in enumerate(code_spans):
        text = text.replace(f"\x01CODE{idx}\x01", span_html)

    return text


def _inline_formatting(text: str, link_style: str | None = None) -> str:
    """Apply bold, italic, strikethrough, and link formatting."""
    link_style = link_style or _theme_markdown_styles()["link_style"]

    # Bold-italic first so the greedier bold/italic passes don't claim the runs.
    text = re.sub(r"\*\*\*(.+?)\*\*\*", r"<b><i>\1</i></b>", text)
    # Bold: **text** or __text__
    text = re.sub(r"\*\*(.+?)\*\*", r"<b>\1</b>", text)
    text = re.sub(r"__(.+?)__", r"<b>\1</b>", text)
    # Strikethrough: ~~text~~
    text = re.sub(r"~~(.+?)~~", r"<s>\1</s>", text)
    # Italic: *text* or _text_ (not mid-word for underscore)
    text = re.sub(r"(?<!\w)\*(.+?)\*(?!\w)", r"<i>\1</i>", text)
    text = re.sub(r"(?<!\w)_(.+?)_(?!\w)", r"<i>\1</i>", text)
    # Links: [text](url)
    text = re.sub(
        r"\[([^\]]+)\]\(([^)]+)\)",
        rf'<a style="{link_style}" href="\2">\1</a>',
        text,
    )
    return text


def _autolink(text: str, link_style: str | None = None) -> str:
    """Turn bare http(s) URLs into links, leaving existing <a> tags untouched."""
    if "://" not in text:
        return text
    link_style = link_style or _theme_markdown_styles()["link_style"]
    # Split out existing anchors so we don't re-link their href/text.
    parts = re.split(r"(<a\b[^>]*>.*?</a>)", text)
    for idx in range(0, len(parts), 2):
        parts[idx] = _BARE_URL_RE.sub(
            rf'<a style="{link_style}" href="\1">\1</a>',
            parts[idx],
        )
    return "".join(parts)
